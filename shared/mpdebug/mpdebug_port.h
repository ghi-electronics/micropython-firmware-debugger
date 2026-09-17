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
 * Everything the debug engine needs from a port.
 *
 * The engine itself is written against py/ APIs only.  Every call that touches
 * a particular chip goes through this header, so bringing the debugger up on a
 * new port means implementing these ten functions and nothing else.
 */

#ifndef MICROPY_INCLUDED_MPDEBUG_PORT_H
#define MICROPY_INCLUDED_MPDEBUG_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------- debug channel ---

// False while the channel does not exist -- USB not enumerated yet, say.  The
// other transport calls below may assume the caller has checked this.
bool mp_debug_port_link_up(void);

// Bytes waiting on the debug channel, or 0.
int mp_debug_port_rx_avail(void);

// Read up to len bytes.  Returns the count read, 0 for none.
int mp_debug_port_rx(uint8_t *buf, size_t len);

// Write len bytes, blocking until every one has gone.  This must not
// short-write: the framing layer above relies on a packet leaving whole and so
// never concerns itself with the transport's buffer size.
void mp_debug_port_tx_always(const uint8_t *buf, size_t len);

// True when the transmit buffer has room to spare.  Only used to decide whether
// a "output dropped" marker can be afforded, so an approximation is fine.
bool mp_debug_port_tx_half_empty(void);

// ----------------------------------------------------------- REPL channel ---

// One byte from the REPL channel, or -1 when nothing is waiting.  This exists
// solely for the halt loop's Ctrl-C escape hatch, so that a board halted for a
// host that never connects is not bricked.
int mp_debug_port_repl_getc(void);

// ------------------------------------------------- state outliving a reset ---

// Remember the conditions to resume with after the reset that is about to
// happen; only the low 8 bits are kept.  The port must tag the value so that
// unrelated content left in the same place cannot be read back as a request.
void mp_debug_port_persist_write(uint32_t conditions);

// Take those conditions, or 0 if none were stored.  One-shot: a second call
// returns 0.
uint32_t mp_debug_port_persist_take(void);

// ------------------------------------------------------- storage and reset ---

// Push any cached filesystem writes out to the medium.
void mp_debug_port_storage_flush(void);

// Detach USB, then reset the board.  Does not return.
//
// The detach is not optional: resetting with the D+ pull-up still asserted
// leaves the host holding a device object that no longer matches the board that
// comes back, and every subsequent open fails until it is physically replugged.
void mp_debug_port_reset(void);

#endif // MICROPY_INCLUDED_MPDEBUG_PORT_H
