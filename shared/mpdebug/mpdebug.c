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

#include <string.h>

#include "py/runtime.h"
#include "py/mpthread.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "shared/runtime/pyexec.h"
#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/mpdebug_port.h"
#include "shared/mpdebug/micropython_debugging.h"

volatile bool mp_debug_armed = false;
volatile uint32_t mp_debug_conditions = 0;

// The VM hook must be live whenever a session is attached -- not merely when
// breakpoints exist -- otherwise pause and stepping can never take effect.
void mp_debug_update_armed(void) {
    mp_debug_armed = ((mp_debug_conditions & (MP_DBG_COND_ATTACHED | MP_DBG_COND_STOPPED)) != 0)
        || (mp_debug_breakpoints_active() > 0)
        || (mp_debug_step_active() != MP_DBG_STEP_NONE);
}
static volatile bool mp_debug_reboot_hard = false;

static wp_receiver_t mp_debug_rx;
static bool mp_debug_inited = false;
static uint32_t mp_debug_msgs_in = 0;
static uint32_t mp_debug_msgs_out = 0;
static uint32_t mp_debug_halts = 0;

static volatile bool mp_debug_reboot_pending = false;
static volatile bool mp_debug_in_halt = false;

// Set only while halted at entry, inside mp_debug_run_main_py.
//
// The distinction matters for reboots. Returning
// BOARDCTRL_GOTO_SOFT_RESET_EXIT gives a clean soft reset that keeps the USB
// connection, but it only works from inside that hook. Halted at a breakpoint
// we are deep in the VM instead: releasing the halt merely resumes the user's
// program, and run_main_py is never re-entered -- a `while True:` loop never
// returns -- so the reset silently never happens and the board keeps running
// the old code. Anywhere but an entry halt, reset the hard way.
static volatile bool mp_debug_in_entry_halt = false;
static volatile bool mp_debug_in_poll = false;
static volatile bool mp_debug_in_output = false;

// Set while an expression supplied by the host is being evaluated.  Evaluating
// runs Python, which trips the VM instruction hook; with STOPPED still set that
// hook would halt again, nested inside the evaluation, and the channel would
// never be serviced again.  A debugger must not trace itself.
volatile bool mp_debug_in_eval = false;

// Diagnostic: how many times the VM loop hook has run. Answers "is the hook
// even reached" with a number instead of an argument.
uint32_t mp_debug_vm_hook_calls = 0;

// Default port hook for Monitor_EnterDfu.  Ports without a firmware-update
// loader keep the default (do nothing); STM32C0 provides a strong override
// that arms mpy_boot.c and resets into ST's ROM DFU.
__attribute__((weak)) void mp_debug_port_enter_dfu(void) {
}

static void mp_debug_send(const wp_packet_t *hdr, const void *payload, uint32_t size) {
    if (!mp_debug_port_link_up()) {
        return;
    }
    mp_debug_port_tx_always((const uint8_t *)hdr, sizeof(*hdr));
    if (size != 0 && payload != NULL) {
        mp_debug_port_tx_always((const uint8_t *)payload, size);
    }
    mp_debug_msgs_out++;
}

