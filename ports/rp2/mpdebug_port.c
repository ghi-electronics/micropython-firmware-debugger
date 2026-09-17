/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 GHI Electronics
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * rp2 implementation of the debug engine's port interface
 * (shared/mpdebug/mpdebug_port.h).  The engine itself is port-neutral; these
 * ten functions are everything it needs from the chip.
 */

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"

#include "tusb.h"
#include "pico/platform/sections.h"
#include "hardware/watchdog.h"
#include "hardware/regs/addressmap.h"

#include "shared/tinyusb/mp_usbd.h"
#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/mpdebug_port.h"

// ---------------------------------------------------------- debug channel ---

bool mp_debug_port_link_up(void) {
    // Do not touch the USB stack before it exists. Our hooks run from the VM and
    // idle paths, which are live long before mp_usbd_init() -- driving tud_task()
    // on an uninitialised stack faults, and a board that faults this early simply
    // never appears on USB at all: no port, no output, no error to inspect.
    if (!tusb_inited()) {
        return false;
    }
    // Drive the USB stack FIRST, before answering.
    //
    // This is the engine's outermost check -- mp_debug_pump() returns early when
    // it is false -- so it is the only place a pump is guaranteed to run.  Put it
    // any deeper (in rx_avail, say) and a board that halts before enumeration has
    // finished deadlocks: completing enumeration needs tud_task(), but tud_task()
    // would only be reached once already enumerated.  The board then sits
    // attached but unanswering, which the host reports as VID_0000 "Device
    // Descriptor Request Failed" -- indistinguishable from a dead board.
    //
    // stm32 needs none of this: there, enumeration is interrupt-driven and a halt
    // loop can simply spin.  tinyusb does that work in tud_task(); its interrupt
    // only queues events.  Any loop that does not unwind -- and the halt loop is
    // exactly that, by design -- must pump the stack itself.
    mp_usbd_task();

    // "The channel exists", not "a host has opened it" -- matching stm32, where
    // this asked whether the CDC interface was present.  Whether anyone is
    // listening is a transmit-side question, handled below.
    return tud_mounted();
}

int mp_debug_port_rx_avail(void) {
    // No pump needed here: link_up() runs first on every path into this file and
    // has already driven the stack.
    return (int)tud_cdc_n_available(MP_DEBUG_CDC_INDEX);
}

int mp_debug_port_rx(uint8_t *buf, size_t len) {
    return (int)tud_cdc_n_read(MP_DEBUG_CDC_INDEX, buf, (uint32_t)len);
}

void mp_debug_port_tx_always(const uint8_t *buf, size_t len) {
    // Must not short-write: the framing layer above assumes a packet leaves
    // whole.  tinyusb's FIFO is smaller than our largest packet, so drive the
    // stack while waiting for the host to drain it.  stm32 got this for free
    // from usbd_cdc_tx_always().
    while (len > 0) {
        if (!tud_cdc_n_connected(MP_DEBUG_CDC_INDEX)) {
            // Nobody has the port open.  Drop rather than spin here forever:
            // this runs from the VM hook, and hanging the program because no
            // debugger is attached would be far worse than losing the packet.
            return;
        }
        uint32_t n = tud_cdc_n_write(MP_DEBUG_CDC_INDEX, buf, (uint32_t)len);
        buf += n;
        len -= n;
        tud_cdc_n_write_flush(MP_DEBUG_CDC_INDEX);
        if (n == 0) {
            // FIFO full: let the stack move data before trying again.
            mp_usbd_task();
        }
    }
}

bool mp_debug_port_tx_half_empty(void) {
    return tud_cdc_n_write_available(MP_DEBUG_CDC_INDEX) >= (CFG_TUD_CDC_TX_BUFSIZE / 2);
}

// ----------------------------------------------------------- REPL channel ---

int mp_debug_port_repl_getc(void) {
    // Ctrl-C never reaches us as a byte on this port: shared/tinyusb/mp_usbd_cdc.c
    // turns it into a scheduled KeyboardInterrupt before it lands anywhere
    // readable.  Report that pending exception as the 0x03 the halt loop is
    // looking for, and consume it -- otherwise the program would also die of it
    // the moment execution resumed.
    if (MP_STATE_THREAD(mp_pending_exception) != MP_OBJ_NULL) {
        MP_STATE_THREAD(mp_pending_exception) = MP_OBJ_NULL;
        return 0x03;
    }
    return -1;
}

// ------------------------------------------------- state outliving a reset ---

// `.uninitialized_data` is a NOLOAD section, so crt0 never clears it and the
// value survives watchdog_reboot().  Its contents are undefined at cold boot,
// hence the tag.
//
// Deliberately NOT the watchdog scratch registers: machine_mem_backup.c hands
// scratch[0..3], scratch[5..7] and the powman scratch to user code as
// machine.mem_backup, and scratch[4] belongs to pico-sdk.  Taking any of them
// would silently corrupt a documented feature.
#define MP_DBG_PERSIST_TAG  (0x4D504400u)   // "MPD" << 8
#define MP_DBG_PERSIST_MASK (0xFFFFFF00u)

static uint32_t __uninitialized_ram(mp_debug_persist_word);

void mp_debug_port_persist_write(uint32_t conditions) {
    mp_debug_persist_word = MP_DBG_PERSIST_TAG | (conditions & 0xFFu);
}

uint32_t mp_debug_port_persist_take(void) {
    uint32_t v = mp_debug_persist_word;
    if ((v & MP_DBG_PERSIST_MASK) != MP_DBG_PERSIST_TAG) {
        return 0;
    }
    mp_debug_persist_word = 0;      // one-shot
    return v & 0xFFu;
}

// ------------------------------------------------------- storage and reset ---

void mp_debug_port_storage_flush(void) {
    // Nothing to do here.  rp2 has no equivalent of stm32's flashbdev write
    // cache: the VFS (littlefs or FAT over rp2_flash) commits on close, which
    // mp_debug_file_finish_put() has already done before this is called.
}

void mp_debug_port_reset(void) {
    // Deliberately NO tud_disconnect() here, unlike stm32.  Measured on a Pico 2
    // 2026-09-03: this is the exact mirror of the stm32 problem in 11.5 item 2.
    //
    //   stm32  - the reset leaves the D+ pull-up asserted, so the host never sees
    //            a disconnect and keeps a stale device.  pyb_usb_dev_deinit() is
    //            required.
    //   RP2350 - a tud_disconnect() before the reset leaves the pull-up
    //            deasserted across it, so the host never sees the board come
    //            back at all.  The device is gone until it is physically
    //            replugged.
    //
    // watchdog_reboot() resets the USB block itself, which releases the pull-up
    // and gives the host a clean disconnect with no help from us.  Proven by
    // A/B against machine.reset(), which is this same call without the detach:
    // that re-enumerates reliably, including through a hub, where ours did not.
    //
    // The engine has already waited for the reply to reach the host before
    // calling this, so there is nothing left to flush.
    watchdog_reboot(0, SRAM_END, 0);
    for (;;) {
    }
}
