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
 * Breakpoints.
 *
 * The check runs in C on the VM's per-instruction hook, gated on mp_debug_armed.
 * A Python-level should_stop() on this path costs orders of magnitude more; here
 * the not-debugging cost is one flag test, and the debugging cost is a line
 * lookup plus a walk of a table that is normally empty or tiny.
 *
 * MICROPY_PY_SYS_SETTRACE is enabled for its metadata and its hook site only --
 * the Python-level trace callback is never used.
 */
#include <string.h>

#include "py/runtime.h"
#include "py/bc.h"
#include "py/mpprint.h"
#include "py/profile.h"
#include "py/objcode.h"
#include "py/objfun.h"    // mp_obj_fun_bc_t, for the code_state chain

#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/micropython_debugging.h"

typedef struct _mp_debug_bp_t {
    uint32_t line;
    uint16_t name_len;                       // 0 = match any file
    char name[MP_DBG_FILE_MATCH_MAX];
    bool active;
} mp_debug_bp_t;

static mp_debug_bp_t mp_debug_bps[MP_DBG_MAX_BREAKPOINTS];
static uint32_t mp_debug_bp_count = 0;

// Only test the table when the source line actually changes, so a line made of
// many bytecodes costs one lookup rather than one per instruction.
static uint32_t mp_debug_last_line = 0;
static qstr mp_debug_last_file = MP_QSTRnull;

// The innermost code_state at the moment we halted.  Only valid while STOPPED:
// once the VM resumes these are unwound and the pointers go stale.
static const mp_code_state_t *mp_debug_hit_code_state = NULL;

// Stepping state.  mp_debug_step_mode is MP_DBG_STEP_*; the depth and line are
// captured when the step is requested so the stop condition can be evaluated
// against where we started.
static uint32_t mp_debug_step_mode = MP_DBG_STEP_NONE;
static uint32_t mp_debug_step_depth = 0;
static uint32_t mp_debug_step_line = 0;

// Call depth = length of the code_state->prev_state chain.  Measured only at
// line boundaries, never per instruction.
static uint32_t mp_debug_depth(const mp_code_state_t *cs) {
    uint32_t d = 0;
    while (cs != NULL) {
        d++;
        cs = cs->prev_state;
    }
    return d;
}

// Begin a step.  Implicitly resumes: the caller clears STOPPED, the VM runs on,
// and the step condition is evaluated at each line boundary.
int32_t mp_debug_step_begin(uint32_t mode) {
    if (mode > MP_DBG_STEP_OUT) {
        return MP_DBG_FILE_ERR_BAD_REQUEST;
    }
    mp_debug_step_mode = mode;
    if (mode != MP_DBG_STEP_NONE && mp_debug_hit_code_state != NULL) {
        mp_debug_step_depth = mp_debug_depth(mp_debug_hit_code_state);
        mp_debug_step_line = mp_debug_last_line;
    }
    mp_debug_update_armed();
    return 0;
}

uint32_t mp_debug_step_active(void) {
    return mp_debug_step_mode;
}

// Serialise the call stack: uint16 count, then per frame
//   uint32 line, uint16 file_len, file, uint16 name_len, name
// Frames are innermost first, which is the order DAP's stackTrace wants.
//
// The chain walked here is code_state->prev_state, NOT frame->back.  The frame
// object has a back field but nothing ever links it -- py/profile.c sets it to
// NULL on creation and only a Python trace callback would build the chain.  The
// real caller link lives on the C structure, set by FRAME_ENTER in py/vm.c.
// The Python-level frame object has no f_back, so a Python-implemented
// stack walk would see only one frame; the C chain has always existed,
// and that is what we walk.
int mp_debug_stack_serialise(uint8_t *buf, uint32_t buf_max) {
    uint32_t off = 2;
    uint16_t count = 0;

    for (const mp_code_state_t *cs = mp_debug_hit_code_state; cs != NULL; cs = cs->prev_state) {
        if (cs->fun_bc == NULL || cs->fun_bc->rc == NULL) {
            break;
        }
        const mp_raw_code_t *rc = cs->fun_bc->rc;
        const mp_bytecode_prelude_t *prelude = &rc->prelude;
        size_t lasti = cs->ip - prelude->opcodes;
        uint32_t line = mp_prof_bytecode_lineno(rc, lasti);

        const char *file = qstr_str(MP_CODE_QSTR_MAP(cs->fun_bc->context, 0));
        const char *name = qstr_str(MP_CODE_QSTR_MAP(cs->fun_bc->context,
            prelude->qstr_block_name_idx));
        uint16_t flen = (uint16_t)strlen(file);
        uint16_t nlen = (uint16_t)strlen(name);

        if (off + 4 + 2 + flen + 2 + nlen > buf_max) {
            break;              // truncate rather than overrun
        }
        memcpy(buf + off, &line, 4);
        off += 4;
        memcpy(buf + off, &flen, 2);
        off += 2;
        memcpy(buf + off, file, flen);
        off += flen;
        memcpy(buf + off, &nlen, 2);
        off += 2;
        memcpy(buf + off, name, nlen);
        off += nlen;
        count++;
    }

    memcpy(buf, &count, 2);
    return (int)off;
}

