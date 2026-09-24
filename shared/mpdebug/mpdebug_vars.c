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
 * Variable inspection.
 *
 * Globals only, for now.  Local variable *names* are not recoverable in
 * upstream MicroPython: the bytecode prelude carries the function name and
 * the line table but nothing that maps a local slot back to its identifier.
 * Slot values exist in code_state->state[], but reporting "local_0 = 5" is
 * not worth the wire traffic.
 *
 * Globals cover more than it sounds: anything assigned at module level lands
 * there, which is most of what a small MicroPython program keeps state in.
 *
 * Nothing here allocates.  Values are formatted into a static buffer through
 * mp_print_t, because building objects while halted risks running the GC from
 * inside the VM (the locals dict cannot be built while GC is locked), and the
 * stack walk was written to the same rule.
 */
#include <string.h>

#include "py/runtime.h"
#include "py/bc.h"
#include "py/gc.h"
#include "py/objmodule.h"
#include "py/objfun.h"
#include "py/compile.h"
#include "py/lexer.h"
#include "py/profile.h"
#include "py/objcode.h"
#include "py/objlist.h"
#include "py/objtuple.h"
#include "py/objtype.h"
#include <stdio.h>

#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/micropython_debugging.h"


typedef struct _mp_debug_valbuf_t {
    char buf[MP_DBG_VALUE_MAX];
    uint16_t len;
} mp_debug_valbuf_t;

// mp_print_t sink that writes into a fixed buffer and drops the overflow.
static void mp_debug_val_print(void *env, const char *str, size_t len) {
    mp_debug_valbuf_t *v = (mp_debug_valbuf_t *)env;
    while (len-- > 0 && v->len < MP_DBG_VALUE_MAX) {
        v->buf[v->len++] = *str++;
    }
}

static void mp_debug_format(mp_obj_t obj, mp_debug_valbuf_t *out) {
    out->len = 0;
    mp_print_t print = { out, mp_debug_val_print };
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        // A __repr__ can raise, and an exception escaping the halt loop would
        // take the board down rather than failing one variable.
        mp_obj_print_helper(&print, obj, PRINT_REPR);
        nlr_pop();
    } else {
        out->len = 0;
        const char *err = "<error>";
        while (*err) {
            out->buf[out->len++] = *err++;
        }
    }
}

/*
 * Object handles.
 *
 * DAP refers to a container by an opaque variablesReference, so the device has
 * to be able to hand one out and resolve it later. Handles are valid only while
 * halted and are reissued at every stop: once execution resumes the objects may
 * be collected or replaced, and a stale handle would read freed memory.
 *
 * The table is a GC root. The objects are normally reachable anyway -- they are
 * in globals or inside a container we walked to get here -- but registering it
 * means that stays true even if the only remaining reference is ours, which
 * matters because evaluation allocates and can therefore collect.
 */
#define MP_DBG_MAX_HANDLES (32)

MP_REGISTER_ROOT_POINTER(mp_obj_t mpdebug_handles[MP_DBG_MAX_HANDLES]);
static uint32_t mp_debug_handle_count = 0;

void mp_debug_handles_reset(void) {
    for (uint32_t i = 0; i < MP_DBG_MAX_HANDLES; i++) {
        MP_STATE_VM(mpdebug_handles)[i] = MP_OBJ_NULL;
    }
    mp_debug_handle_count = 0;
}

// Containers worth offering an expander for. Anything else is shown as its
// repr alone, which is honest: a handle that produced no children would give
// the user an arrow that opens onto nothing.
static bool mp_debug_expandable(mp_obj_t o) {
    if (o == MP_OBJ_NULL || mp_obj_is_small_int(o) || mp_obj_is_qstr(o)) {
        return false;
    }
    if (mp_obj_is_type(o, &mp_type_list) || mp_obj_is_type(o, &mp_type_tuple)
        || mp_obj_is_type(o, &mp_type_dict)) {
        return true;
    }
    // Class instances keep their attributes in a map, so they expand the same
    // way a dict does. Worth having: most real code puts its state on objects,
    // and an instance that showed only <Foo object at 0x...> would be the least
    // useful entry in the panel.
    return mp_obj_is_obj(o) && mp_obj_is_instance_type(mp_obj_get_type(o));
}

