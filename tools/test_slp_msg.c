/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the Slippi message bodies (src/pc/slippi/slp_msg.c).
 *
 * These are the messages whose layouts must match Dolphin exactly: a wrong
 * field order or width makes a peer read a character select as a stage select,
 * or drop a pad. So this pins the actual bytes for each message against the
 * layout in upstream SlippiNetplay.cpp (writeToPacket, the inline pad and
 * pad-ack writes, WriteChatMessageToPacket), and checks the receiver-side
 * arithmetic that rejects malformed pads.
 *
 * From the repo root:
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 \
 *      -I src -I src/sdk_include \
 *      tools/test_slp_msg.c -o /tmp/test_slp_msg && /tmp/test_slp_msg
 * (The two .c files are #included above, so they are not linked separately.) */
#include "../src/pc/slippi/slp_packet.c"
#include "../src/pc/slippi/slp_msg.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void test_selections_roundtrip_and_bytes(void) {
    PcSlippiSelections s;
    pc_slippi_selections_reset(&s);
    s.character_id = 9;   /* Marth */
    s.character_color = 2;
    s.character_selected = true;
    s.player_idx = 1;
    s.stage_id = 0x1F;    /* Battlefield */
    s.stage_selected = true;
    s.rng_offset = 0x11223344;
    s.team_id = 3;
    s.alt_stage_mode = 1;

    uint8_t buf[64];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    pc_slippi_write_selections(&w, &s);
    assert(w.ok);

    /* id 0x82, char 0x09, color 0x02, charSelected 0x01, playerIdx 0x01,
     * stage 0x00 0x1F, stageSelected 0x01, rngOffset 11 22 33 44,
     * teamId 0x03, altStageMode 0x01. */
    const uint8_t expect[] = {0x82, 0x09, 0x02, 0x01, 0x01, 0x00, 0x1F, 0x01,
                              0x11, 0x22, 0x33, 0x44, 0x03, 0x01};
    assert(w.len == sizeof expect);
    assert(memcmp(buf, expect, sizeof expect) == 0);

    PcSlippiReader r;
    pc_slippi_reader_init(&r, buf + 1, w.len - 1); /* skip the id, as the receiver does */
    PcSlippiSelections got;
    assert(pc_slippi_read_selections(&r, &got));
    assert(got.character_id == 9 && got.character_color == 2 && got.character_selected);
    assert(got.player_idx == 1 && got.stage_id == 0x1F && got.stage_selected);
    assert(got.rng_offset == 0x11223344 && got.team_id == 3 && got.alt_stage_mode == 1);
    assert(r.pos == r.len);
}

static void test_selections_short_body(void) {
    /* One byte short: read_selections must fail and set error. */
    uint8_t buf[14];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    PcSlippiSelections s;
    pc_slippi_selections_reset(&s);
    pc_slippi_write_selections(&w, &s);
    PcSlippiReader r;
    pc_slippi_reader_init(&r, buf + 1, w.len - 2);
    PcSlippiSelections got;
    assert(!pc_slippi_read_selections(&r, &got));
    assert(got.error);
}

static void test_selections_merge(void) {
    PcSlippiSelections local, remote;
    /* A remote that selected only a character leaves the local stage alone. */
    pc_slippi_selections_reset(&local);
    local.stage_id = 0x1F;
    local.stage_selected = true;
    local.character_id = 1;
    pc_slippi_selections_reset(&remote);
    remote.character_id = 9;
    remote.character_color = 4;
    remote.character_selected = true;
    remote.team_id = 2;
    remote.rng_offset = 99;
    pc_slippi_selections_merge(&local, &remote);
    assert(local.character_id == 9 && local.character_color == 4 && local.character_selected);
    assert(local.team_id == 2);
    assert(local.stage_id == 0x1F && local.stage_selected); /* untouched */
    assert(local.rng_offset == 99);                          /* always copied */

    /* A remote stage selection overwrites. */
    pc_slippi_selections_reset(&remote);
    remote.stage_id = 0x08;
    remote.stage_selected = true;
    remote.alt_stage_mode = 1;
    pc_slippi_selections_merge(&local, &remote);
    assert(local.stage_id == 0x08 && local.stage_selected && local.alt_stage_mode == 1);
    assert(local.character_id == 9); /* untouched by a stage-only merge */
}