// Halt here, or park if another thread already owns this stop.
//
// Everything between claiming and releasing is the owner's alone: the frame the
// host walks, the stop event, and the transport.  A thread that loses the claim
// must touch none of it -- see 12.3a for what each of those looked like when it
// did.
static void mp_debug_stop_here(const mp_code_state_t *code_state,
    uint32_t reason, uint32_t index, uint32_t line, const char *file) {
    if (!mp_debug_claim_halt()) {
        mp_debug_park();
        return;
    }
    mp_debug_hit_code_state = code_state;
    mp_debug_stop(reason, index, line, file);
    mp_debug_hit_code_state = NULL;
    mp_debug_release_halt();
}

// Used by the exception path, which halts from outside this file.
void mp_debug_set_hit_frame(const mp_code_state_t *cs) {
    mp_debug_hit_code_state = cs;
}

// Resolve a DAP frame index (0 = innermost) against the halted chain.
const mp_code_state_t *mp_debug_frame_at(uint32_t index) {
    const mp_code_state_t *cs = mp_debug_hit_code_state;
    while (cs != NULL && index-- > 0) {
        cs = cs->prev_state;
    }
    return cs;
}

uint32_t mp_debug_breakpoints_active(void) {
    return mp_debug_bp_count;
}

// Replace the whole breakpoint set.  Payload:
//   uint16 count, then count x { uint16 name_len; char name[name_len]; uint32 line }
// A zero-length name matches any file, which is what the host uses while it does
// not yet know how the device spells the running script's path.
int32_t mp_debug_breakpoints_set(const uint8_t *payload, uint32_t size) {
    if (payload == NULL || size < 2) {
        return MP_DBG_FILE_ERR_BAD_REQUEST;
    }
    uint16_t count;
    memcpy(&count, payload, 2);

    uint32_t off = 2;
    uint32_t n = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (off + 2 > size) {
            return MP_DBG_FILE_ERR_BAD_REQUEST;
        }
        uint16_t name_len;
        memcpy(&name_len, payload + off, 2);
        off += 2;
        if (off + name_len + 4 > size) {
            return MP_DBG_FILE_ERR_BAD_REQUEST;
        }
        // Take as many as fit and report the count, rather than failing the
        // whole request. The host greys out the ones past the limit, so asking
        // for a ninth breakpoint costs you the ninth -- not the other eight.
        // A path too long to store is skipped for the same reason.
        if (n >= MP_DBG_MAX_BREAKPOINTS || name_len >= MP_DBG_FILE_MATCH_MAX) {
            off += name_len + 4;
            continue;
        }
        memcpy(mp_debug_bps[n].name, payload + off, name_len);
        mp_debug_bps[n].name[name_len] = '\0';
        mp_debug_bps[n].name_len = name_len;
        off += name_len;
        memcpy(&mp_debug_bps[n].line, payload + off, 4);
        off += 4;
        mp_debug_bps[n].active = true;
        n++;
    }
    mp_debug_bp_count = n;
    mp_debug_update_armed();
    mp_debug_last_line = 0;
    mp_debug_last_file = MP_QSTRnull;
    return (int32_t)n;
}