static void mp_debug_dispatch(void *ctx, const wp_packet_t *hdr, const uint8_t *payload) {
    wp_receiver_t *r = (wp_receiver_t *)ctx;
    wp_packet_t out;
    (void)payload;

    mp_debug_msgs_in++;

    switch (hdr->cmd) {
        case MP_DBG_CMD_MONITOR_PING: {
            mp_dbg_ping_t reply;
            reply.source = MP_DBG_PING_SOURCE_DEVICE;
            reply.dbg_flags = 0;
            wp_build_reply(r, hdr, WP_FLAG_ACK, &reply, sizeof(reply), &out);
            mp_debug_send(&out, &reply, sizeof(reply));
            break;
        }
        case MP_DBG_CMD_MONITOR_REBOOT: {
            uint32_t flags = MP_DBG_REBOOT_SOFT;
            if (payload != NULL && hdr->size >= sizeof(mp_dbg_reboot_t)) {
                mp_dbg_reboot_t req;
                memcpy(&req, payload, sizeof(req));
                flags = req.flags;
            }
            if (flags & MP_DBG_REBOOT_WAIT_FOR_DEBUGGER) {
                mp_debug_conditions |= MP_DBG_COND_STOP_ON_START;
            }
            // Acknowledge before resetting: after this the VM restarts, and on a
            // hard reset USB re-enumerates and the reply would be lost.
            wp_build_reply(r, hdr, WP_FLAG_ACK, &flags, sizeof(flags), &out);
            mp_debug_send(&out, &flags, sizeof(flags));

            mp_debug_reboot_pending = true;

            // A clean soft reset means returning BOARDCTRL_GOTO_SOFT_RESET_EXIT
            // from the run-main-py board hook, which only works while we are
            // actually inside that hook -- i.e. halted.  Anywhere else there is
            // no safe way to unwind: scheduling a SystemExit from the idle hook
            // surfaces outside pyexec's nlr_push and lands in the fatal handler
            // instead of restarting.  So fall back to a hard reset, which costs
            // a USB re-enumeration and a host reconnect.
            if ((flags & MP_DBG_REBOOT_HARD) || !mp_debug_in_entry_halt) {
                mp_debug_reboot_hard = true;
                // .bss does not survive a hard reset; carry the request across.
                if (mp_debug_conditions & MP_DBG_COND_STOP_ON_START) {
                    mp_debug_port_persist_write(MP_DBG_COND_STOP_ON_START);
                }
            } else {
                mp_debug_conditions &= ~MP_DBG_COND_STOPPED;   // let the halt return
            }
            break;
        }

        case MP_DBG_CMD_MONITOR_ENTER_DFU: {
            // Ack before leaving: the USB link is about to drop and any reply
            // sent after that is lost.  ACK carries no payload -- the caller
            // just needs to know the request landed before the device vanishes.
            uint32_t rc = 0;
            wp_build_reply(r, hdr, WP_FLAG_ACK, &rc, sizeof(rc), &out);
            mp_debug_send(&out, &rc, sizeof(rc));

            // Flush anything still cached before the reset, mirroring the
            // reboot path.  A File_Put in flight would otherwise lose its
            // directory entry across the reset.
            mp_debug_file_finish_put();
            mp_debug_port_storage_flush();

            // Let the ACK reach the host before the link drops.
            mp_hal_delay_us(50000);

            // Ports without a DFU loader keep the weak default and this call
            // returns; the host times out waiting for the DFU device to appear
            // and reports it, which is the right shape for that failure.  The
            // STM32C0 override does not return.
            mp_debug_port_enter_dfu();
            break;
        }

        case MP_DBG_CMD_THREAD_LIST: {
            // DAP requires a threads response after every stop.  MicroPython
            // here is single-threaded, so report exactly one.
            struct __attribute__((packed)) {
                uint16_t count;
                uint32_t id;
            } reply = { 1, 1 };
            wp_build_reply(r, hdr, WP_FLAG_ACK, &reply, sizeof(reply), &out);
            mp_debug_send(&out, &reply, sizeof(reply));
            break;
        }

        case MP_DBG_CMD_VALUE_SET_VARIABLE: {
            static uint8_t set_buf[WP_MAX_PAYLOAD];
            uint32_t frame = 0;
            uint16_t nlen = 0, elen = 0;
            int n;
            if (payload != NULL && hdr->size >= 6) {
                memcpy(&frame, payload, 4);
                memcpy(&nlen, payload + 4, 2);
            }
            if (payload != NULL && hdr->size >= 8u + nlen) {
                memcpy(&elen, payload + 6 + nlen, 2);
            }
            if (nlen != 0 && elen != 0 && 8u + nlen + elen <= hdr->size) {
                n = mp_debug_set_variable(mp_debug_frame_at(frame),
                    (const char *)payload + 6, nlen,
                    (const char *)payload + 8 + nlen, elen,
                    set_buf, sizeof(set_buf));
            } else {
                n = mp_debug_set_variable(NULL, "", 0, "", 0, set_buf, sizeof(set_buf));
            }
            wp_build_reply(r, hdr, WP_FLAG_ACK, set_buf, n, &out);
            mp_debug_send(&out, set_buf, n);
            break;
        }

        case MP_DBG_CMD_VALUE_EVALUATE: {
            static uint8_t eval_buf[WP_MAX_PAYLOAD];
            uint32_t frame = 0;
            uint16_t elen = 0;
            int n;
            if (payload != NULL && hdr->size >= 6) {
                memcpy(&frame, payload, 4);
                memcpy(&elen, payload + 4, 2);
            }
            if (elen != 0 && 6u + elen <= hdr->size) {
                n = mp_debug_eval(mp_debug_frame_at(frame),
                    (const char *)payload + 6, elen, eval_buf, sizeof(eval_buf));
            } else {
                n = mp_debug_eval(NULL, "", 0, eval_buf, sizeof(eval_buf));
            }
            wp_build_reply(r, hdr, WP_FLAG_ACK, eval_buf, n, &out);
            mp_debug_send(&out, eval_buf, n);
            break;
        }

        case MP_DBG_CMD_VALUE_GET_CHILDREN: {
            static uint8_t child_buf[WP_MAX_PAYLOAD];
            uint32_t handle = 0, start = 0;
            if (payload != NULL && hdr->size >= 4) {
                memcpy(&handle, payload, 4);
            }
            if (payload != NULL && hdr->size >= 8) {
                memcpy(&start, payload + 4, 4);
            }
            int n = mp_debug_children_serialise(handle, start, child_buf, sizeof(child_buf));
            wp_build_reply(r, hdr, WP_FLAG_ACK, child_buf, n, &out);
            mp_debug_send(&out, child_buf, n);
            break;
        }

        case MP_DBG_CMD_VALUE_GET_SCOPE: {
            uint32_t frame = 0, scope = MP_DBG_SCOPE_GLOBALS, start = 0;
            if (payload != NULL && hdr->size >= 8) {
                memcpy(&frame, payload, 4);
                memcpy(&scope, payload + 4, 4);
            }
            if (payload != NULL && hdr->size >= 12) {
                memcpy(&start, payload + 8, 4);
            }
            static uint8_t var_buf[WP_MAX_PAYLOAD];
            int n = mp_debug_vars_serialise(mp_debug_frame_at(frame), scope,
                start, var_buf, sizeof(var_buf));
            wp_build_reply(r, hdr, WP_FLAG_ACK, var_buf, n, &out);
            mp_debug_send(&out, var_buf, n);
            break;
        }

        case MP_DBG_CMD_THREAD_STACK: {
            // Served from the halt loop, so the frames are still live.
            static uint8_t stack_buf[WP_MAX_PAYLOAD];
            int n = mp_debug_stack_serialise(stack_buf, sizeof(stack_buf));
            wp_build_reply(r, hdr, WP_FLAG_ACK, stack_buf, n, &out);
            mp_debug_send(&out, stack_buf, n);
            break;
        }

        case MP_DBG_CMD_EXECUTION_STEP: {
            uint32_t mode = MP_DBG_STEP_NONE;
            if (payload != NULL && hdr->size >= 4) {
                memcpy(&mode, payload, 4);
            }
            int32_t rc = mp_debug_step_begin(mode);
            wp_build_reply(r, hdr, WP_FLAG_ACK, &rc, sizeof(rc), &out);
            mp_debug_send(&out, &rc, sizeof(rc));
            break;
        }

        case MP_DBG_CMD_EXECUTION_CAPABILITIES: {
            struct __attribute__((packed)) {
                uint16_t protocol;
                uint16_t max_breakpoints;
                uint16_t max_payload;
                uint16_t max_value_len;
                uint32_t vm_hook_calls;   // diagnostic: is the VM hook reached?
            } caps = {
                MP_DBG_PROTOCOL_VERSION,
                MP_DBG_MAX_BREAKPOINTS,
                WP_MAX_PAYLOAD,
                MP_DBG_VALUE_MAX,
                mp_debug_vm_hook_calls,
            };
            wp_build_reply(r, hdr, WP_FLAG_ACK, &caps, sizeof(caps), &out);
            mp_debug_send(&out, &caps, sizeof(caps));
            break;
        }

        case MP_DBG_CMD_EXECUTION_BREAKPOINTS: {
            int32_t rc = mp_debug_breakpoints_set(payload, hdr->size);
            // Arm the VM hook only while breakpoints exist, so a session with
            // none costs nothing on the interpreter hot path.
            mp_debug_update_armed();
            wp_build_reply(r, hdr, WP_FLAG_ACK, &rc, sizeof(rc), &out);
            mp_debug_send(&out, &rc, sizeof(rc));
            break;
        }

        case MP_DBG_CMD_EXECUTION_CHANGE_CONDITIONS: {
            uint32_t set = 0, reset = 0;
            if (payload != NULL && hdr->size >= sizeof(mp_dbg_change_conditions_t)) {
                mp_dbg_change_conditions_t req;
                memcpy(&req, payload, sizeof(req));
                set = req.set;
                reset = req.reset;
            }
            mp_debug_conditions = (mp_debug_conditions | set) & ~reset;
            mp_debug_update_armed();
            uint32_t current = mp_debug_conditions;
            wp_build_reply(r, hdr, WP_FLAG_ACK, &current, sizeof(current), &out);
            mp_debug_send(&out, &current, sizeof(current));
            break;
        }

        default: {
            // File commands are handled in mpdebug_files.c.
            uint8_t reply_buf[16];
            int n = mp_debug_file_dispatch(hdr->cmd, payload, hdr->size,
                reply_buf, sizeof(reply_buf));
            if (n >= 0) {
                wp_build_reply(r, hdr, WP_FLAG_ACK, reply_buf, n, &out);
                mp_debug_send(&out, reply_buf, n);
            } else {
                wp_build_reply(r, hdr, WP_FLAG_NACK, NULL, 0, &out);
                mp_debug_send(&out, NULL, 0);
            }
            break;
        }
    }
}

