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
 * File transfer over the debug channel.
 *
 * F5 pushes the workspace to the device over this protocol rather than over
 * the REPL: raw-paste-style transfer fights the user's REPL, and in dual-CDC
 * mode there may be no mass-storage drive to fall back on.
 *
 * Everything here runs from the debug pump, which is reached from the idle
 * hook.  Filesystem calls block on flash erase/program, and a blocking call
 * can re-enter the engine through that hook, so mp_debug_poll() guards
 * against reentrancy and nothing in this file may delay with mp_hal_delay_ms().
 */
#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/stream.h"
#include "py/objstr.h"
#include "extmod/vfs.h"
#include "shared/mpdebug/mpdebug_port.h"

#include "shared/mpdebug/mpdebug.h"
#include "shared/mpdebug/micropython_debugging.h"

// Longest path we accept.  Kept small on purpose: this lands in .bss.
#define MP_DBG_FILE_NAME_MAX (128)

static bool mp_debug_file_name(const uint8_t *payload, uint32_t size,
    uint32_t name_off, uint16_t name_len, char *out) {
    if (name_len == 0 || name_len >= MP_DBG_FILE_NAME_MAX) {
        return false;
    }
    if (name_off + name_len > size) {
        return false;
    }
    memcpy(out, payload + name_off, name_len);
    out[name_len] = '\0';
    return true;
}

/*
 * A File_Put transfer holds its file open across chunks.
 *
 * Each chunk used to be a complete open/write/close. FatFs closes by syncing
 * the file's data sector *and* its directory entry -- two different flash
 * locations, and flashbdev caches only one erase unit, so each close costs an
 * erase-and-program either way. Measured on an STM32L4 with FatFs on internal
 * flash: ~140 ms per chunk, identical for a 64-byte chunk and a 470-byte one,
 * which dominates the whole deploy time. 8 KB took 3.0 s across 18 chunks,
 * while reading the same 8 KB back took 11 ms.
 *
 * Holding the handle makes that one open/close per file rather than per chunk.
 *
 * The handle is a GC root: it lives between commands, and the collector runs
 * in that gap. The name is kept alongside so a continuation chunk for some
 * other file is rejected rather than silently appended to whatever is open.
 */
MP_REGISTER_ROOT_POINTER(mp_obj_t mpdebug_put_file);
static char mp_debug_put_name[MP_DBG_FILE_NAME_MAX];

/*
 * Finish any transfer in progress and get it onto flash. A no-op when nothing
 * is open, so callers need not check.
 *
 * This is the safety valve for the whole scheme. A file left open across a
 * reset would lose its directory entry and the deployed code would silently
 * remain the previous version -- the "code is not updated until I reset the
 * board" failure. So it runs on the last chunk, when a new transfer starts,
 * before every other filesystem command, and before either kind of reset.
 */
void mp_debug_file_finish_put(void) {
    if (MP_STATE_VM(mpdebug_put_file) == MP_OBJ_NULL) {
        return;
    }
    mp_obj_t f = MP_STATE_VM(mpdebug_put_file);
    // Cleared before closing, so a raise cannot leave a stale handle behind.
    MP_STATE_VM(mpdebug_put_file) = MP_OBJ_NULL;
    mp_debug_put_name[0] = '\0';

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_stream_close(f);
        nlr_pop();
    }
    // Push it out of the RAM cache: flashbdev writes dirty blocks back on a
    // timer, and a deploy is followed immediately by a reset that would
    // otherwise discard them.
    mp_debug_port_storage_flush();
}

