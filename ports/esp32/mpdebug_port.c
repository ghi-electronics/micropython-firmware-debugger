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
 * esp32 implementation of the debug engine's port interface
 * (shared/mpdebug/mpdebug_port.h).  The engine itself is port-neutral;
 * these ten functions are everything it needs from the chip.
 *
 * Two transports are available at compile time (selected in
 * mpdebug_board.h via MICROPY_HW_MPDEBUG_TRANSPORT):
 *
 *   USB CDC (S2, S3, P4) -- second CDC interface on the USB OTG peripheral,
 *                           CDC 1 carries the debug protocol.
 *   UART    (original ESP32 without native USB) -- steals UART0 (or a
 *                           board-picked UART) so the same USB cable that
 *                           reaches the onboard USB-to-serial bridge carries
 *                           the debug protocol.
 *
 * The rest of the port interface (REPL Ctrl-C interception, RTC persist,
 * storage flush, esp_restart) is transport-neutral and shared.
 */

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"

#include "esp_system.h"
#include "esp_attr.h"

#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/mpdebug_port.h"

// ---------------------------------------------------------- debug channel ---

#if MICROPY_HW_MPDEBUG_TRANSPORT == MICROPY_HW_MPDEBUG_TRANSPORT_USB

#include "tusb.h"
#include "shared/tinyusb/mp_usbd.h"

bool mp_debug_port_link_up(void) {
    // Do not touch the USB stack before it exists. Our hooks run from the VM and
    // idle paths, which are live long before mp_usbd_init() -- driving tud_task()
    // on an uninitialised stack faults, and a board that faults this early simply
    // never appears on USB at all: no port, no output, no error to inspect.
    if (!tusb_inited()) {
        return false;
    }
    // Drive the USB stack first, exactly as on rp2 and for the same reason:
    // tinyusb does its work in tud_task(), its interrupt only queues events, and
    // this is the engine's outermost check -- mp_debug_pump() returns early when
    // it is false, so it is the only place a pump is guaranteed to run. Put it
    // any deeper and a board that halts before enumeration finishes deadlocks.
    mp_usbd_task();
    return tud_mounted();
}

int mp_debug_port_rx_avail(void) {
    return (int)tud_cdc_n_available(MP_DEBUG_CDC_INDEX);
}

int mp_debug_port_rx(uint8_t *buf, size_t len) {
    return (int)tud_cdc_n_read(MP_DEBUG_CDC_INDEX, buf, (uint32_t)len);
}

void mp_debug_port_tx_always(const uint8_t *buf, size_t len) {
    // Must not short-write: the framing layer above assumes a packet leaves
    // whole, so pump the stack while waiting for the host to drain the FIFO.
    while (len > 0) {
        if (!tud_cdc_n_connected(MP_DEBUG_CDC_INDEX)) {
            // Nobody has the port open. Drop rather than spin: this runs from
            // the VM hook, and hanging a program because no debugger is
            // attached would be far worse than losing the packet.
            return;
        }
        uint32_t n = tud_cdc_n_write(MP_DEBUG_CDC_INDEX, buf, (uint32_t)len);
        buf += n;
        len -= n;
        tud_cdc_n_write_flush(MP_DEBUG_CDC_INDEX);
        if (n == 0) {
            mp_usbd_task();
        }
    }
}

bool mp_debug_port_tx_half_empty(void) {
    return tud_cdc_n_write_available(MP_DEBUG_CDC_INDEX) >= (CFG_TUD_CDC_TX_BUFSIZE / 2);
}

#elif MICROPY_HW_MPDEBUG_TRANSPORT == MICROPY_HW_MPDEBUG_TRANSPORT_UART

// UART transport -- for original ESP32 chips whose only path to the host is
// a USB-to-serial bridge wired to UART0.  Baud, UART number and buffer sizes
// are picked in the board's mpconfigboard.h; sensible defaults follow.
#include "driver/uart.h"
#include "hal/uart_types.h"

#ifndef MICROPY_HW_MPDEBUG_UART_NUM
#define MICROPY_HW_MPDEBUG_UART_NUM             (UART_NUM_0)
#endif
#ifndef MICROPY_HW_MPDEBUG_UART_BAUD
#define MICROPY_HW_MPDEBUG_UART_BAUD            (115200)
#endif
// The driver's ring buffers.  The RX buffer must be at least twice the UART
// hardware FIFO, and both need enough room for a maximal MPYDBG1 frame plus a
// bit of slack so the engine isn't back-pressured on every packet.
#ifndef MICROPY_HW_MPDEBUG_UART_RX_BUF
#define MICROPY_HW_MPDEBUG_UART_RX_BUF          (1024)
#endif
#ifndef MICROPY_HW_MPDEBUG_UART_TX_BUF
#define MICROPY_HW_MPDEBUG_UART_TX_BUF          (1024)
#endif

// Set true once uart_driver_install() has returned successfully.  Guards every
// hot-path call so the engine's hooks -- which start firing before user code
// runs -- do not touch an uninstalled driver.
static bool s_uart_ready = false;