void mp_debug_init(void) {
    wp_receiver_init(&mp_debug_rx);
    mp_debug_msgs_in = 0;
    mp_debug_msgs_out = 0;
    mp_debug_inited = true;
}

// The pump proper.  Split from mp_debug_poll() so the halt loop can service the
// channel even when it is nested inside a poll: halting from a dispatched
// command is legitimate, and the reentrancy guard must not silence it.
static void mp_debug_pump(void) {
    if (!mp_debug_inited) {
        mp_debug_init();
    }
    if (!mp_debug_port_link_up()) {
        return;
    }
    uint8_t buf[64];
    for (;;) {
        int avail = mp_debug_port_rx_avail();
        if (avail <= 0) {
            break;
        }
        if (avail > (int)sizeof(buf)) {
            avail = sizeof(buf);
        }
        int n = mp_debug_port_rx(buf, avail);
        if (n <= 0) {
            break;
        }
        wp_receive(&mp_debug_rx, buf, n, mp_debug_dispatch, &mp_debug_rx);
    }

}

void mp_debug_poll(void) {
    // Filesystem work blocks on flash erase/program, and anything that blocks
    // can re-enter here through the idle hook.  One level only.
    if (mp_debug_in_poll) {
        return;
    }
    mp_debug_in_poll = true;
    mp_debug_pump();
    mp_debug_in_poll = false;

    if (mp_debug_reboot_hard) {
        // Clear first: mp_debug_poll() must not re-enter this block.  The delays
        // below MUST NOT be mp_hal_delay_ms(), which runs the idle behaviour and
        // therefore calls MICROPY_INTERNAL_EVENT_HOOK -> mp_debug_event_hook() ->
        // mp_debug_poll(), recursing here until the board wedges mid-teardown
        // with USB half torn down.  mp_hal_delay_us() busy-waits instead.
        mp_debug_reboot_hard = false;

        // Anything still sitting in the filesystem cache would be lost across
        // the reset, and a File_Put still open would lose its directory entry
        // entirely -- the board would come back running the previous code.
        mp_debug_file_finish_put();
        mp_debug_port_storage_flush();

        // Let the ACK reach the host before the link drops.
        mp_hal_delay_us(50000);

        // Detaches USB and does not return.
        mp_debug_port_reset();
    }
}

