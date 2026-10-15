/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay message bodies: encode and decode the payloads that follow a
 * message id on the wire.
 *
 * Ported for native C use in melee-pc from project-slippi/Ishiiruka
 * SlippiNetplay.cpp (writeToPacket/readSelectionsFromPacket,
 * WriteChatMessageToPacket/ReadChatMessageFromPacket, and the inline pad and
 * pad-ack handling in OnData), and from Hero88go/melee-unlocked
 * port/runtime/hle/slippi_net.cpp. See docs/slippi-migration-plan.md.
 *
 * This module is transport-free: it turns Slippi messages into C structs and
 * back. The ENet layer (slp_net.c, M2b) is what carries the bytes. Splitting
 * them lets the wire format be tested byte-for-byte without a socket.
 *
 * A "selections" message is the CSS/SSS pick sync: character, color, stage,
 * the RNG offset and the alt-stage toggle. `Merge` applies a remote pick over
 * a local one exactly as Slippi does (rngOffset always, stage and character
 * only when the sender had selected them). */
#ifndef PC_SLIPPI_SLP_MSG_H
#define PC_SLIPPI_SLP_MSG_H

#include <stdbool.h>
#include <stdint.h>

#include "pc/slippi/slp_packet.h"
#include "pc/slippi/slp_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- player selections (NP_MSG_SLIPPI_MATCH_SELECTIONS) ---------------- */
typedef struct PcSlippiSelections {
    uint8_t player_idx;
    uint8_t character_id;
    uint8_t character_color;
    uint8_t team_id;
    bool character_selected;
    uint16_t stage_id;
    bool stage_selected;
    uint8_t alt_stage_mode;
    uint32_t rng_offset;
    /* Chat-only: the message id and whether decoding failed. Not part of a
     * selections packet on the wire. */
    int message_id;
    bool error;
} PcSlippiSelections;

void pc_slippi_selections_reset(PcSlippiSelections* s);
/* Overlay `from` onto `self` the way Slippi's Merge does. */
void pc_slippi_selections_merge(PcSlippiSelections* self, const PcSlippiSelections* from);

/* Write a full selections message (id + body) into `w`. */
void pc_slippi_write_selections(PcSlippiWriter* w, const PcSlippiSelections* s);
/* Read a selections body (the id has already been consumed). Sets s->error and
 * returns false on a short or malformed body. */
bool pc_slippi_read_selections(PcSlippiReader* r, PcSlippiSelections* s);

/* ---- chat (NP_MSG_SLIPPI_CHAT_MESSAGE) --------------------------------- */
/* Write a chat message, or return false without writing if the id may not be
 * sent (Slippi accepts only its fixed set plus CHAT_DISABLED). */
bool pc_slippi_write_chat(PcSlippiWriter* w, int message_id, uint8_t player_idx);
/* Read a chat body into `s` (message_id set); false on a short body or an id
 * outside the allowed set. */
bool pc_slippi_read_chat(PcSlippiReader* r, PcSlippiSelections* s);
/* True when `id` is one Slippi permits (its 16 chat ids or CHAT_DISABLED). */
bool pc_slippi_chat_id_allowed(int id);

/* ---- pad (NP_MSG_SLIPPI_PAD) ------------------------------------------- */
/* One pad on the wire is the first 8 bytes of the 12-byte GCPadStatus. */
typedef struct PcSlippiPad {
    int32_t frame;
    uint8_t data[PC_SLIPPI_PAD_DATA_SIZE];
} PcSlippiPad;

/* Write a pad message: id, newest frame, player port, checksum frame,
 * checksum, then `count` pads newest-first. `count` must be 1..MAX_INPUT_FRAMES
 * and the caller must pass newest-first order. Returns false and writes
 * nothing if count is out of range. */
bool pc_slippi_write_pads(PcSlippiWriter* w, int32_t frame, uint8_t player_port,
                          int32_t checksum_frame, uint32_t checksum, const PcSlippiPad* pads,
                          int count);

/* The header fields of a pad message, plus how many pad bytes the payload can
 * hold. This is what the receiver validates before copying any input. */
typedef struct PcSlippiPadHeader {
    int32_t frame;
    uint8_t player_port;
    int32_t checksum_frame;
    uint32_t checksum;
    /* Pads the payload actually carries, given the body length. */
    int pad_count;
} PcSlippiPadHeader;

/* Read the pad header and compute how many 8-byte pads fit. `frame` is the
 * newest frame in the message. `head_frame` is the receiver's newest held
 * frame (0 when it holds none), and `inputs_to_copy` is frame - head_frame as
 * Slippi computes it. Returns false and latches r->ok false on a short header,
 * a claimed frame ahead of the payload, or more than MAX_INPUT_FRAMES. */
bool pc_slippi_read_pad_header(PcSlippiReader* r, int32_t head_frame,
                               PcSlippiPadHeader* out, int64_t* inputs_to_copy);

/* Copy one pad out of a pad message body at index i (0 = newest). The caller
 * has already validated the header; this does the bounds-safe copy. */
void pc_slippi_read_pad_at(const PcSlippiReader* r, const PcSlippiPadHeader* h, int index,
                           PcSlippiPad* out);

/* ---- pad ack (NP_MSG_SLIPPI_PAD_ACK) ----------------------------------- */
bool pc_slippi_write_pad_ack(PcSlippiWriter* w, int32_t frame, uint8_t player_idx);
bool pc_slippi_read_pad_ack(PcSlippiReader* r, int32_t* frame, uint8_t* player_idx);

/* ---- player index mapping ---------------------------------------------
 * Slippi's PlayerIdxFromPort: an on-wire player *port* (0..3) maps to a
 * remote-array index by dropping our own port. Port p maps to p when p is
 * below our index, else p-1. Both peers must agree or a pad lands in the
 * wrong slot. */
uint8_t pc_slippi_player_idx_from_port(uint8_t port, uint8_t local_port);

#ifdef __cplusplus
}
#endif

#endif
