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

#ifndef MICROPY_INCLUDED_MPDEBUG_MPDEBUG_H
#define MICROPY_INCLUDED_MPDEBUG_MPDEBUG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "shared/mpdebug/wireprotocol.h"

// Which CDC interface carries the debug channel.  CDC0 stays the REPL.
#ifndef MP_DEBUG_CDC_INDEX
#define MP_DEBUG_CDC_INDEX (1)
#endif

// The "debugger armed" flag, tested on the VM hot path.  One load and one
// compare per loop, with the expensive work kept behind it.
extern volatile bool mp_debug_armed;

// Debugger condition bits (MP_DBG_COND_*).  Lives in .bss, which the stm32 soft
// reset loop does NOT re-zero -- the same property pyb_usb_flags relies on --
// so MP_DBG_COND_STOP_ON_START survives a soft reset and takes effect on the
// next run of main.py.
extern volatile uint32_t mp_debug_conditions;
extern volatile bool mp_debug_in_eval;

// The three phases a port wraps in whatever "run main.py" hook it has.  Between
// before/after the port runs its own main.py; a true from take_soft_reset means
// skip that and go to the port's soft-reset exit instead.
void mp_debug_before_main_py(void);
bool mp_debug_take_soft_reset(void);
void mp_debug_after_main_py(void);

int mp_debug_file_dispatch(uint32_t cmd, const uint8_t *payload, uint32_t size,
    uint8_t *reply_buf, uint32_t reply_max);

// Ends any File_Put transfer still open and pushes it to flash.
// Must run before either kind of reset: a file left open would lose its
// directory entry and the board would keep running the previous code.
void mp_debug_file_finish_put(void);

struct _mp_code_state_t;
void mp_debug_instr_tick(struct _mp_code_state_t *code_state);
void mp_debug_send_stopped(uint32_t reason, uint32_t index, uint32_t line, const char *file);
// Stop ownership.  With more than one thread every thread halts together, so
// one must own the stop and the others park silently.  See 12.3a.
bool mp_debug_claim_halt(void);
void mp_debug_release_halt(void);
void mp_debug_park(void);

void mp_debug_stop(uint32_t reason, uint32_t index, uint32_t line, const char *file);
void mp_debug_update_armed(void);
void mp_debug_stdout(const char *str, size_t len);
void mp_debug_halt_loop(void);
int32_t mp_debug_breakpoints_set(const uint8_t *payload, uint32_t size);
uint32_t mp_debug_breakpoints_active(void);
int mp_debug_stack_serialise(uint8_t *buf, uint32_t buf_max);
struct _mp_code_state_t;
int mp_debug_vars_serialise(const struct _mp_code_state_t *cs, uint32_t scope,
    uint32_t start, uint8_t *buf, uint32_t buf_max);
int mp_debug_children_serialise(uint32_t handle, uint32_t start,
    uint8_t *buf, uint32_t buf_max);
void mp_debug_handles_reset(void);
const struct _mp_code_state_t *mp_debug_frame_at(uint32_t index);
void mp_debug_set_hit_frame(const struct _mp_code_state_t *cs);
void mp_debug_exception(const struct _mp_code_state_t *cs, void *exc);
int mp_debug_eval(const struct _mp_code_state_t *cs, const char *expr,
    uint32_t expr_len, uint8_t *buf, uint32_t buf_max);
int mp_debug_set_variable(const struct _mp_code_state_t *cs,
    const char *name, uint32_t name_len,
    const char *expr, uint32_t expr_len,
    uint8_t *buf, uint32_t buf_max);
int32_t mp_debug_step_begin(uint32_t mode);
uint32_t mp_debug_step_active(void);

// Called from the VM per-instruction hook.  Must stay a single flag test when
// the debugger is not armed -- this is the hot path.
#define MP_DEBUG_INSTR_HOOK(cs) do { if (mp_debug_armed) { mp_debug_instr_tick(cs); } } while (0)

void mp_debug_init(void);
void mp_debug_poll(void);

// Placed in MICROPY_VM_HOOK_LOOP.  Must stay this cheap.
#define MP_DEBUG_VM_HOOK() do { if (mp_debug_armed) { mp_debug_poll(); } } while (0)

#endif // MICROPY_INCLUDED_MPDEBUG_MPDEBUG_H