// Called from MICROPY_VM_HOOK_LOOP, which the VM reaches on every branch.
//
// This pumps the channel on a counter and is NOT gated on mp_debug_armed. It
// has to be: a program in a tight loop --
//
//     while True:
//         x = x + 1
//
// never reaches the idle path, so the event hook never fires, and gating this
// on the armed flag left the device unreachable until the program happened to
// sleep.  That is the trap in its general form; the sleep case is handled by
// the idle hook, and this counter handles the busy-loop case.
//
// The counter keeps the cost proportional. Polling on every branch would be far
// too expensive, but one in 256 is a few cycles amortised, and still far more
// often than a host can notice.
//
// The expensive per-instruction breakpoint check stays gated on the armed flag;
// that is a different hook.
#define MP_DEBUG_VM_POLL_INTERVAL (256)

void mp_debug_vm_hook(void) {
    static uint32_t ticks;
    mp_debug_vm_hook_calls++;
    if (mp_debug_armed) {
        mp_debug_poll();
    } else if (++ticks >= MP_DEBUG_VM_POLL_INTERVAL) {
        ticks = 0;
        mp_debug_poll();
    }
}

// Called from MICROPY_INTERNAL_EVENT_HOOK: the idle/sleep path, reached from
// mp_event_handle_nowait() whenever the REPL waits for input or a script sleeps.
//
// Deliberately NOT gated on mp_debug_armed.  The armed flag exists to keep the
// VM hot path cheap; the idle path is not hot, and the debug channel has to stay
// answerable so a host can connect to a board that is merely sitting at the REPL.
void mp_debug_event_hook(void) {
    mp_debug_poll();
}