static uint32_t mp_debug_handle_for(mp_obj_t o) {
    if (!mp_debug_expandable(o) || mp_debug_handle_count >= MP_DBG_MAX_HANDLES) {
        return 0;
    }
    MP_STATE_VM(mpdebug_handles)[mp_debug_handle_count] = o;
    return ++mp_debug_handle_count;          // 1-based; 0 means "no children"
}

static mp_obj_t mp_debug_handle_get(uint32_t handle) {
    if (handle == 0 || handle > mp_debug_handle_count) {
        return MP_OBJ_NULL;
    }
    return MP_STATE_VM(mpdebug_handles)[handle - 1];
}

/*
 * One variable entry:
 *   uint16 name_len, name, uint16 value_len, value, uint32 handle
 * Returns the new offset, or 0 if it would not fit.
 */
static uint32_t mp_debug_encode_var(uint8_t *buf, uint32_t off, uint32_t buf_max,
    const char *name, uint16_t nlen, mp_obj_t value) {
    mp_debug_valbuf_t val;
    mp_debug_format(value, &val);
    uint32_t handle = mp_debug_handle_for(value);

    if (off + 2 + nlen + 2 + val.len + 4 > buf_max) {
        return 0;
    }
    memcpy(buf + off, &nlen, 2);
    off += 2;
    memcpy(buf + off, name, nlen);
    off += nlen;
    memcpy(buf + off, &val.len, 2);
    off += 2;
    memcpy(buf + off, val.buf, val.len);
    off += val.len;
    memcpy(buf + off, &handle, 4);
    off += 4;
    return off;
}

// A slot that has not been assigned yet: keeps its position in the list, with
// an empty value, so the host's slot numbering stays aligned.
static uint32_t mp_debug_encode_unset(uint8_t *buf, uint32_t off, uint32_t buf_max,
    const char *name, uint16_t nlen) {
    uint16_t zero = 0;
    uint32_t handle = 0;
    if (off + 2 + nlen + 2 + 4 > buf_max) {
        return 0;
    }
    memcpy(buf + off, &nlen, 2);
    off += 2;
    memcpy(buf + off, name, nlen);
    off += nlen;
    memcpy(buf + off, &zero, 2);
    off += 2;
    memcpy(buf + off, &handle, 4);
    off += 4;
    return off;
}

/*
 * Serialise a scope:
 *   uint16 count, then per variable
 *     uint16 name_len, name, uint16 value_len, value
 *
 * Truncates rather than overruns; the host sees however many fit.
 */
/*
 * Locals.
 *
 * MicroPython keeps a function's locals in code_state->state[], addressed from
 * the top: state[n_state - 1 - i]. There is no slot-to-identifier map, which is
 * why a debugger cannot simply name every local -- but the *arguments* are not
 * guesswork. They occupy the first slots in declaration order, and their names
 * are in the bytecode: the prelude is followed by the block name and then one
 * qstr per positional and keyword-only argument, which is how the VM matches
 * keyword arguments at call time (py/bc.c). Reading them back is exact, costs
 * nothing at runtime, and works inside a .mpy, because the qstr table is stored
 * there too.
 *
 * Slots past the arguments are sent with an empty name. Their values are real
 * but their identity is not knowable here: n_state covers the locals *and* the
 * value stack, locals at the top and stack from the bottom, and nothing in the
 * bytecode records where the boundary falls. The host can name some of them
 * from the source, and it can check its work because the argument names above
 * are authoritative -- if its own analysis disagrees about those, it knows not
 * to trust its answer for the rest.
 *
 * Every slot is sent in order, including unassigned ones, which are sent with
 * an empty value. Position carries the meaning here, so dropping an entry
 * would shift every slot after it and quietly mislabel them.
 * (mp_setup_code_state memsets the state array, so unset slots read NULL.)
 */