// True if this breakpoint's file constraint matches the running code.  The
// device sees whatever path the module was loaded under, which is not
// necessarily what the editor shows, so a suffix match is used: "main.py"
// matches "/flash/main.py", and "lib/x.py" matches "/flash/lib/x.py".
//
// The suffix must start on a path boundary.  Without that check the tail of
// one name matches another -- "util.py" would fire inside "mathutil.py", and
// "json.py" inside "ujson.py" -- which stops being hypothetical as soon as a
// lib/ directory holds third-party modules.
static bool mp_debug_file_matches(const mp_debug_bp_t *bp, const char *file) {
    if (bp->name_len == 0) {
        return true;
    }
    size_t flen = strlen(file);
    if (bp->name_len > flen) {
        return false;
    }
    size_t at = flen - bp->name_len;
    if (at != 0 && file[at - 1] != '/') {
        return false;
    }
    return strcmp(file + at, bp->name) == 0;
}

// Called from the VM's per-instruction hook, only while armed.
void mp_debug_instr_tick(struct _mp_code_state_t *code_state) {
    // Never trace the debugger's own evaluation: it runs Python while STOPPED
    // is set, which would halt again from inside itself.
    if (mp_debug_in_eval) {
        return;
    }

    // Pause: the host set STOPPED while we were running, so halt at the next
    // instruction.  Checked first so pause works with no breakpoints set.
    if (mp_debug_conditions & MP_DBG_COND_STOPPED) {
        // Report where we stopped. Computing the line costs nothing here --
        // this runs once, when pausing -- and a stop event that names no
        // location is unhelpful in a log or a UI that shows it.
        uint32_t line = 0;
        const char *file = NULL;
        const mp_obj_frame_t *f = code_state->frame;
        if (f != NULL && f->code != NULL) {
            const mp_raw_code_t *rc = f->code->rc;
            line = mp_prof_bytecode_lineno(rc, code_state->ip - rc->prelude.opcodes);
            file = qstr_str(MP_CODE_QSTR_MAP(f->code->context, 0));
        }
        mp_debug_stop_here(code_state, MP_DBG_STOP_PAUSE, 0, line, file);
        return;
    }

    if (mp_debug_bp_count == 0 && mp_debug_step_mode == MP_DBG_STEP_NONE) {
        return;
    }

    const mp_obj_frame_t *frame = code_state->frame;
    if (frame == NULL || frame->code == NULL) {
        return;
    }

    const mp_raw_code_t *rc = frame->code->rc;
    const mp_bytecode_prelude_t *prelude = &rc->prelude;
    qstr file = MP_CODE_QSTR_MAP(frame->code->context, 0);
    size_t lasti = code_state->ip - prelude->opcodes;
    uint32_t line = mp_prof_bytecode_lineno(rc, lasti);

    // Everything below is per line, not per instruction.
    if (line == mp_debug_last_line && file == mp_debug_last_file) {
        return;
    }
    mp_debug_last_line = line;
    mp_debug_last_file = file;

    const char *file_str = qstr_str(file);

    // Stepping.  Depth is only computed once we know the line changed.
    if (mp_debug_step_mode != MP_DBG_STEP_NONE) {
        uint32_t depth = mp_debug_depth(code_state);
        bool stop = false;
        // Compare against the line the step STARTED on, not simply "the line
        // changed".  A call completes on its own line: stepping over
        //     n = add(n, 1)
        // enters add(), runs its lines, then returns to that same line to
        // finish the assignment.  Testing "line changed" sees the return as a
        // change (the last line seen was inside add) and stops on the call site
        // a second time instead of moving on.
        bool moved = (line != mp_debug_step_line) || (depth != mp_debug_step_depth);
        switch (mp_debug_step_mode) {
            case MP_DBG_STEP_IN:
                stop = moved;
                break;
            case MP_DBG_STEP_OVER:
                stop = (depth < mp_debug_step_depth)
                    || (depth == mp_debug_step_depth && line != mp_debug_step_line);
                break;
            case MP_DBG_STEP_OUT:
                stop = (depth < mp_debug_step_depth);
                break;
        }
        if (stop) {
            mp_debug_step_mode = MP_DBG_STEP_NONE;
            mp_debug_stop_here(code_state, MP_DBG_STOP_STEP, 0, line, file_str);
            return;
        }
    }

    for (uint32_t i = 0; i < mp_debug_bp_count; i++) {
        if (!mp_debug_bps[i].active || mp_debug_bps[i].line != line) {
            continue;
        }
        if (!mp_debug_file_matches(&mp_debug_bps[i], file_str)) {
            continue;
        }
        mp_debug_stop_here(code_state, MP_DBG_STOP_BREAKPOINT, i, line, file_str);
        return;
    }
}