// Report a breakpoint hit, then halt.  Called from the VM hook, so we are deep
// inside the interpreter: the halt loop pumps the channel and never unwinds,
// which is exactly what keeps the Python stack intact for the host to inspect.
void mp_debug_send_stopped(uint32_t reason, uint32_t index, uint32_t line, const char *file) {
    uint8_t payload[104];
    uint32_t file_len = file ? strlen(file) : 0;
    if (file_len > sizeof(payload) - 14) {
        file_len = sizeof(payload) - 14;
    }
    memcpy(payload + 0, &reason, 4);
    memcpy(payload + 4, &index, 4);
    memcpy(payload + 8, &line, 4);
    uint16_t fl = (uint16_t)file_len;
    memcpy(payload + 12, &fl, 2);
    if (file_len) {
        memcpy(payload + 14, file, file_len);
    }
    uint32_t total = 14 + file_len;

    wp_packet_t out;
    wp_build_event(&mp_debug_rx, MP_DBG_CMD_EXECUTION_STOPPED,
        WP_FLAG_NON_CRITICAL, payload, total, &out);
    mp_debug_send(&out, payload, total);
}

// Forward device stdout to the host as an event, so print() lands in the Debug
// Console instead of only on the REPL.  In dual-CDC mode the REPL is a separate
// port the debugger UI never sees, so without this the user's own output is
// invisible while debugging.
//
// Only while attached: an unattached board must not pay for this, and nothing
// would be reading the other end.
// Largest output payload sent in one go. Chosen so header plus payload fits
// inside the CDC TX buffer's half-empty threshold, which is what makes the
// "is there room" test below meaningful.
#define MP_DBG_OUTPUT_CHUNK (80)

