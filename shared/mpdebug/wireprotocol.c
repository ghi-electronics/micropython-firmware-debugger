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
#include "shared/mpdebug/wireprotocol.h"

#define WP_CRC_POLY (0x04C11DB7u)

uint32_t wp_crc(const void *data, size_t len, uint32_t crc) {
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= (uint32_t)(*p++) << 24;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ WP_CRC_POLY) : (crc << 1);
        }
    }
    return crc;
}

// The header CRC is computed with the CRC field itself set to zero.
static uint32_t wp_header_crc(const wp_packet_t *h) {
    wp_packet_t tmp = *h;
    tmp.crc_header = 0;
    return wp_crc(&tmp, sizeof(tmp), 0);
}

bool wp_verify_header(const wp_packet_t *h) {
    return wp_header_crc(h) == h->crc_header;
}

void wp_receiver_reset(wp_receiver_t *r) {
    r->state = WP_RX_SYNC;
    r->got = 0;
    r->payload_got = 0;
}

void wp_receiver_init(wp_receiver_t *r) {
    memset(r, 0, sizeof(*r));
    wp_receiver_reset(r);
}

void wp_receive(wp_receiver_t *r, const uint8_t *buf, size_t len, wp_dispatch_t cb, void *ctx) {
    static const char marker[WP_MARKER_SIZE] = WP_MARKER_PACKET;
    uint8_t *hp = (uint8_t *)&r->header;

    while (len--) {
        uint8_t b = *buf++;
        switch (r->state) {
            case WP_RX_SYNC:
                if (b == (uint8_t)marker[r->got]) {
                    hp[r->got++] = b;
                    if (r->got == WP_MARKER_SIZE) {
                        r->state = WP_RX_HEADER;
                    }
                } else {
                    // Resync.  Re-test this byte as a possible start of marker
                    // so that "GG HIPKT1..." does not lose the real header.
                    r->got = 0;
                    if (b == (uint8_t)marker[0]) {
                        hp[r->got++] = b;
                    }
                }
                break;

            case WP_RX_HEADER:
                hp[r->got++] = b;
                if (r->got == sizeof(wp_packet_t)) {
                    if (!wp_verify_header(&r->header) || r->header.size > WP_MAX_PAYLOAD) {
                        wp_receiver_reset(r);
                    } else if (r->header.size == 0) {
                        if (r->header.crc_data == 0) {
                            cb(ctx, &r->header, NULL);
                        }
                        wp_receiver_reset(r);
                    } else {
                        r->payload_got = 0;
                        r->state = WP_RX_PAYLOAD;
                    }
                }
                break;

            case WP_RX_PAYLOAD:
                r->payload[r->payload_got++] = b;
                if (r->payload_got == r->header.size) {
                    if (wp_crc(r->payload, r->header.size, 0) == r->header.crc_data) {
                        cb(ctx, &r->header, r->payload);
                    }
                    wp_receiver_reset(r);
                }
                break;
        }
    }
}

void wp_build_reply(wp_receiver_t *r, const wp_packet_t *req, uint32_t flags,
    const void *payload, uint32_t size, wp_packet_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->signature, WP_MARKER_PACKET, WP_MARKER_SIZE);
    out->crc_data = wp_crc(payload, size, 0);
    out->cmd = req->cmd;
    out->seq = r->seq_out++;
    out->seq_reply = req->seq;
    out->flags = flags | WP_FLAG_REPLY;
    out->size = size;
    out->crc_header = 0;
    out->crc_header = wp_crc(out, sizeof(*out), 0);
}

void wp_build_event(wp_receiver_t *r, uint32_t cmd, uint32_t flags,
    const void *payload, uint32_t size, wp_packet_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->signature, WP_MARKER_PACKET, WP_MARKER_SIZE);
    out->crc_data = wp_crc(payload, size, 0);
    out->cmd = cmd;
    out->seq = r->seq_out++;
    out->seq_reply = 0;
    out->flags = flags;
    out->size = size;
    out->crc_header = 0;
    out->crc_header = wp_crc(out, sizeof(*out), 0);
}