static void test_chat(void) {
    uint8_t buf[16];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(pc_slippi_write_chat(&w, 136, 1));
    assert(w.ok);
    /* id 0x84, messageId i32 00 00 00 88, playerIdx 0x01. */
    const uint8_t expect[] = {0x84, 0x00, 0x00, 0x00, 0x88, 0x01};
    assert(w.len == sizeof expect && memcmp(buf, expect, sizeof expect) == 0);

    PcSlippiReader r;
    pc_slippi_reader_init(&r, buf + 1, w.len - 1);
    PcSlippiSelections got;
    assert(pc_slippi_read_chat(&r, &got));
    assert(got.message_id == 136 && got.player_idx == 1 && !got.error);

    /* CHAT_DISABLED is allowed; an arbitrary id is not, on either side. */
    assert(pc_slippi_chat_id_allowed(PC_SLIPPI_CHAT_DISABLED));
    assert(!pc_slippi_chat_id_allowed(12345));
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(!pc_slippi_write_chat(&w, 12345, 0));
    assert(w.len == 0); /* nothing written for a refused id */

    /* A forged id is rejected on read even if the body is well formed. */
    const uint8_t forged[] = {0x00, 0x00, 0x30, 0x39, 0x00}; /* 12345 */
    pc_slippi_reader_init(&r, forged, sizeof forged);
    assert(!pc_slippi_read_chat(&r, &got));
    assert(got.error);
}

static void test_player_idx(void) {
    /* Our port 0: remote ports 1,2,3 map to indices 0,1,2. */
    assert(pc_slippi_player_idx_from_port(1, 0) == 0);
    assert(pc_slippi_player_idx_from_port(2, 0) == 1);
    assert(pc_slippi_player_idx_from_port(3, 0) == 2);
    /* Our port 1: port 0 -> 0, ports 2,3 -> 1,2. */
    assert(pc_slippi_player_idx_from_port(0, 1) == 0);
    assert(pc_slippi_player_idx_from_port(2, 1) == 1);
    assert(pc_slippi_player_idx_from_port(3, 1) == 2);
}

static void test_pads_roundtrip(void) {
    uint8_t buf[PC_SLIPPI_PACKET_MAX];
    PcSlippiPad pads[3];
    memset(pads, 0, sizeof pads);
    for (int i = 0; i < 3; i++) {
        pads[i].frame = 1000 - i; /* newest first */
        for (int j = 0; j < PC_SLIPPI_PAD_DATA_SIZE; j++) {
            pads[i].data[j] = (uint8_t)(i * 16 + j);
        }
    }
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(pc_slippi_write_pads(&w, 1000, 1, 990, 0xCAFEBABE, pads, 3));
    assert(w.ok);
    assert(w.len == PC_SLIPPI_PAD_DATA_OFFSET + 3 * PC_SLIPPI_PAD_DATA_SIZE);

    /* id + header bytes, pinned. */
    const uint8_t head[] = {0x80, 0x00, 0x00, 0x03, 0xE8, 0x01,
                            0x00, 0x00, 0x03, 0xDE, 0xCA, 0xFE, 0xBA, 0xBE};
    assert(memcmp(buf, head, PC_SLIPPI_PAD_DATA_OFFSET) == 0);

    /* Stationary pad 0 byte range starts at offset 14. */
    assert(buf[14] == 0x00 && buf[21] == 0x07);
    assert(buf[22] == 0x10 && buf[29] == 0x17);

    PcSlippiReader r;
    pc_slippi_reader_init(&r, buf + 1, w.len - 1);
    PcSlippiPadHeader h;
    int64_t copy;
    /* Receiver held frame 997, so frames 998,999,1000 are new: 3. */
    assert(pc_slippi_read_pad_header(&r, 997, &h, &copy));
    assert(h.frame == 1000 && h.player_port == 1 && h.checksum_frame == 990);
    assert(h.checksum == 0xCAFEBABE);
    assert(copy == 3 && h.pad_count == 3);
    PcSlippiPad p;
    pc_slippi_read_pad_at(&r, &h, 0, &p);
    assert(p.frame == 1000 && p.data[7] == 0x07);
    pc_slippi_read_pad_at(&r, &h, 2, &p);
    assert(p.frame == 998 && p.data[0] == 0x20 && p.data[7] == 0x27);
    /* Out-of-range index is a no-op, not a read past the buffer. */
    pc_slippi_read_pad_at(&r, &h, 3, &p);
    assert(p.frame == 0 && p.data[0] == 0);
}