static bool mp_debug_output_dropped = false;

static void mp_debug_output_chunk(const char *str, uint32_t len) {
    wp_packet_t out;
    wp_build_event(&mp_debug_rx, MP_DBG_CMD_MONITOR_OUTPUT,
        WP_FLAG_NON_CRITICAL, str, len, &out);
    mp_debug_send(&out, str, len);
}

void mp_debug_stdout(const char *str, size_t len) {
    // Boards with a REPL (SC13048Q, ESP32, RP2040) drop output when the host
    // is not attached -- their REPL channel is the primary output and the
    // debug channel is dedicated to the debugger protocol.
    //
    // Single-channel boards (STM32C071) have no REPL: the debug channel is
    // the only path.  Dropping when unattached would lose startup prints
    // during the attach race, so those boards define
    // MP_DEBUG_STDOUT_ALWAYS_SEND to skip this gate.
#ifndef MP_DEBUG_STDOUT_ALWAYS_SEND
    if (!(mp_debug_conditions & MP_DBG_COND_ATTACHED) || len == 0) {
        return;
    }
#else
    if (len == 0) {
        return;
    }
#endif
    // A send must never re-enter here. Nothing on this path prints today, but
    // an assert or error message added to the transmit path later would
    // otherwise recurse until the stack gave out.
    if (mp_debug_in_output) {
        return;
    }
    if (!mp_debug_port_link_up()) {
        return;
    }
    mp_debug_in_output = true;

    // Say so, once, rather than let output vanish silently.
    if (mp_debug_output_dropped && mp_debug_port_tx_half_empty()) {
        static const char marker[] = "\n[debug output dropped]\n";
        mp_debug_output_dropped = false;
        mp_debug_output_chunk(marker, sizeof(marker) - 1);
    }

    while (len > 0) {
        // Drop rather than block. The underlying write waits for the host to
        // drain, so a host that stops reading -- a minimised window, a modal
        // dialog -- would otherwise stall the program inside print(). Losing
        // diagnostic output is a far smaller harm than hanging the board, and
        // this was the last way the debugger could wedge a working program.
        if (!mp_debug_port_tx_half_empty()) {
            mp_debug_output_dropped = true;
            break;
        }
        uint32_t chunk = len > MP_DBG_OUTPUT_CHUNK
            ? MP_DBG_OUTPUT_CHUNK : (uint32_t)len;
        mp_debug_output_chunk(str, chunk);
        str += chunk;
        len -= chunk;
    }

    mp_debug_in_output = false;
}

// Report a stop and halt.  Called from the VM hook, so we are deep inside the
// interpreter: the halt loop pumps the channel and never unwinds, which is what
// keeps the Python stack intact for the host to inspect.
// Which thread owns the current stop.  0 means nobody is halted.
//
// The pause check in mp_debug_instr_tick() reads a global, so on a port with
// real threads every thread halts as soon as any one of them does -- all-stop
// happens by itself (12.3a).  What does not happen by itself is agreement about
// whose stop it is.  Exactly one thread owns it: it reports the location, its
// frame is the one the host walks, and it alone drives the transport.  The rest
// park in silence.  Before this existed the second thread to arrive reported a
// duplicate stop, overwrote the frame pointer so the host was handed the wrong
// thread's stack, and entered the pump alongside the owner -- two threads inside
// a USB stack that is not SMP-safe.
static volatile mp_uint_t mp_debug_halt_owner = 0;

