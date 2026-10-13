/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay packet codec (slp_packet.h). See that header for the wire
 * rules and the source of the behaviour. */
#include "pc/slippi/slp_packet.h"

#include <string.h>

/* Slippi strings that reach a packet are connect codes, display names and
 * build fingerprints. A length larger than this is malformed input, not a
 * very long name; latch the error rather than advertise a huge read. */
#define PC_SLIPPI_STRING_MAX 0x400

/* ---- writer ----------------------------------------------------------- */

void pc_slippi_writer_init(PcSlippiWriter* w, void* data, size_t cap) {
    w->data = (uint8_t*)data;
    w->cap = cap;
    w->len = 0;
    w->ok = true;
}

static void put_raw(PcSlippiWriter* w, const void* p, size_t n) {
    if (!w->ok || n > w->cap - w->len) {
        w->ok = false;
        return;
    }
    memcpy(w->data + w->len, p, n);
    w->len += n;
}

void pc_slippi_put_u8(PcSlippiWriter* w, uint8_t v) { put_raw(w, &v, 1); }

void pc_slippi_put_bool(PcSlippiWriter* w, bool v) { pc_slippi_put_u8(w, v ? 1 : 0); }

void pc_slippi_put_u16(PcSlippiWriter* w, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put_raw(w, b, sizeof b);
}

void pc_slippi_put_u32(PcSlippiWriter* w, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put_raw(w, b, sizeof b);
}

void pc_slippi_put_i32(PcSlippiWriter* w, int32_t v) { pc_slippi_put_u32(w, (uint32_t)v); }

void pc_slippi_put_bytes(PcSlippiWriter* w, const void* data, size_t n) { put_raw(w, data, n); }

void pc_slippi_put_string(PcSlippiWriter* w, const char* s) {
    size_t n = s != NULL ? strlen(s) : 0;
    if (n > PC_SLIPPI_STRING_MAX) {
        w->ok = false;
        return;
    }
    pc_slippi_put_u32(w, (uint32_t)n);
    if (n != 0) {
        put_raw(w, s, n);
    }
}

/* ---- reader ----------------------------------------------------------- */

void pc_slippi_reader_init(PcSlippiReader* r, const void* data, size_t len) {
    r->data = (const uint8_t*)data;
    r->len = len;
    r->pos = 0;
    r->ok = true;
}

size_t pc_slippi_reader_remaining(const PcSlippiReader* r) {
    return r->pos <= r->len ? r->len - r->pos : 0;
}

/* Every read goes through here. Once `ok` is false it yields zero without
 * advancing, which is what an sf::Packet does after it hits the end. */
static bool take(PcSlippiReader* r, size_t n, const uint8_t** out) {
    if (!r->ok || n > pc_slippi_reader_remaining(r)) {
        r->ok = false;
        return false;
    }
    *out = r->data + r->pos;
    r->pos += n;
    return true;
}

uint8_t pc_slippi_get_u8(PcSlippiReader* r) {
    const uint8_t* p;
    return take(r, 1, &p) ? p[0] : 0;
}

bool pc_slippi_get_bool(PcSlippiReader* r) { return pc_slippi_get_u8(r) != 0; }

uint16_t pc_slippi_get_u16(PcSlippiReader* r) {
    const uint8_t* p;
    return take(r, 2, &p) ? (uint16_t)(p[0] << 8 | p[1]) : 0;
}

uint32_t pc_slippi_get_u32(PcSlippiReader* r) {
    const uint8_t* p;
    if (!take(r, 4, &p)) {
        return 0;
    }
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int32_t pc_slippi_get_i32(PcSlippiReader* r) { return (int32_t)pc_slippi_get_u32(r); }

size_t pc_slippi_get_bytes(PcSlippiReader* r, void* out, size_t n) {
    const uint8_t* p;
    if (!take(r, n, &p)) {
        return 0;
    }
    if (out != NULL) {
        memcpy(out, p, n);
    }
    return n;
}

void pc_slippi_get_string(PcSlippiReader* r, char* out, size_t cap) {
    uint32_t n = pc_slippi_get_u32(r);
    if (!r->ok) {
        if (out != NULL && cap != 0) {
            out[0] = '\0';
        }
        return;
    }
    if (n > PC_SLIPPI_STRING_MAX) {
        r->ok = false;
        if (out != NULL && cap != 0) {
            out[0] = '\0';
        }
        return;
    }
    size_t keep = n;
    if (out != NULL) {
        if (keep > cap - 1) {
            keep = cap != 0 ? cap - 1 : 0;
        }
        pc_slippi_get_bytes(r, out, keep);
        out[keep] = '\0';
        /* Discard whatever did not fit so `pos` still ends at the string end. */
        if (n > keep) {
            pc_slippi_get_bytes(r, NULL, n - keep);
        }
    } else {
        pc_slippi_get_bytes(r, NULL, n);
    }
}
