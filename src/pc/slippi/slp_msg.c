/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay message bodies (slp_msg.h). See that header for the wire
 * layouts and the upstream sources they were ported from. */
#include "pc/slippi/slp_msg.h"

#include <string.h>

/* Slippi accepts exactly these 16 chat ids plus CHAT_DISABLED. */
static const int kChatIds[] = {136, 129, 130, 132, 34, 40, 33, 36,
                               72,  66,  68,  65,  24, 18, 20, 17};

bool pc_slippi_chat_id_allowed(int id) {
    if (id == PC_SLIPPI_CHAT_DISABLED) {
        return true;
    }
    for (size_t i = 0; i < sizeof kChatIds / sizeof kChatIds[0]; i++) {
        if (kChatIds[i] == id) {
            return true;
        }
    }
    return false;
}

uint8_t pc_slippi_player_idx_from_port(uint8_t port, uint8_t local_port) {
    return port > local_port ? (uint8_t)(port - 1) : port;
}

/* ---- selections ------------------------------------------------------- */

void pc_slippi_selections_reset(PcSlippiSelections* s) {
    s->player_idx = 0;
    s->character_id = 0;
    s->character_color = 0;
    s->team_id = 0;
    s->character_selected = false;
    s->stage_id = 0;
    s->stage_selected = false;
    s->alt_stage_mode = 0;
    s->rng_offset = 0;
    s->message_id = 0;
    s->error = false;
}

void pc_slippi_selections_merge(PcSlippiSelections* self, const PcSlippiSelections* from) {
    /* rngOffset always carries over; stage and character only when the sender
     * actually selected them. Copied verbatim from Slippi's Merge. */
    self->rng_offset = from->rng_offset;
    if (from->stage_selected) {
        self->stage_id = from->stage_id;
        self->stage_selected = true;
        self->alt_stage_mode = from->alt_stage_mode;
    }
    if (from->character_selected) {
        self->character_id = from->character_id;
        self->character_color = from->character_color;
        self->team_id = from->team_id;
        self->character_selected = true;
    }
}

void pc_slippi_write_selections(PcSlippiWriter* w, const PcSlippiSelections* s) {
    /* Order is writeToPacket: char, color, charSelected, playerIdx, stage,
     * stageSelected, rngOffset, teamId, altStageMode. */
    pc_slippi_put_u8(w, PC_SLIPPI_MSG_MATCH_SELECTIONS);
    pc_slippi_put_u8(w, s->character_id);
    pc_slippi_put_u8(w, s->character_color);
    pc_slippi_put_bool(w, s->character_selected);
    pc_slippi_put_u8(w, s->player_idx);
    pc_slippi_put_u16(w, s->stage_id);
    pc_slippi_put_bool(w, s->stage_selected);
    pc_slippi_put_u32(w, s->rng_offset);
    pc_slippi_put_u8(w, s->team_id);
    pc_slippi_put_u8(w, s->alt_stage_mode);
}

bool pc_slippi_read_selections(PcSlippiReader* r, PcSlippiSelections* s) {
    pc_slippi_selections_reset(s);
    s->character_id = pc_slippi_get_u8(r);
    s->character_color = pc_slippi_get_u8(r);
    s->character_selected = pc_slippi_get_bool(r);
    s->player_idx = pc_slippi_get_u8(r);
    s->stage_id = pc_slippi_get_u16(r);
    s->stage_selected = pc_slippi_get_bool(r);
    s->rng_offset = pc_slippi_get_u32(r);
    s->team_id = pc_slippi_get_u8(r);
    s->alt_stage_mode = pc_slippi_get_u8(r);
    if (!r->ok) {
        s->error = true;
        return false;
    }
    return true;
}

/* ---- chat ------------------------------------------------------------- */

bool pc_slippi_write_chat(PcSlippiWriter* w, int message_id, uint8_t player_idx) {
    if (!pc_slippi_chat_id_allowed(message_id)) {
        return false;
    }
    pc_slippi_put_u8(w, PC_SLIPPI_MSG_CHAT);
    pc_slippi_put_u32(w, (uint32_t)message_id);
    pc_slippi_put_u8(w, player_idx);
    return w->ok;
}