#if MICROPY_PY_THREAD
#define MP_DEBUG_SELF() (mp_thread_get_id())
#else
// Without threads there is one context, and it is always the owner.  Any
// non-zero value will do; 0 is reserved for "nobody".
#define MP_DEBUG_SELF() ((mp_uint_t)1)
#endif

// Take ownership of the stop, or report that another thread already has it.
// True also when this thread is re-entering its own halt, which the evaluation
// path can do.
bool mp_debug_claim_halt(void) {
    const mp_uint_t self = MP_DEBUG_SELF();
    mp_uint_t owner;

    // Test-and-set has to be atomic: without a GIL two cores reach here at once.
    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();
    owner = mp_debug_halt_owner;
    if (owner == 0) {
        mp_debug_halt_owner = self;
    }
    MICROPY_END_ATOMIC_SECTION(atomic_state);

    return owner == 0 || owner == self;
}

void mp_debug_release_halt(void) {
    mp_debug_halt_owner = 0;
}

// Where a non-owning thread waits.  It must NOT pump the transport: that is the
// whole point of electing an owner.
void mp_debug_park(void) {
    while (mp_debug_conditions & MP_DBG_COND_STOPPED) {
    }
}

void mp_debug_stop(uint32_t reason, uint32_t index, uint32_t line, const char *file) {
    // Handles from the previous stop are meaningless now: execution moved on and
    // the objects they named may be gone.
    mp_debug_handles_reset();
    mp_debug_conditions |= MP_DBG_COND_STOPPED;
    mp_debug_update_armed();
    mp_debug_send_stopped(reason, index, line, file);
    mp_debug_halt_loop();
}

// Halt: pump the debug channel and do not unwind.  Returns once the host
// clears MP_DBG_COND_STOPPED via Execution_ChangeConditions.
//
// Bring-up escape hatch: Ctrl-C on CDC0 (the REPL) releases the halt, so a board
// waiting for a host that never connects is not bricked.  It must be Ctrl-C
// specifically and not merely "any byte": a stray newline left in the REPL buffer,
// or a host probing whether the REPL is alive, would otherwise release the halt
// and silently defeat the primitive.  Input arriving during a halt is consumed.
void mp_debug_halt_loop(void) {
    mp_debug_halts++;
    mp_debug_in_halt = true;

    // Discard anything already buffered on the REPL before halting, so leftovers
    // from the command that triggered the reset cannot release us immediately.
    while (mp_debug_port_repl_getc() >= 0) {
    }

    while (mp_debug_conditions & MP_DBG_COND_STOPPED) {
        mp_debug_pump();
        if (mp_debug_port_repl_getc() == 0x03) {
            mp_debug_conditions &= ~MP_DBG_COND_STOPPED;
        }
    }

    mp_debug_in_halt = false;
}

// Run before main.py: pick up a request that had to survive a hard reset, and
// halt at entry when the host asked for it.  A port calls this from whatever
// hook it has that runs after USB is up and before the first bytecode.
void mp_debug_before_main_py(void) {
    mp_debug_conditions |= mp_debug_port_persist_take();

    if (mp_debug_conditions & MP_DBG_COND_STOP_ON_START) {
        // One-shot: consume the request so a host that never resumes cannot
        // wedge the board on every subsequent reset.
        mp_debug_conditions &= ~MP_DBG_COND_STOP_ON_START;
        mp_debug_conditions |= MP_DBG_COND_STOPPED;
        mp_debug_update_armed();
        // Tell the host we are halted at entry rather than making it poll.
        mp_debug_send_stopped(MP_DBG_STOP_ENTRY, 0, 0, NULL);
        mp_debug_in_entry_halt = true;
        mp_debug_halt_loop();
        mp_debug_in_entry_halt = false;
    }
}