// Write one chunk. Wrapped in nlr_push because every filesystem call can raise
// (full filesystem, bad path) and an escaping exception from the debug pump
// would take down the board rather than fail the command.
static int32_t mp_debug_file_write(const char *name, bool first, bool last,
    const uint8_t *data, uint32_t len) {
    nlr_buf_t nlr;
    int32_t rc;

    if (first) {
        // A previous transfer the host never finished must not be appended to.
        mp_debug_file_finish_put();
    } else if (MP_STATE_VM(mpdebug_put_file) == MP_OBJ_NULL
               || strcmp(mp_debug_put_name, name) != 0) {
        // A continuation for a file we do not have open means host and device
        // disagree about the transfer. Failing is better than appending to the
        // wrong file or leaving a truncated one that looks complete.
        return MP_DBG_FILE_ERR_BAD_REQUEST;
    }

    if (nlr_push(&nlr) == 0) {
        if (first) {
            mp_obj_t args[2] = {
                mp_obj_new_str(name, strlen(name)),
                mp_obj_new_str("wb", 2),
            };
            mp_map_t kw;
            mp_map_init(&kw, 0);
            MP_STATE_VM(mpdebug_put_file) = mp_vfs_open(2, args, &kw);
            strcpy(mp_debug_put_name, name);
        }
        int err = 0;
        if (len != 0) {
            mp_stream_write_exactly(MP_STATE_VM(mpdebug_put_file), data, len, &err);
        }
        rc = (err == 0) ? 0 : -(int32_t)err;
        nlr_pop();
    } else {
        rc = MP_DBG_FILE_ERR_FAILED;
    }

    // Close on the final chunk, and on any failure: a half-written file is
    // better closed and reported than left open for a chunk that never comes.
    if (last || rc != 0) {
        mp_debug_file_finish_put();
    }
    return rc;
}

// CRC and size of a file, so the host can skip pushing anything unchanged.
// Section 7.2: re-pushing an unchanged project on every F5 wastes seconds over
// full-speed USB at 80 MHz.
static int32_t mp_debug_file_crc(const char *name, uint32_t *crc_out, uint32_t *size_out) {
    nlr_buf_t nlr;
    int32_t rc;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[2] = {
            mp_obj_new_str(name, strlen(name)),
            mp_obj_new_str("rb", 2),
        };
        mp_map_t kw;
        mp_map_init(&kw, 0);
        mp_obj_t f = mp_vfs_open(2, args, &kw);

        uint32_t crc = 0, total = 0;
        uint8_t buf[64];
        for (;;) {
            int err = 0;
            mp_uint_t n = mp_stream_rw(f, buf, sizeof(buf), &err, 0);
            if (err != 0 || n == 0) {
                break;
            }
            crc = wp_crc(buf, n, crc);
            total += n;
        }
        mp_stream_close(f);
        *crc_out = crc;
        *size_out = total;
        rc = 0;
        nlr_pop();
    } else {
        rc = MP_DBG_FILE_ERR_NOT_FOUND;
    }
    return rc;
}

// Create a directory. Succeeding when it already exists is deliberate: the
// host creates parents unconditionally rather than probing first, which keeps
// deployment to one round trip per directory instead of two.
static int32_t mp_debug_file_mkdir(const char *name) {
    nlr_buf_t nlr;
    int32_t rc;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_mkdir(mp_obj_new_str(name, strlen(name)));
        rc = 0;
        nlr_pop();
    } else {
        // EEXIST is success for our purposes; anything else is a real failure,
        // and the subsequent File_Put will report it.
        rc = 0;
    }
    return rc;
}

/*
 * Filesystem size, so the host can warn before a deploy rather than after.
 *
 * statvfs returns a 10-tuple; the fields wanted here are f_bsize (0),
 * f_blocks (2) and f_bavail (4), matching os.statvfs in Python.
 */
static int32_t mp_debug_file_stat(const char *name, uint32_t *bsize,
    uint32_t *total, uint32_t *avail) {
    nlr_buf_t nlr;
    int32_t rc;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t res = mp_vfs_statvfs(mp_obj_new_str(name, strlen(name)));
        size_t len;
        mp_obj_t *items;
        mp_obj_get_array(res, &len, &items);
        if (len >= 5) {
            *bsize = mp_obj_get_int(items[0]);
            *total = mp_obj_get_int(items[2]);
            *avail = mp_obj_get_int(items[4]);
            rc = 0;
        } else {
            rc = MP_DBG_FILE_ERR_FAILED;
        }
        nlr_pop();
    } else {
        rc = MP_DBG_FILE_ERR_FAILED;
    }
    return rc;
}

