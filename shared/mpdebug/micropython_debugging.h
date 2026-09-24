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
 * Command IDs for the MicroPython source-level debug protocol.
 *
 * The command numbering derives from the .NET Micro Framework debug protocol.
 */
#ifndef MICROPY_INCLUDED_MPDEBUG_DEBUGGING_H
#define MICROPY_INCLUDED_MPDEBUG_DEBUGGING_H

// Monitor commands (0x0000xxxx)
#define MP_DBG_CMD_MONITOR_PING         (0x00000000)
#define MP_DBG_CMD_MONITOR_OUTPUT       (0x00000001)   // device -> host event
#define MP_DBG_CMD_MONITOR_REBOOT       (0x00000007)
// Ask the port to leave the running firmware and enter its ROM DFU / update
// loader on the next boot. Not every port has a DFU path (rp2 uses BOOTSEL,
// esp32 uses its ROM loader over UART), so the default port hook is a no-op
// and the host relies on the manifest kind to know when to send this.
#define MP_DBG_CMD_MONITOR_ENTER_DFU    (0x00000008)

// Ping payload, both directions: { uint32_t source; uint32_t dbg_flags; }
#define MP_DBG_PING_SOURCE_DEVICE       (0x00000000)
#define MP_DBG_PING_SOURCE_HOST         (0x00000001)

#define MP_DBG_PING_DBGFLAG_STOP        (0x00000001)

// Monitor_Reboot options.  A soft reset restarts boot.py/main.py with fresh VM
// state without re-enumerating USB, so the host keeps its connection across the
// restart; a hard reset does re-enumerate and the host must reconnect.
#define MP_DBG_REBOOT_SOFT              (0x00000000)
#define MP_DBG_REBOOT_WAIT_FOR_DEBUGGER (0x00000001)
#define MP_DBG_REBOOT_HARD              (0x00000002)

typedef struct _mp_dbg_reboot_t {
    uint32_t flags;
} mp_dbg_reboot_t;

// Execution commands (0x0002xxxx)
#define MP_DBG_CMD_EXECUTION_CHANGE_CONDITIONS  (0x00020001)
// Execution_Capabilities: reply { uint16 protocol, uint16 max_breakpoints,
//                                 uint16 max_payload, uint16 max_value_len }
// The host should size its requests from this rather than hardcoding limits.
#define MP_DBG_CMD_EXECUTION_CAPABILITIES       (0x00020008)
// Bumped on any change the host needs to know about, including behaviour
// fixes -- not only wire-format changes. Being able to tell which firmware
// is on a board turns arguments into observations.
#define MP_DBG_PROTOCOL_VERSION                 (10)

// Sizing limits. Guarded so a board with more memory can raise them from its
// mpconfigboard.h without editing shared debugger code: these are all fixed
// buffers in .bss, so the right value is a property of the part, not of the
// protocol. The host already reads them back from Execution_Capabilities.
#ifndef MP_DBG_MAX_BREAKPOINTS
#define MP_DBG_MAX_BREAKPOINTS                  (16)
#endif
#ifndef MP_DBG_VALUE_MAX
#define MP_DBG_VALUE_MAX                        (128)
#endif
// Longest breakpoint path the device stores. Matching is by suffix, so this
// bounds the tail that has to be distinctive, not the full path -- but nested
// third-party packages get long, and a path that did not fit used to reject
// the whole request.
#ifndef MP_DBG_FILE_MATCH_MAX
#define MP_DBG_FILE_MATCH_MAX                   (128)
#endif

#define MP_DBG_CMD_EXECUTION_BREAKPOINTS        (0x00020005)

// Execution_Step: request { uint32 mode }, reply { int32 result }.
// Issuing a step implicitly resumes: the VM runs until the step condition is
// met, then reports Execution_Stopped with reason=step.
#define MP_DBG_CMD_EXECUTION_STEP               (0x00020003)
#define MP_DBG_STEP_NONE                (0)
#define MP_DBG_STEP_IN                  (1)
#define MP_DBG_STEP_OVER                (2)
#define MP_DBG_STEP_OUT                 (3)
#define MP_DBG_CMD_EXECUTION_STOPPED            (0x00020006)   // device -> host event

// Why the VM stopped.  DAP's `stopped` event carries a reason string; these map
// onto it directly, so one device event covers every case.
#define MP_DBG_STOP_BREAKPOINT          (0)
#define MP_DBG_STOP_PAUSE               (1)
#define MP_DBG_STOP_STEP                (2)
#define MP_DBG_STOP_EXCEPTION           (3)
#define MP_DBG_STOP_ENTRY               (4)
#define MP_DBG_STOP_EXITED              (5)

