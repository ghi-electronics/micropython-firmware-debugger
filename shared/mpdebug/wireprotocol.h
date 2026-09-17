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
 * WireProtocol framing for the source-level debug channel.
 *
 * The framing derives from the .NET Micro Framework debug protocol.
 */
#ifndef MICROPY_INCLUDED_MPDEBUG_WIREPROTOCOL_H
#define MICROPY_INCLUDED_MPDEBUG_WIREPROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WP_MARKER_PACKET    "MPYDBG1"   // 7 chars + NUL, stuffed into 8 bytes
#define WP_MARKER_SIZE      (8)

// Largest payload we will accept.  Kept small deliberately: internal RAM is
// the scarce resource on small MCUs, and every command in the protocol fits.
#ifndef WP_MAX_PAYLOAD
#define WP_MAX_PAYLOAD      (512)
#endif

// Packet flags.
#define WP_FLAG_NON_CRITICAL    (0x0001)
#define WP_FLAG_REPLY           (0x0002)
#define WP_FLAG_BAD_HEADER      (0x0004)
#define WP_FLAG_BAD_PAYLOAD     (0x0008)
#define WP_FLAG_NACK            (0x4000)
#define WP_FLAG_ACK             (0x8000)

// 32-byte header.  Field order and widths must not change.
typedef struct __attribute__((packed)) _wp_packet_t {
    uint8_t  signature[WP_MARKER_SIZE];
    uint32_t crc_header;
    uint32_t crc_data;
    uint32_t cmd;
    uint16_t seq;
    uint16_t seq_reply;
    uint32_t flags;
    uint32_t size;
} wp_packet_t;

typedef enum {
    WP_RX_SYNC = 0,
    WP_RX_HEADER,
    WP_RX_PAYLOAD,
} wp_rx_state_t;

// Called for each complete, CRC-verified message.  payload is NULL when size==0.
typedef void (*wp_dispatch_t)(void *ctx, const wp_packet_t *header, const uint8_t *payload);

typedef struct _wp_receiver_t {
    wp_rx_state_t state;
    wp_packet_t header;
    uint32_t got;               // bytes of header received so far
    uint32_t payload_got;
    uint16_t seq_out;           // our outbound sequence counter
    uint8_t payload[WP_MAX_PAYLOAD];
} wp_receiver_t;

// CRC-32, MSB-first, poly 0x04C11DB7, no reflection, no final xor.
// Computed without a lookup table to keep 1 KB out of flash.
uint32_t wp_crc(const void *data, size_t len, uint32_t crc);

void wp_receiver_init(wp_receiver_t *r);
void wp_receiver_reset(wp_receiver_t *r);
bool wp_verify_header(const wp_packet_t *h);

// Feed received bytes; invokes cb once per complete message.
void wp_receive(wp_receiver_t *r, const uint8_t *buf, size_t len, wp_dispatch_t cb, void *ctx);

// Build a reply header for a request. Does not transmit.
void wp_build_reply(wp_receiver_t *r, const wp_packet_t *req, uint32_t flags,
    const void *payload, uint32_t size, wp_packet_t *out);

// Build an unsolicited message (an event such as BreakpointHit). Same framing as
// a reply but with no request to answer, so seq_reply is 0 and c_Reply is unset.
void wp_build_event(wp_receiver_t *r, uint32_t cmd, uint32_t flags,
    const void *payload, uint32_t size, wp_packet_t *out);

#endif // MICROPY_INCLUDED_MPDEBUG_WIREPROTOCOL_H