// Safe to hand to mp_obj_print_helper?
//
// Non-argument locals share the same state[] array as the VM's value stack,
// and nothing in the bytecode records where the boundary between the two
// falls. So when we walk slots past n_args, we may pick up raw C values --
// saved instruction pointers, iterator counters, byte offsets -- that happen
// to have LSB=0 and look like tagged pointers. Handing one of those to
// mp_obj_print_helper follows the "pointer" into unmapped memory and either
// crashes or hangs the board.
//
// The escape valve: only accept a slot as a real object if it is either an
// immediate (small int, qstr, imm obj -- all safe by tag) or a pointer that
// gc_nbytes() confirms is a live block on the GC heap. gc_nbytes checks
// alignment and range internally and is safe to call with any pointer value.
//
// The cost is that ROM-hosted objects (mp_const_empty_tuple, module and
// function references, class type objects) get filtered out. In practice,
// user-facing values in a debugger are small ints, strings, dicts, lists --
// all GC-heap allocated -- so this catches the important cases and hides
// only the rare ones. Preferable to a hang.
static bool mp_debug_slot_safe(mp_obj_t v) {
    if (v == MP_OBJ_NULL) {
        return false;
    }
    if (!mp_obj_is_obj(v)) {
        return true;                     // small int, qstr, imm obj
    }
    return gc_nbytes(MP_OBJ_TO_PTR(v)) > 0;
}

static int mp_debug_locals_serialise(const mp_code_state_t *cs, uint32_t start,
    uint8_t *buf, uint32_t buf_max) {
    uint32_t off = 4;
    uint16_t count = 0, more = 0;

    const byte *ip = cs->fun_bc->bytecode;
    MP_BC_PRELUDE_SIG_DECODE(ip);
    MP_BC_PRELUDE_SIZE_DECODE(ip);

    // ip now sits at the block name, followed by one qstr per argument.
    const byte *arg_names = mp_decode_uint_skip(ip);
    size_t n_args = n_pos_args + n_kwonly_args;

    // Named arguments first: the bytecode's qstr table names them for us.
    // Slots past n_args are non-argument locals plus the VM's value stack;
    // we send them with an empty name and let the host attach names from
    // its source-based analysis (localNames.ts). Each unnamed slot passes
    // through mp_debug_slot_safe -- a bad pointer that happens to sit at
    // one of those positions is treated as unset rather than formatted,
    // which is what once hung the board on the second stop after a for loop.
    uint32_t seen = 0;
    for (size_t i = 0; i < n_args; i++) {
        qstr q = mp_decode_uint(&arg_names);
        #if MICROPY_EMIT_BYTECODE_USES_QSTR_TABLE
        q = cs->fun_bc->context->constants.qstr_table[q];
        #endif
        const char *name = qstr_str(q);
        uint16_t nlen = (uint16_t)strlen(name);
        mp_obj_t value = cs->state[n_state - 1 - i];
        if (seen++ < start) {
            continue;
        }
        uint32_t next = (value == MP_OBJ_NULL)
            ? mp_debug_encode_unset(buf, off, buf_max, name, nlen)
            : mp_debug_encode_var(buf, off, buf_max, name, nlen, value);
        if (next == 0) {
            more = 1;
            break;
        }
        off = next;
        count++;
    }

    if (!more) {
        for (size_t i = n_args; i < n_state; i++) {
            mp_obj_t value = cs->state[n_state - 1 - i];
            if (seen++ < start) {
                continue;
            }
            uint32_t next = mp_debug_slot_safe(value)
                ? mp_debug_encode_var(buf, off, buf_max, "", 0, value)
                : mp_debug_encode_unset(buf, off, buf_max, "", 0);
            if (next == 0) {
                more = 1;
                break;
            }
            off = next;
            count++;
        }
    }

    memcpy(buf, &count, 2);
    memcpy(buf + 2, &more, 2);
    return (int)off;
}