// Lazily install the ESP-IDF UART driver on the first pump.  Idempotent so it
// is safe to call from every mp_debug_port_link_up().  Doing this from the
// engine's hot path (rather than a MICROPY_BOARD_EARLY_INIT) means we do not
// need any board-side plumbing; every UART-transport board benefits the moment
// it flips MICROPY_HW_MPDEBUG_TRANSPORT to _UART.
static void uart_lazy_init(void) {
    uart_config_t cfg = {
        .baud_rate = MICROPY_HW_MPDEBUG_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // Any UART driver left over from bootloader / IDF stdio is torn down first
    // so uart_driver_install() sees a clean peripheral.  A double-install
    // returns ESP_ERR_INVALID_STATE and we would silently run without a driver.
    uart_driver_delete(MICROPY_HW_MPDEBUG_UART_NUM);
    esp_err_t err = uart_driver_install(MICROPY_HW_MPDEBUG_UART_NUM,
                                        MICROPY_HW_MPDEBUG_UART_RX_BUF,
                                        MICROPY_HW_MPDEBUG_UART_TX_BUF,
                                        0, NULL, 0);
    if (err != ESP_OK) {
        return;
    }
    (void)uart_param_config(MICROPY_HW_MPDEBUG_UART_NUM, &cfg);
    // Pin defaults come from the SoC's default UART routing; the boards this
    // transport targets wire the onboard USB bridge to those defaults, so we
    // deliberately do not call uart_set_pin() here.
    s_uart_ready = true;
}

bool mp_debug_port_link_up(void) {
    if (!s_uart_ready) {
        uart_lazy_init();
    }
    // No enumeration to wait for on UART -- the host is either listening or
    // not, but the transport itself is "up" the moment the driver is installed.
    // If the host is not there yet, bytes we write sit in the driver's TX ring
    // and the OS/bridge chip forwards them once the port is opened.
    return s_uart_ready;
}

int mp_debug_port_rx_avail(void) {
    if (!s_uart_ready) {
        return 0;
    }
    size_t avail = 0;
    (void)uart_get_buffered_data_len(MICROPY_HW_MPDEBUG_UART_NUM, &avail);
    return (int)avail;
}

int mp_debug_port_rx(uint8_t *buf, size_t len) {
    if (!s_uart_ready) {
        return 0;
    }
    // Non-blocking read: the engine polls in the VM/idle hook and expects to
    // see whatever is immediately available.  A blocking read here would stall
    // user code every VM tick when no debugger is attached.
    int n = uart_read_bytes(MICROPY_HW_MPDEBUG_UART_NUM, buf, len, 0);
    return n < 0 ? 0 : n;
}

void mp_debug_port_tx_always(const uint8_t *buf, size_t len) {
    if (!s_uart_ready) {
        return;
    }
    // uart_write_bytes queues into the driver's TX ring; if it fills, the
    // call blocks until space frees up.  The ring is sized (TX_BUF above) to
    // hold a whole MPYDBG1 packet with headroom so the common case is
    // wait-free.  Unlike USB CDC there is no "host disconnected" signal to
    // short-circuit on -- the bridge chip stays online across resets --
    // which is fine: if the host isn't listening, bytes are simply dropped
    // by the OS-side USB driver, not by us.
    (void)uart_write_bytes(MICROPY_HW_MPDEBUG_UART_NUM, buf, len);
}

bool mp_debug_port_tx_half_empty(void) {
    if (!s_uart_ready) {
        return false;
    }
    // No public API to query TX ring occupancy on ESP-IDF, so report "always
    // half-empty".  The engine uses this to decide whether to send another
    // packet immediately or come back on the next hook -- being optimistic
    // here just means uart_write_bytes may block briefly if the ring fills,
    // which we accept for now.  Revisit if this becomes a throughput
    // bottleneck.
    return true;
}

#else
#error "MICROPY_HW_MPDEBUG_TRANSPORT must be MICROPY_HW_MPDEBUG_TRANSPORT_USB or _UART"
#endif

// ----------------------------------------------------------- REPL channel ---

int mp_debug_port_repl_getc(void) {
    // Ctrl-C never reaches us as a byte: shared/tinyusb/mp_usbd_cdc.c turns it
    // into a scheduled KeyboardInterrupt before it lands anywhere readable.
    // Report that specific pending exception as the 0x03 the halt loop looks
    // for, and consume it, or the program dies of it the moment execution
    // resumes.
    //
    // Match ONLY the KeyboardInterrupt instance -- any other async exception
    // (RMT/timer/USB ISR paths, neopixel driver's scheduled callbacks) would
    // otherwise trip the halt-loop's Ctrl-C check and release the halt
    // spuriously, which is what caused rgb-blink to run past its breakpoint
    // while the debugger UI showed "stopped" (see the second-hit bug
    // investigated 2026-09-11).
    mp_obj_t pending = MP_STATE_THREAD(mp_pending_exception);
    if (pending == MP_OBJ_FROM_PTR(&MP_STATE_VM(mp_kbd_exception))) {
        MP_STATE_THREAD(mp_pending_exception) = MP_OBJ_NULL;
        return 0x03;
    }
    return -1;
}

// ------------------------------------------------- state outliving a reset ---

// RTC_NOINIT_ATTR is RTC slow memory that the startup code does not clear, so
// it survives esp_restart().  Its contents are undefined at cold boot, hence the
// tag.
//
// A private symbol, deliberately: machine_rtc.c hands a region of RTC memory to
// user code as machine.RTC.memory(), and sharing it would corrupt a documented
// feature.
#define MP_DBG_PERSIST_TAG  (0x4D504400u)   // "MPD" << 8
#define MP_DBG_PERSIST_MASK (0xFFFFFF00u)

static RTC_NOINIT_ATTR uint32_t mp_debug_persist_word;

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
    // Nothing to do: the esp32 VFS commits on close, which
    // mp_debug_file_finish_put() has already done before this is called.
}

void mp_debug_port_reset(void) {
    // No tud_disconnect() here: the reset resets the USB block and releases
    // the pull-up by itself, and detaching first can leave the host never
    // seeing the board come back.
    //
    // The engine has already waited for the reply to reach the host.
    esp_restart();
    for (;;) {
    }
}
