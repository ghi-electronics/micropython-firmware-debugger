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
 * Source-level debugger configuration for every esp32 board.
 *
 * Deliberately not per board.  These hooks have to arrive as a set -- with
 * MICROPY_DEBUG_INSTR_HOOK missing, the engine halts at entry and answers the
 * channel while breakpoints silently never fire.  Copying the set into each
 * board's mpconfigboard.h is how that happens again, so it lives in one
 * place and mpconfigport.h includes it.
 *
 * A board that must not have the debugger can define MICROPY_HW_MPDEBUG to 0
 * before this is reached.
 */

#ifndef MICROPY_INCLUDED_ESP32_MPDEBUG_BOARD_H
#define MICROPY_INCLUDED_ESP32_MPDEBUG_BOARD_H

// Only parts with USB OTG can present a second CDC interface; the rest have a
// fixed-function USB Serial/JTAG peripheral or no USB device at all.
#ifndef MICROPY_HW_MPDEBUG
#define MICROPY_HW_MPDEBUG (MICROPY_HW_ENABLE_USBDEV)
#endif

#if MICROPY_HW_MPDEBUG
// Second CDC interface for the source-level debugger.  CDC 0 stays the REPL and
// behaves exactly as stock; CDC 1 carries the debug protocol.
#define MICROPY_HW_USB_CDC_NUM                  (2)

// Breakpoint check on the VM per-instruction path.  One flag test while the
// debugger is idle; the table is only consulted once something is armed.
#define MICROPY_VM_HOOK_LOOP { extern void mp_debug_vm_hook(void); mp_debug_vm_hook(); }

// Keep the debug channel answerable while the board is idle (REPL waiting for
// input, or a script sleeping).  Runs unconditionally -- see mp_debug_event_hook().
#define MICROPY_INTERNAL_EVENT_HOOK do { extern void mp_debug_event_hook(void); mp_debug_event_hook(); } while (0)

// Source-level debugging needs the line tables, source-file names and frame
// metadata that settrace brings in.  The Python callback is never installed;
// only the metadata and hook site are wanted.
#define MICROPY_PY_SYS_SETTRACE (1)

// Halt before the first bytecode of main.py when the host has asked for it.
// These are the port-neutral phases the debug engine exposes; stm32 wraps the
// same three in its boardctrl hook.
extern void mp_debug_before_main_py(void);
extern bool mp_debug_take_soft_reset(void);
extern void mp_debug_after_main_py(void);
#define MICROPY_BOARD_BEFORE_MAIN_PY() mp_debug_before_main_py()
#define MICROPY_BOARD_SKIP_MAIN_PY() mp_debug_take_soft_reset()
#define MICROPY_BOARD_AFTER_MAIN_PY() mp_debug_after_main_py()

// Breakpoint check on the VM per-instruction path.  One flag test while the
// debugger is not armed.  MICROPY_VM_HOOK_LOOP
// above is NOT a substitute: that one only runs after jump opcodes, which is
// enough to pump the channel but not to stop on an arbitrary line.
struct _mp_code_state_t;
void mp_debug_instr_tick(struct _mp_code_state_t *code_state);
extern volatile _Bool mp_debug_armed;
#define MICROPY_DEBUG_INSTR_HOOK(cs) do { if (mp_debug_armed) { mp_debug_instr_tick(cs); } } while (0)

// Forward program output to the debug channel so print() reaches the Debug
// Console.  CDC 0 is a port the debugger UI never looks at.
void mp_debug_stdout(const char *str, unsigned int len);
#define MICROPY_DEBUG_STDOUT_HOOK(str, len) mp_debug_stdout((str), (len))

// Halt at the raise point of an exception nothing can catch.
void mp_debug_exception(const struct _mp_code_state_t *cs, void *exc);
#define MICROPY_DEBUG_EXC_HOOK(cs, exc) do { if (mp_debug_armed) { mp_debug_exception((cs), (exc)); } } while (0)

#endif // MICROPY_HW_MPDEBUG

#endif // MICROPY_INCLUDED_ESP32_MPDEBUG_BOARD_H