int mp_debug_vars_serialise(const mp_code_state_t *cs, uint32_t scope,
    uint32_t start, uint8_t *buf, uint32_t buf_max) {
    uint32_t off = 4;
    uint16_t count = 0, more = 0;

    if (cs == NULL || cs->fun_bc == NULL
        || (scope != MP_DBG_SCOPE_GLOBALS && scope != MP_DBG_SCOPE_LOCALS)) {
        memcpy(buf, &count, 2);
        memcpy(buf + 2, &more, 2);
        return 4;
    }

    if (scope == MP_DBG_SCOPE_LOCALS) {
        return mp_debug_locals_serialise(cs, start, buf, buf_max);
    }

    mp_obj_dict_t *globals = cs->fun_bc->context->module.globals;
    if (globals == NULL) {
        memcpy(buf, &count, 2);
        memcpy(buf + 2, &more, 2);
        return 4;
    }

    mp_map_t *map = &globals->map;
    uint32_t seen = 0;
    for (size_t i = 0; i < map->alloc; i++) {
        if (!mp_map_slot_is_filled(map, i)) {
            continue;
        }
        mp_obj_t key = map->table[i].key;
        if (!mp_obj_is_qstr(key)) {
            continue;
        }
        if (seen++ < start) {
            continue;
        }
        const char *name = qstr_str(MP_OBJ_QSTR_VALUE(key));
        uint32_t next = mp_debug_encode_var(buf, off, buf_max,
            name, (uint16_t)strlen(name), map->table[i].value);
        if (next == 0) {
            more = 1;
            break;
        }
        off = next;
        count++;
    }

    memcpy(buf, &count, 2);
    memcpy(buf + 2, &more, 2);
    return (int)off;
}

/*
 * Children of a container, addressed by the handle issued when it was listed.
 *
 * Same encoding as a scope, so the host decodes both with one routine.
 * Sequences are named by index, dicts by the repr of their key -- which is what
 * the user typed, and what they would type again to reach it.
 */
int mp_debug_children_serialise(uint32_t handle, uint32_t start,
    uint8_t *buf, uint32_t buf_max) {
    uint32_t off = 4;
    uint16_t count = 0, more = 0;
    mp_obj_t obj = mp_debug_handle_get(handle);

    if (obj == MP_OBJ_NULL) {
        memcpy(buf, &count, 2);
        memcpy(buf + 2, &more, 2);
        return 4;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        if (mp_obj_is_type(obj, &mp_type_list) || mp_obj_is_type(obj, &mp_type_tuple)) {
            size_t len;
            mp_obj_t *items;
            mp_obj_get_array(obj, &len, &items);
            for (size_t i = start; i < len; i++) {
                char name[16];
                int n = snprintf(name, sizeof(name), "[%u]", (unsigned int)i);
                uint32_t next = mp_debug_encode_var(buf, off, buf_max,
                    name, (uint16_t)n, items[i]);
                if (next == 0) {
                    more = 1;
                    break;
                }
                off = next;
                count++;
            }
        } else {
            // Dicts and class instances are both a map of key to value; the
            // only difference is where the map lives.
            mp_map_t *map = NULL;
            if (mp_obj_is_type(obj, &mp_type_dict)) {
                map = &((mp_obj_dict_t *)MP_OBJ_TO_PTR(obj))->map;
            } else if (mp_obj_is_obj(obj) && mp_obj_is_instance_type(mp_obj_get_type(obj))) {
                map = &((mp_obj_instance_t *)MP_OBJ_TO_PTR(obj))->members;
            }
            if (map != NULL) {
                uint32_t seen = 0;
                for (size_t i = 0; i < map->alloc; i++) {
                    if (!mp_map_slot_is_filled(map, i)) {
                        continue;
                    }
                    if (seen++ < start) {
                        continue;
                    }
                    mp_debug_valbuf_t key;
                    mp_debug_format(map->table[i].key, &key);
                    uint32_t next = mp_debug_encode_var(buf, off, buf_max,
                        key.buf, key.len, map->table[i].value);
                    if (next == 0) {
                        more = 1;
                        break;
                    }
                    off = next;
                    count++;
                }
            }
        }
        nlr_pop();
    } else {
        // A __repr__ or a custom container can raise; report what we managed
        // rather than losing the whole request.
    }

    memcpy(buf, &count, 2);
    memcpy(buf + 2, &more, 2);
    return (int)off;
}