bool pc_slippi_read_chat(PcSlippiReader* r, PcSlippiSelections* s) {
    pc_slippi_selections_reset(s);
    s->message_id = (int)pc_slippi_get_u32(r);
    s->player_idx = pc_slippi_get_u8(r);
    if (!r->ok) {
        s->error = true;
        return false;
    }
    if (!pc_slippi_chat_id_allowed(s->message_id)) {
        s->error = true;
        return false;
    }
    return true;
}

/* ---- pads ------------------------------------------------------------- */

bool pc_slippi_write_pads(PcSlippiWriter* w, int32_t frame, uint8_t player_port,
                          int32_t checksum_frame, uint32_t checksum, const PcSlippiPad* pads,
                          int count) {
    if (count < 1 || count > PC_SLIPPI_MAX_INPUT_FRAMES) {
        return false;
    }
    pc_slippi_put_u8(w, PC_SLIPPI_MSG_PAD);
    pc_slippi_put_i32(w, frame);
    pc_slippi_put_u8(w, player_port);
    pc_slippi_put_i32(w, checksum_frame);
    pc_slippi_put_u32(w, checksum);
    for (int i = 0; i < count; i++) {
        pc_slippi_put_bytes(w, pads[i].data, PC_SLIPPI_PAD_DATA_SIZE);
    }
    return w->ok;
}

bool pc_slippi_read_pad_header(PcSlippiReader* r, int32_t head_frame, PcSlippiPadHeader* out,
                               int64_t* inputs_to_copy) {
    out->frame = pc_slippi_get_i32(r);
    out->player_port = pc_slippi_get_u8(r);
    out->checksum_frame = pc_slippi_get_i32(r);
    out->checksum = pc_slippi_get_u32(r);
    out->pad_count = 0;
    *inputs_to_copy = 0;
    if (!r->ok) {
        return false;
    }
    /* Slippi computes inputsToCopy = frame - headFrame (64-bit to avoid
     * overflow), rejects more than 128, and requires the payload to hold the
     * claimed pads. Reproduce that: a claimed frame ahead of the payload is a
     * malformed packet, not a partial read. */
    int64_t copy = (int64_t)out->frame - (int64_t)head_frame;
    if (copy > PC_SLIPPI_MAX_INPUT_FRAMES) {
        r->ok = false;
        return false;
    }
    if (copy < 0) {
        /* An older frame already held: Slippi computes a negative count, the
         * for-loop copies nothing, and the packet is simply not useful. Treat
         * it as zero new frames rather than an error. */
        copy = 0;
    }
    size_t remaining = pc_slippi_reader_remaining(r);
    int fits = (int)(remaining / PC_SLIPPI_PAD_DATA_SIZE);
    if ((int64_t)fits < copy) {
        r->ok = false;
        return false;
    }
    out->pad_count = (int)copy;
    *inputs_to_copy = copy;
    return true;
}

void pc_slippi_read_pad_at(const PcSlippiReader* r, const PcSlippiPadHeader* h, int index,
                           PcSlippiPad* out) {
    memset(out, 0, sizeof *out);
    if (index < 0 || index >= h->pad_count) {
        return;
    }
    /* Pads are ordered newest-first starting at the current position. */
    size_t off = (size_t)index * PC_SLIPPI_PAD_DATA_SIZE;
    if (off + PC_SLIPPI_PAD_DATA_SIZE > pc_slippi_reader_remaining(r)) {
        return;
    }
    out->frame = h->frame - index;
    memcpy(out->data, r->data + r->pos + off, PC_SLIPPI_PAD_DATA_SIZE);
}

/* ---- pad ack ---------------------------------------------------------- */

bool pc_slippi_write_pad_ack(PcSlippiWriter* w, int32_t frame, uint8_t player_idx) {
    pc_slippi_put_u8(w, PC_SLIPPI_MSG_PAD_ACK);
    pc_slippi_put_i32(w, frame);
    pc_slippi_put_u8(w, player_idx);
    return w->ok;
}

bool pc_slippi_read_pad_ack(PcSlippiReader* r, int32_t* frame, uint8_t* player_idx) {
    *frame = pc_slippi_get_i32(r);
    *player_idx = pc_slippi_get_u8(r);
    return r->ok;
}