static int32_t mp_debug_file_delete(const char *name) {
    nlr_buf_t nlr;
    int32_t rc;
    if (nlr_push(&nlr) == 0) {
        mp_vfs_remove(mp_obj_new_str(name, strlen(name)));
        mp_debug_port_storage_flush();
        rc = 0;
        nlr_pop();
    } else {
        rc = MP_DBG_FILE_ERR_NOT_FOUND;
    }
    return rc;
}

// List a directory. Paginated like the variable commands, and for the same
// reason: one reply has to fit one packet.
static int mp_debug_file_list(const char *path, uint32_t start,
    uint8_t *buf, uint32_t buf_max) {
    uint32_t off = 4;
    uint16_t count = 0, more = 0;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[1] = { mp_obj_new_str(path, strlen(path)) };
        mp_obj_t iter = mp_vfs_ilistdir(1, args);
        mp_obj_iter_buf_t iter_buf;
        mp_obj_t iterable = mp_getiter(iter, &iter_buf);
        mp_obj_t item;
        uint32_t seen = 0;

        while ((item = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
            if (seen++ < start) {
                continue;
            }
            size_t n_fields;
            mp_obj_t *fields;
            mp_obj_tuple_get(item, &n_fields, &fields);
            if (n_fields < 2) {
                continue;
            }
            const char *name = mp_obj_str_get_str(fields[0]);
            uint16_t nlen = (uint16_t)strlen(name);
            // 0x4000 is the directory bit in the type returned by ilistdir.
            uint8_t is_dir = (mp_obj_get_int(fields[1]) & 0x4000) ? 1 : 0;

            if (off + 2 + nlen + 1 > buf_max) {
                more = 1;
                break;
            }
            memcpy(buf + off, &nlen, 2);
            off += 2;
            memcpy(buf + off, name, nlen);
            off += nlen;
            buf[off++] = is_dir;
            count++;
        }
        nlr_pop();
    } else {
        // A missing directory is an empty listing, not a failure: the host
        // asks about paths that may legitimately not exist yet.
    }

    memcpy(buf, &count, 2);
    memcpy(buf + 2, &more, 2);
    return (int)off;
}