// Execution_Stopped payload:
//   uint32 reason, uint32 index, uint32 line, uint16 file_len, char file[]

// Thread commands (0x0002001x)
#define MP_DBG_CMD_THREAD_LIST                  (0x00020010)
#define MP_DBG_CMD_THREAD_STACK                 (0x00020011)

// Value commands (0x000200 3x).
//
// Value_GetScope    request { uint32 frame, uint32 scope, uint32 start }
// Value_GetChildren request { uint32 handle, uint32 start }
// both reply        { uint16 count, uint16 more, then count entries of
//                     uint16 name_len, name, uint16 value_len, value,
//                     uint32 handle }
//
// `start` and `more` exist because a reply has to fit one packet. Without
// them a module with many globals, or one long value, would silently lose
// entries off the end; the host instead asks again from where it left off.
#define MP_DBG_CMD_VALUE_GET_SCOPE              (0x00020030)
#define MP_DBG_CMD_VALUE_EVALUATE               (0x00020031)
#define MP_DBG_CMD_VALUE_GET_CHILDREN           (0x00020032)

// Value_SetVariable: request { uint32 frame, uint16 name_len, name,
//                              uint16 expr_len, expr }
//                    reply   { int32 rc, uint16 len, new value }
#define MP_DBG_CMD_VALUE_SET_VARIABLE           (0x00020033)

#define MP_DBG_SCOPE_LOCALS             (0)
#define MP_DBG_SCOPE_GLOBALS            (1)

// Debugger condition bits.
// MP_DBG_COND_STOPPED     - the VM is halted; the halt loop pumps and does not unwind.
// MP_DBG_COND_STOP_ON_START - halt before the first bytecode of main.py on the next
//                             (soft) reset.  Without this F5 is a race: the script
//                             can finish before the host has sent setBreakpoints.
#define MP_DBG_COND_STOPPED             (0x00000001)
#define MP_DBG_COND_STOP_ON_START       (0x00000002)
// A debug session is attached.  Keeps the VM hook live even with no breakpoints
// set, which is what pause and stepping need.  Without this the hook is only
// armed while breakpoints exist and pause can never take effect.
#define MP_DBG_COND_ATTACHED            (0x00000004)

// Execution_ChangeConditions: request { set, reset }, reply { current }.
typedef struct _mp_dbg_change_conditions_t {
    uint32_t set;
    uint32_t reset;
} mp_dbg_change_conditions_t;

typedef struct _mp_dbg_ping_t {
    uint32_t source;
    uint32_t dbg_flags;
} mp_dbg_ping_t;

// File commands (0x0003xxxx).  Sync .py source from the host to the device VFS.
#define MP_DBG_CMD_FILE_PUT             (0x00030000)
#define MP_DBG_CMD_FILE_CRC             (0x00030001)
#define MP_DBG_CMD_FILE_DELETE          (0x00030002)
#define MP_DBG_CMD_FILE_MKDIR           (0x00030003)

// File_List: request { uint16 path_len, path, uint32 start }
//            reply   { uint16 count, uint16 more, then per entry:
//                      uint16 name_len, name, uint8 is_dir }
// Lets the host see what is actually on the device, so a deploy can remove
// files the workspace no longer has instead of leaving them to be imported.
#define MP_DBG_CMD_FILE_LIST            (0x00030004)

// File_Stat: request { uint16 path_len, path }
//            reply   { int32 rc, uint32 block_size, uint32 total_blocks,
//                      uint32 free_blocks }
// The filesystem is small enough that a library project can fill it, and
// without this the first sign of that is a write failing part-way through a
// deploy. The host can now say so before it starts.
#define MP_DBG_CMD_FILE_STAT            (0x00030005)

#define MP_DBG_FILE_FLAG_FIRST          (0x00000001)   // truncate/create
#define MP_DBG_FILE_FLAG_LAST           (0x00000002)   // final chunk

#define MP_DBG_FILE_ERR_FAILED          (-1)
#define MP_DBG_FILE_ERR_NOT_FOUND       (-2)
#define MP_DBG_FILE_ERR_BAD_REQUEST     (-3)

// File_Put:    header, then name[name_len], then data[data_len].
typedef struct __attribute__((packed)) _mp_dbg_file_put_t {
    uint32_t flags;
    uint16_t name_len;
    uint16_t data_len;
} mp_dbg_file_put_t;

// File_Crc / File_Delete: header, then name[name_len].
typedef struct __attribute__((packed)) _mp_dbg_file_name_t {
    uint16_t name_len;
} mp_dbg_file_name_t;

#endif // MICROPY_INCLUDED_MPDEBUG_DEBUGGING_H