// True when the host asked for a clean soft reset rather than a run of main.py:
// no exception, no USB re-enumeration, the host keeps its connection across the
// restart.  Performs the filesystem cleanup that owes either way, so a caller
// that gets true should go straight to its port's soft-reset exit.
bool mp_debug_take_soft_reset(void) {
    if (!mp_debug_reboot_pending || mp_debug_reboot_hard) {
        return false;
    }
    mp_debug_reboot_pending = false;
    mp_debug_file_finish_put();
    mp_debug_port_storage_flush();
    return true;
}

// Run after main.py returns.  DAP expects a terminated event; without it VS Code
// sits showing a running program that has already exited.
void mp_debug_after_main_py(void) {
    if (mp_debug_conditions & MP_DBG_COND_ATTACHED) {
        mp_debug_send_stopped(MP_DBG_STOP_EXITED, 0, 0, NULL);
    }
}

/******************************************************************************/
// Python bindings.
//
// Bring-up scaffolding: these expose debugger internals to user code, which a
// shipping build has no reason to offer. Off by default; set
// MICROPY_PY_MPDEBUG_MODULE to 1 in a board config to get them back for
// diagnostics.
#if MICROPY_PY_MPDEBUG_MODULE

static mp_obj_t mpdebug_enable(mp_obj_t on) {
    mp_debug_init();
    mp_debug_armed = mp_obj_is_true(on);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpdebug_enable_obj, mpdebug_enable);

static mp_obj_t mpdebug_poll(void) {
    mp_debug_poll();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpdebug_poll_obj, mpdebug_poll);

static mp_obj_t mpdebug_stats(void) {
    mp_obj_t t[4] = {
        mp_obj_new_int_from_uint(mp_debug_msgs_in),
        mp_obj_new_int_from_uint(mp_debug_msgs_out),
        mp_obj_new_int_from_uint(mp_debug_halts),
        mp_obj_new_int_from_uint(mp_debug_conditions),
    };
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mpdebug_stats_obj, mpdebug_stats);

static mp_obj_t mpdebug_crc(mp_obj_t buf_in) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    return mp_obj_new_int_from_uint(wp_crc(bufinfo.buf, bufinfo.len, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpdebug_crc_obj, mpdebug_crc);

static mp_obj_t mpdebug_conditions(size_t n_args, const mp_obj_t *args) {
    if (n_args >= 1) {
        mp_debug_conditions = mp_obj_get_int(args[0]);
    }
    return mp_obj_new_int_from_uint(mp_debug_conditions);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpdebug_conditions_obj, 0, 1, mpdebug_conditions);

static mp_obj_t mpdebug_stop_at_start(mp_obj_t on) {
    if (mp_obj_is_true(on)) {
        mp_debug_conditions |= MP_DBG_COND_STOP_ON_START;
    } else {
        mp_debug_conditions &= ~MP_DBG_COND_STOP_ON_START;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpdebug_stop_at_start_obj, mpdebug_stop_at_start);

static const mp_rom_map_elem_t mpdebug_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mpdebug) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&mpdebug_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&mpdebug_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&mpdebug_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_crc), MP_ROM_PTR(&mpdebug_crc_obj) },
    { MP_ROM_QSTR(MP_QSTR_conditions), MP_ROM_PTR(&mpdebug_conditions_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop_at_start), MP_ROM_PTR(&mpdebug_stop_at_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_COND_STOPPED), MP_ROM_INT(MP_DBG_COND_STOPPED) },
    { MP_ROM_QSTR(MP_QSTR_COND_STOP_ON_START), MP_ROM_INT(MP_DBG_COND_STOP_ON_START) },
};
static MP_DEFINE_CONST_DICT(mpdebug_module_globals, mpdebug_module_globals_table);

const mp_obj_module_t mp_module_mpdebug = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mpdebug_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mpdebug, mp_module_mpdebug);

#endif // MICROPY_PY_MPDEBUG_MODULE