/*
 * Evaluate an expression in a frame's context.
 *
 * MicroPython makes this straightforward: parse, compile, call -- with the
 * frame's module globals installed so names resolve the way the user expects.
 *
 * Unlike the rest of this file this DOES allocate: compiling requires the heap.
 * That is unavoidable for evaluation, and acceptable because it happens only
 * when a user asks, never on the hot path. It is wrapped in nlr_push so a
 * syntax error, a NameError, or an exception raised by the expression itself
 * fails the request instead of taking down the board.
 *
 * Reply: int32 rc (0 ok, negative on failure), uint16 len, value.
 */
int mp_debug_eval(const mp_code_state_t *cs, const char *expr, uint32_t expr_len,
    uint8_t *buf, uint32_t buf_max) {
    mp_debug_valbuf_t val;
    val.len = 0;
    int32_t rc = 0;

    #if MICROPY_ENABLE_COMPILER
    if (cs == NULL || cs->fun_bc == NULL || expr_len == 0) {
        rc = MP_DBG_FILE_ERR_BAD_REQUEST;
    } else {
        mp_obj_dict_t *saved = mp_globals_get();
        mp_debug_in_eval = true;
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_globals_set(cs->fun_bc->context->module.globals);

            mp_lexer_t *lex = mp_lexer_new_from_str_len(
                MP_QSTR__lt_string_gt_, expr, expr_len, 0);
            qstr source_name = lex->source_name;
            mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_EVAL_INPUT);
            mp_obj_t fun = mp_compile(&tree, source_name, false);
            mp_obj_t result = mp_call_function_0(fun);

            mp_debug_format(result, &val);
            mp_globals_set(saved);
            mp_debug_in_eval = false;
            nlr_pop();
        } else {
            mp_globals_set(saved);
            mp_debug_in_eval = false;
            // Report the exception rather than a generic failure: "NameError:
            // name not defined" is the answer the user needs.
            mp_debug_format(MP_OBJ_FROM_PTR(nlr.ret_val), &val);
            rc = MP_DBG_FILE_ERR_FAILED;
        }
    }
    #else
    // No on-device compiler: expression evaluation is unavailable.  Host runs
    // mpy-cross so full source is never on the target.  Report failure with an
    // empty value so the host can surface "unsupported" in the Watch panel.
    (void)cs;
    (void)expr;
    (void)expr_len;
    rc = MP_DBG_FILE_ERR_FAILED;
    #endif

    if (buf_max < 6u + val.len) {
        val.len = 0;
    }
    memcpy(buf, &rc, 4);
    memcpy(buf + 4, &val.len, 2);
    memcpy(buf + 6, val.buf, val.len);
    return 6 + val.len;
}

/*
 * Halt at the raise point of an exception nothing is going to catch.
 *
 * Called from the VM before any unwinding, so the whole frame chain is still
 * live and the host can walk it. That is the difference between "your program
 * crashed" and "your program crashed HERE, and these were the frames".
 *
 * Distinguishing caught from uncaught: a frame with exc_sp_idx == 0 has no
 * active exception handler. If no frame in the chain has one, nothing can
 * catch this. The test is deliberately conservative -- an active try block
 * whose except clause does not actually match will suppress the halt -- because
 * stopping on exceptions the program handles itself would make the debugger
 * unusable on any code that uses try/except for control flow.
 */