static void test_pad_header_rejects_liars(void) {
    uint8_t buf[PC_SLIPPI_PACKET_MAX];
    PcSlippiPad pads[2];
    memset(pads, 0, sizeof pads);
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(pc_slippi_write_pads(&w, 1000, 1, 0, 0, pads, 2));

    /* Claims frame 1000 but receiver holds 900: wants 100 pads, payload has 2.
     * Must be refused, not read past the buffer. */
    PcSlippiReader r;
    pc_slippi_reader_init(&r, buf + 1, w.len - 1);
    PcSlippiPadHeader h;
    int64_t copy;
    assert(!pc_slippi_read_pad_header(&r, 900, &h, &copy));
    assert(!r.ok);

    /* Claims more than MAX_INPUT_FRAMES ahead even with a big payload: refused
     * by the count limit. Build a header-only packet that lies. */
    uint8_t small[PC_SLIPPI_PAD_DATA_OFFSET];
    pc_slippi_writer_init(&w, small, sizeof small);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_PAD);
    pc_slippi_put_i32(&w, 5000);
    pc_slippi_put_u8(&w, 1);
    pc_slippi_put_i32(&w, 0);
    pc_slippi_put_u32(&w, 0);
    pc_slippi_reader_init(&r, small + 1, w.len - 1);
    assert(!pc_slippi_read_pad_header(&r, 900, &h, &copy)); /* 4100 > 128 */
}

static void test_pad_ack(void) {
    uint8_t buf[8];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(pc_slippi_write_pad_ack(&w, 1000, 1));
    const uint8_t expect[] = {0x81, 0x00, 0x00, 0x03, 0xE8, 0x01};
    assert(w.len == sizeof expect && memcmp(buf, expect, sizeof expect) == 0);
    PcSlippiReader r;
    pc_slippi_reader_init(&r, buf + 1, w.len - 1);
    int32_t frame;
    uint8_t port;
    assert(pc_slippi_read_pad_ack(&r, &frame, &port));
    assert(frame == 1000 && port == 1);
}

static void test_write_pads_rejects_bad_count(void) {
    uint8_t buf[64];
    PcSlippiWriter w;
    PcSlippiPad pads[1];
    memset(pads, 0, sizeof pads);
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(!pc_slippi_write_pads(&w, 1, 0, 0, 0, pads, 0));
    assert(w.len == 0);
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(!pc_slippi_write_pads(&w, 1, 0, 0, 0, pads, PC_SLIPPI_MAX_INPUT_FRAMES + 1));
    assert(w.len == 0);
}

int main(void) {
    test_selections_roundtrip_and_bytes();
    test_selections_short_body();
    test_selections_merge();
    test_chat();
    test_player_idx();
    test_pads_roundtrip();
    test_pad_header_rejects_liars();
    test_pad_ack();
    test_write_pads_rejects_bad_count();
    printf("test_slp_msg: all checks passed\n");
    return 0;
}