// Handles the File_* commands.  Returns the number of reply bytes written into
// reply_buf, or -1 if cmd is not a file command.
int mp_debug_file_dispatch(uint32_t cmd, const uint8_t *payload, uint32_t size,
    uint8_t *reply_buf, uint32_t reply_max) {
    char name[MP_DBG_FILE_NAME_MAX];

    // Any command other than a File_Put chunk ends the transfer in progress
    // first. A File_Crc while the file is still open would read what has been
    // flushed rather than what was written, and report a mismatch for a file
    // that is in fact correct; delete and list have the same problem.
    if (cmd != MP_DBG_CMD_FILE_PUT) {
        mp_debug_file_finish_put();
    }

    switch (cmd) {
        case MP_DBG_CMD_FILE_PUT: {
            mp_dbg_file_put_t req;
            int32_t rc = MP_DBG_FILE_ERR_BAD_REQUEST;
            if (payload != NULL && size >= sizeof(req)) {
                memcpy(&req, payload, sizeof(req));
                if (mp_debug_file_name(payload, size, sizeof(req), req.name_len, name)
                    && sizeof(req) + req.name_len + req.data_len <= size) {
                    const uint8_t *data = payload + sizeof(req) + req.name_len;
                    rc = mp_debug_file_write(name,
                        (req.flags & MP_DBG_FILE_FLAG_FIRST) != 0,
                        (req.flags & MP_DBG_FILE_FLAG_LAST) != 0,
                        data, req.data_len);
                }
            }
            if (reply_max < 4) {
                return 0;
            }
            memcpy(reply_buf, &rc, 4);
            return 4;
        }

        case MP_DBG_CMD_FILE_CRC: {
            mp_dbg_file_name_t req;
            int32_t rc = MP_DBG_FILE_ERR_BAD_REQUEST;
            uint32_t crc = 0, fsize = 0;
            if (payload != NULL && size >= sizeof(req)) {
                memcpy(&req, payload, sizeof(req));
                if (mp_debug_file_name(payload, size, sizeof(req), req.name_len, name)) {
                    rc = mp_debug_file_crc(name, &crc, &fsize);
                }
            }
            if (reply_max < 12) {
                return 0;
            }
            memcpy(reply_buf + 0, &rc, 4);
            memcpy(reply_buf + 4, &crc, 4);
            memcpy(reply_buf + 8, &fsize, 4);
            return 12;
        }

        case MP_DBG_CMD_FILE_LIST: {
            mp_dbg_file_name_t req;
            uint32_t start = 0;
            if (payload == NULL || size < sizeof(req)) {
                return 0;
            }
            memcpy(&req, payload, sizeof(req));
            // An empty name means the filesystem root.
            if (req.name_len == 0) {
                name[0] = '/';
                name[1] = 0;
            } else if (!mp_debug_file_name(payload, size, sizeof(req), req.name_len, name)) {
                return 0;
            }
            if (size >= sizeof(req) + req.name_len + 4u) {
                memcpy(&start, payload + sizeof(req) + req.name_len, 4);
            }
            return mp_debug_file_list(name, start, reply_buf, reply_max);
        }

        case MP_DBG_CMD_FILE_STAT: {
            mp_dbg_file_name_t req;
            int32_t rc = MP_DBG_FILE_ERR_BAD_REQUEST;
            uint32_t bsize = 0, total = 0, avail = 0;
            if (payload != NULL && size >= sizeof(req)) {
                memcpy(&req, payload, sizeof(req));
                // An empty path means the filesystem the program lives on,
                // which is the current directory -- not "/". Nothing is mounted
                // at the root, and statvfs("/") answers for the root VFS with a
                // tuple of zeros rather than failing, so asking there reports an
                // empty filesystem instead of an error.
                if (req.name_len == 0) {
                    name[0] = '.';
                    name[1] = '\0';
                } else if (!mp_debug_file_name(payload, size, sizeof(req), req.name_len, name)) {
                    name[0] = '\0';
                }
                if (name[0] != '\0') {
                    rc = mp_debug_file_stat(name, &bsize, &total, &avail);
                }
            }
            if (reply_max < 16) {
                return 0;
            }
            memcpy(reply_buf + 0, &rc, 4);
            memcpy(reply_buf + 4, &bsize, 4);
            memcpy(reply_buf + 8, &total, 4);
            memcpy(reply_buf + 12, &avail, 4);
            return 16;
        }

        case MP_DBG_CMD_FILE_MKDIR: {
            mp_dbg_file_name_t req;
            int32_t rc = MP_DBG_FILE_ERR_BAD_REQUEST;
            if (payload != NULL && size >= sizeof(req)) {
                memcpy(&req, payload, sizeof(req));
                if (mp_debug_file_name(payload, size, sizeof(req), req.name_len, name)) {
                    rc = mp_debug_file_mkdir(name);
                }
            }
            if (reply_max < 4) {
                return 0;
            }
            memcpy(reply_buf, &rc, 4);
            return 4;
        }

        case MP_DBG_CMD_FILE_DELETE: {
            mp_dbg_file_name_t req;
            int32_t rc = MP_DBG_FILE_ERR_BAD_REQUEST;
            if (payload != NULL && size >= sizeof(req)) {
                memcpy(&req, payload, sizeof(req));
                if (mp_debug_file_name(payload, size, sizeof(req), req.name_len, name)) {
                    rc = mp_debug_file_delete(name);
                }
            }
            if (reply_max < 4) {
                return 0;
            }
            memcpy(reply_buf, &rc, 4);
            return 4;
        }

        default:
            return -1;
    }
}