void mp_debug_exception(const mp_code_state_t *cs, void *exc) {
    if (cs == NULL || exc == NULL) {
        return;
    }
    if (mp_debug_in_eval) {
        return;      // an exception raised by an evaluated expression is a
    }                // result to report, not a program crash to halt on
    if (mp_debug_conditions & MP_DBG_COND_STOPPED) {
        return;                      // already halted; do not recurse
    }
    for (const mp_code_state_t *f = cs; f != NULL; f = f->prev_state) {
        if (f->exc_sp_idx > 0) {
            return;                  // somebody up the chain may handle it
        }
    }

    // Where did it happen?
    uint32_t line = 0;
    const char *file = "";
    if (cs->fun_bc != NULL && cs->fun_bc->rc != NULL) {
        const mp_bytecode_prelude_t *prelude = &cs->fun_bc->rc->prelude;
        line = mp_prof_bytecode_lineno(cs->fun_bc->rc, cs->ip - prelude->opcodes);
        file = qstr_str(MP_CODE_QSTR_MAP(cs->fun_bc->context, 0));
    }

    // Send the exception text through the normal output path so it appears in
    // the Debug Console; the stopped event itself carries no message field.
    mp_debug_valbuf_t val;
    mp_debug_format(MP_OBJ_FROM_PTR(exc), &val);
    mp_debug_stdout("\nUncaught exception: ", 21);
    mp_debug_stdout(val.buf, val.len);
    mp_debug_stdout("\n", 1);

    // One thread owns the stop; another arriving here parks silently rather than
    // reporting a second exception and overwriting the frame the host walks.
    // See 12.3a.
    if (!mp_debug_claim_halt()) {
        mp_debug_park();
        return;
    }
    mp_debug_set_hit_frame(cs);
    mp_debug_stop(MP_DBG_STOP_EXCEPTION, 0, line, file);
    mp_debug_set_hit_frame(NULL);
    mp_debug_release_halt();
}

/*
 * Assign to a name in a frame's context.
 *
 * Implemented as evaluating "<name> = <expr>" rather than as a separate write
 * path: the compiler already knows how to bind a name, and reusing it means
 * assignment obeys exactly the same rules as typing the statement at the REPL.
 *
 * Only names in module globals can be set, which is the same limit the
 * Variables panel has and for the same reason: locals have no names to bind to.
 *
 * Reply: int32 rc, uint16 len, the value after assignment.
 */
int mp_debug_set_variable(const mp_code_state_t *cs,
    const char *name, uint32_t name_len,
    const char *expr, uint32_t expr_len,
    uint8_t *buf, uint32_t buf_max) {
    mp_debug_valbuf_t val;
    val.len = 0;
    int32_t rc = 0;

    #if MICROPY_ENABLE_COMPILER
    char stmt[192];

    if (cs == NULL || name_len == 0 || expr_len == 0
        || name_len + expr_len + 4 > sizeof(stmt)) {
        rc = MP_DBG_FILE_ERR_BAD_REQUEST;
    } else {
        memcpy(stmt, name, name_len);
        memcpy(stmt + name_len, " = ", 3);
        memcpy(stmt + name_len + 3, expr, expr_len);
        uint32_t stmt_len = name_len + 3 + expr_len;

        mp_obj_dict_t *saved = mp_globals_get();
        mp_debug_in_eval = true;
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_globals_set(cs->fun_bc->context->module.globals);

            mp_lexer_t *lex = mp_lexer_new_from_str_len(
                MP_QSTR__lt_string_gt_, stmt, stmt_len, 0);
            qstr source_name = lex->source_name;
            mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
            mp_obj_t fun = mp_compile(&tree, source_name, false);
            mp_call_function_0(fun);

            // Read the name back, so the host shows what the device actually
            // holds rather than echoing what was requested.
            mp_obj_t now = mp_load_global(qstr_from_strn(name, name_len));
            mp_debug_format(now, &val);

            mp_globals_set(saved);
            mp_debug_in_eval = false;
            nlr_pop();
        } else {
            mp_globals_set(saved);
            mp_debug_in_eval = false;
            mp_debug_format(MP_OBJ_FROM_PTR(nlr.ret_val), &val);
            rc = MP_DBG_FILE_ERR_FAILED;
        }
    }
    #else
    // No on-device compiler: assignment is implemented as
    // "<name> = <expr>" compiled and run, which we cannot do.
    (void)cs;
    (void)name;
    (void)name_len;
    (void)expr;
    (void)expr_len;
    rc = MP_DBG_FILE_ERR_FAILED;
    #endif

    if (buf_max < 6u + val.len) {
        val.len = 0;
    }
    memcpy(buf, &rc, 4);
    memcpy(buf + 4, &val.len, 2);
    memcpy(buf + 6, val.buf, val.len);
    return 6 + val.len;
}
