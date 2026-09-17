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
 * stm32 implementation of the debug engine's port interface.
 */

#include <string.h>

// USB headers must come before py/* : usbd_def.h defines MIN/MAX and py/misc.h
// redefines them, which is -Werror under this build.  usb.c uses the same order.
#include "usbd_core.h"
#include "usbd_cdc_msc_hid.h"
#include "usbd_cdc_interface.h"

#include "py/mphal.h"
#include "usb.h"
#include "boardctrl.h"
#include "storage.h"
#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/mpdebug_port.h"

// Defined in usb.c -- usb_device is file-static there.
usbd_cdc_itf_t *usb_vcp_get_cdc_itf(int idx);

static usbd_cdc_itf_t *mp_debug_port_cdc(void) {
    return usb_vcp_get_cdc_itf(MP_DEBUG_CDC_INDEX);
}

bool mp_debug_port_link_up(void) {
    return mp_debug_port_cdc() != NULL;
}

int mp_debug_port_rx_avail(void) {
    usbd_cdc_itf_t *cdc = mp_debug_port_cdc();
    return cdc == NULL ? 0 : usbd_cdc_rx_num(cdc);
}

int mp_debug_port_rx(uint8_t *buf, size_t len) {
    usbd_cdc_itf_t *cdc = mp_debug_port_cdc();
    if (cdc == NULL) {
        return 0;
    }
    return usbd_cdc_rx(cdc, buf, len, 0);
}

void mp_debug_port_tx_always(const uint8_t *buf, size_t len) {
    usbd_cdc_itf_t *cdc = mp_debug_port_cdc();
    if (cdc == NULL) {
        return;
    }
    // usbd_cdc_tx_always blocks rather than short-writing, so the 256-byte CDC
    // TX buffer is not a concern at this layer.
    usbd_cdc_tx_always(cdc, buf, len);
}

bool mp_debug_port_tx_half_empty(void) {
    usbd_cdc_itf_t *cdc = mp_debug_port_cdc();
    return cdc != NULL && usbd_cdc_tx_half_empty(cdc);
}

int mp_debug_port_repl_getc(void) {
    usbd_cdc_itf_t *repl = usb_vcp_get_cdc_itf(0);
    if (repl == NULL || usbd_cdc_rx_num(repl) <= 0) {
        return -1;
    }
    uint8_t c;
    if (usbd_cdc_rx(repl, &c, 1, 0) != 1) {
        return -1;
    }
    return c;
}

// A hard reset re-zeroes .bss, so the stop-on-start request cannot simply live
// there if it has to survive one.  Stash it in an RTC backup register instead:
// those sit in the backup domain and are cleared only by a backup-domain reset
// or loss of VBAT, not by a system reset.  Tagged so an unrelated value cannot
// be mistaken for a request.
#define MP_DBG_PERSIST_TAG   (0x4D504400u)   // "MPD" << 8
#define MP_DBG_PERSIST_MASK  (0xFFFFFF00u)
#define MP_DBG_PERSIST_REG   (RTC->BKP31R)

static void mp_debug_port_persist_enable(void) {
    // The backup registers need the RTC APB clock and the backup-domain write
    // protection lifted.  Neither requires the RTC itself to be running, which
    // matters when a board keeps MICROPY_HW_ENABLE_RTC = 0.  The macro name
    // for "APB clock only, not the counter" differs between STM32 families:
    // L4 has RTCAPB, H7 rolls it into RTC_CLK (sets RTCAPBEN in APB4ENR).
    #if defined(__HAL_RCC_RTCAPB_CLK_ENABLE)
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    #else
    __HAL_RCC_RTC_CLK_ENABLE();
    #endif
    HAL_PWR_EnableBkUpAccess();
}

void mp_debug_port_persist_write(uint32_t conditions) {
    mp_debug_port_persist_enable();
    MP_DBG_PERSIST_REG = MP_DBG_PERSIST_TAG | (conditions & 0xFFu);
}

uint32_t mp_debug_port_persist_take(void) {
    mp_debug_port_persist_enable();
    uint32_t v = MP_DBG_PERSIST_REG;
    if ((v & MP_DBG_PERSIST_MASK) != MP_DBG_PERSIST_TAG) {
        return 0;
    }
    MP_DBG_PERSIST_REG = 0;      // one-shot
    return v & 0xFFu;
}

void mp_debug_port_storage_flush(void) {
    storage_flush();
}

void mp_debug_port_reset(void) {
    // Detach USB before resetting.  NVIC_SystemReset() alone leaves the D+
    // pull-up asserted through the reset, so the host never sees a disconnect:
    // it keeps its old device object, which is then out of sync with the freshly
    // booted device.  On Windows the port still enumerates and reports OK, but
    // every open fails with "a device attached to the system is not functioning"
    // until the board is physically replugged.
    pyb_usb_dev_deinit();
    mp_hal_delay_us(100000);

    NVIC_SystemReset();
}

// Board hook replacing boardctrl_run_main_py.  Halts before the first bytecode
// of main.py when the host has asked for it.
// The three mp_debug_*_main_py calls are the port-neutral part; everything else
// here is stm32's boardctrl shape and this board's pin naming.
int mp_debug_run_main_py(struct _boardctrl_state_t *state) {
    // By now USB is up, so this reaches the REPL and the Debug Console; the
    // pin is read far earlier than that, where nothing could be printed.
    if (((boardctrl_state_t *)state)->reset_mode == BOARDCTRL_RESET_MODE_SAFE_MODE) {
        const char *msg = "RUN_APP pin is low: boot.py and main.py were skipped.\n";
        mp_hal_stdout_tx_strn(msg, strlen(msg));
    }

    mp_debug_before_main_py();

    if (mp_debug_take_soft_reset()) {
        return BOARDCTRL_GOTO_SOFT_RESET_EXIT;
    }

    int ret = boardctrl_run_main_py((boardctrl_state_t *)state);
    mp_debug_after_main_py();
    return ret;
}
