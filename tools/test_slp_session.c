/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the Slippi netplay session core (src/pc/slippi/slp_session.c).
 *
 * Two sessions are wired back to back in one process, so the bookkeeping that
 * has to match Dolphin is tested without ENet and without timing flakiness.
 * Covered:
 *   - a pad queued by P1 arrives at P2 with the right frame, port and bytes,
 *     and produces exactly one ack;
 *   - the ack advances P1's last-acked frame and samples RTT from the matching
 *     timer (and only from a matching frame);
 *   - the remote pad window returns the oldest selected frames, newest-first,
 *     with the game-facing 12-byte stride and zero in bytes 8..11;
 *   - drop-old keeps at least one pad and trims the rest;
 *   - the ack floor (newest-128) keeps the local queue bounded when a peer
 *     stops acking;
 *   - the time-offset estimate sorts and averages the middle third, and takes
 *     the minimum across remotes;
 *   - a malformed pad packet (claims more frames than it carries) is refused
 *     and leaves the queue untouched.
 *
 * From the repo root:
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 \
 *      -I src -I src/sdk_include \
 *      tools/test_slp_session.c -o /tmp/test_slp_session && /tmp/test_slp_session
 * (The three .c files are #included above, so they are not linked separately.) */
#include "../src/pc/slippi/slp_packet.c"
#include "../src/pc/slippi/slp_msg.c"
#include "../src/pc/slippi/slp_session.c"

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

static void make_session(PcSlippiSession* s, uint8_t local_port) {
    PcSlippiSessionConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_port = local_port;
    cfg.remote_count = 1;
    cfg.remote_ports[0] = local_port == 0 ? 1 : 0;
    pc_slippi_session_init(s, &cfg);
}

static PcSlippiPad mkpad(int32_t frame, uint8_t fill) {
    PcSlippiPad p;
    p.frame = frame;
    for (int i = 0; i < PC_SLIPPI_PAD_DATA_SIZE; i++) {
        p.data[i] = (uint8_t)(fill + i);
    }
    return p;
}

/* Deliver a pad packet's body (the id already consumed) to `dst`, returning
 * whether an ack was produced. */
static bool deliver_pad(PcSlippiSession* dst, const uint8_t* pkt, size_t len, uint8_t* ack,
                        size_t ack_cap, size_t* ack_len) {
    PcSlippiReader r;
    pc_slippi_reader_init(&r, pkt + 1, len - 1); /* skip the id byte */
    int slot = -1;
    bool want = pc_slippi_session_on_pads(dst, &r, &slot, ack, ack_cap, ack_len);
    return want;
}

static void test_pad_exchange_and_ack(void) {
    PcSlippiSession p1, p2;
    make_session(&p1, 0); /* P1 */
    make_session(&p2, 1); /* P2 */

    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    size_t len = 0;
    PcSlippiPad pad = mkpad(1, 0x10);
    assert(pc_slippi_session_queue_local_pad(&p1, 1, pad.data, 1000, pkt, sizeof pkt, &len));
    assert(len == PC_SLIPPI_PAD_DATA_OFFSET + PC_SLIPPI_PAD_DATA_SIZE);

    uint8_t ack[16];
    size_t ack_len = 0;
    assert(deliver_pad(&p2, pkt, len, ack, sizeof ack, &ack_len));
    assert(ack_len == 6);
    assert(ack[0] == PC_SLIPPI_MSG_PAD_ACK);

    /* P2 now holds frame 1 from port 0 (remote slot 0). */
    PcSlippiRemotePadOutput out;
    pc_slippi_session_remote_pad(&p2, 0, PC_SLIPPI_REMOTE_PAD_WINDOW, &out);
    assert(out.count == 1);
    assert(out.latest_frame == 1);
    assert(out.player_idx == 0); /* P1 is remote port 0 to P2 */
    assert(out.data[0] == 0x10 && out.data[7] == 0x17);
    assert(out.data[8] == 0 && out.data[11] == 0); /* 12-byte game-facing pad */

    /* Deliver the ack back to P1 and check RTT sampling. */
    PcSlippiReader ar;
    pc_slippi_reader_init(&ar, ack + 1, ack_len - 1);
    int32_t aframe;
    uint8_t aport;
    assert(pc_slippi_read_pad_ack(&ar, &aframe, &aport));
    assert(aframe == 1 && aport == 1);
    uint32_t rtt = pc_slippi_session_on_pad_ack(&p1, 0, aframe, 5000);
    assert(rtt == 4000); /* 5000 - 1000 */
    assert(pc_slippi_session_ping_us(&p1, 0) == 4000);
}

static void test_ack_only_matches_exact_frame(void) {
    PcSlippiSession p1;
    make_session(&p1, 0);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    size_t len;
    for (int32_t f = 10; f <= 12; f++) {
        PcSlippiPad pad = mkpad(f, (uint8_t)f);
        assert(pc_slippi_session_queue_local_pad(&p1, f, pad.data, (uint64_t)(f * 100), pkt,
                                                 sizeof pkt, &len));
    }
    /* An ack for a frame with no timer (e.g. already dropped) yields no RTT. */
    assert(pc_slippi_session_on_pad_ack(&p1, 0, 5, 9999) == 0);
    /* An ack for frame 12 matches its timer. */
    assert(pc_slippi_session_on_pad_ack(&p1, 0, 12, 5000) == 5000 - 1200);
    /* Re-acking frame 12 (timer consumed) yields no new sample. */
    assert(pc_slippi_session_on_pad_ack(&p1, 0, 12, 6000) == 0);
}

static void test_remote_window_oldest_selected(void) {
    PcSlippiSession p1, p2;
    make_session(&p1, 0);
    make_session(&p2, 1);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    size_t len;
    /* Queue 10 pads (frames 1..10) in one packet. */
    PcSlippiPad pads[10];
    for (int i = 0; i < 10; i++) {
        pads[i] = mkpad(10 - i, (uint8_t)(100 + i)); /* newest-first: 10,9,...1 */
    }
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, pkt, sizeof pkt);
    assert(pc_slippi_write_pads(&w, 10, 0, 0, 0, pads, 10));
    len = w.len;

    uint8_t ack[16];
    size_t ack_len;
    assert(deliver_pad(&p2, pkt, len, ack, sizeof ack, &ack_len));

    /* Window of 7 should take the OLDEST 7 (frames 1..7), newest-first: 7..1. */
    PcSlippiRemotePadOutput out;
    pc_slippi_session_remote_pad(&p2, 0, 7, &out);
    assert(out.count == 7);
    assert(out.latest_frame == 7); /* newest among the selected */
    /* data[0] is the newest selected (frame 7 -> fill 103), data[6] the oldest (frame 1 -> 109). */
    assert(out.data[0] == 103 && out.data[7] == 103 + 7);
    assert(out.data[6 * PC_SLIPPI_PAD_FULL_SIZE] == 109);
    assert(out.data[6 * PC_SLIPPI_PAD_FULL_SIZE + 7] == (uint8_t)(109 + 7));
}

static void test_drop_old(void) {
    PcSlippiSession p1, p2;
    make_session(&p1, 0);
    make_session(&p2, 1);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    PcSlippiPad pads[5];
    for (int i = 0; i < 5; i++) {
        pads[i] = mkpad(5 - i, (uint8_t)(10 + i)); /* newest-first: 5,4,3,2,1 */
    }
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, pkt, sizeof pkt);
    assert(pc_slippi_write_pads(&w, 5, 0, 0, 0, pads, 5));
    uint8_t ack[16];
    size_t ack_len;
    assert(deliver_pad(&p2, pkt, w.len, ack, sizeof ack, &ack_len));

    /* Finalized at 4: drop frames < 4, keep at least one. */
    pc_slippi_session_drop_old(&p2, 4);
    PcSlippiRemotePadOutput out;
    pc_slippi_session_remote_pad(&p2, 0, 7, &out);
    assert(out.count == 2); /* frames 4,5 remain */
    assert(out.latest_frame == 5);

    /* Finalized past everything: one pad stays (Slippi keeps at least one). */
    pc_slippi_session_drop_old(&p2, 999);
    pc_slippi_session_remote_pad(&p2, 0, 7, &out);
    assert(out.count == 1);
    assert(out.latest_frame == 5);
}

static void test_malformed_pad_refused(void) {
    PcSlippiSession p1, p2;
    make_session(&p1, 0);
    make_session(&p2, 1);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    uint8_t ack[16];
    size_t ack_len = 123;
    PcSlippiReader r;
    PcSlippiRemotePadOutput out;

    /* Establish frame 1 so P2 holds something. */
    PcSlippiPad one = mkpad(1, 1);
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, pkt, sizeof pkt);
    assert(pc_slippi_write_pads(&w, 1, 0, 0, 0, &one, 1));
    pc_slippi_reader_init(&r, pkt + 1, w.len - 1);
    assert(pc_slippi_session_on_pads(&p2, &r, NULL, ack, sizeof ack, &ack_len));

    /* Now a packet claiming frame 50 but carrying one pad: P2 holds 1, so it
     * wants 49 and the payload has 1. Refused, queue untouched. */
    PcSlippiPad liar = mkpad(50, 0xEE);
    pc_slippi_writer_init(&w, pkt, sizeof pkt);
    assert(pc_slippi_write_pads(&w, 50, 0, 0, 0, &liar, 1));
    pc_slippi_reader_init(&r, pkt + 1, w.len - 1);
    assert(!pc_slippi_session_on_pads(&p2, &r, NULL, ack, sizeof ack, &ack_len));
    pc_slippi_session_remote_pad(&p2, 0, 7, &out);
    assert(out.count == 1 && out.latest_frame == 1); /* still just frame 1 */

    /* A packet claiming far more than the 128 cap, even with an empty queue. */
    PcSlippiSession fresh;
    make_session(&fresh, 1);
    pc_slippi_writer_init(&w, pkt, sizeof pkt);
    assert(pc_slippi_write_pads(&w, 5000, 0, 0, 0, &one, 1));
    pc_slippi_reader_init(&r, pkt + 1, w.len - 1);
    assert(!pc_slippi_session_on_pads(&fresh, &r, NULL, ack, sizeof ack, &ack_len));
}

static void test_ack_floor_bounds_queue(void) {
    PcSlippiSession p1;
    make_session(&p1, 0);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    size_t len;
    /* Queue 200 frames with no acks. The floor (newest-128) should cap the
     * sent payload at 128 pads. */
    for (int32_t f = 1; f <= 200; f++) {
        PcSlippiPad pad = mkpad(f, (uint8_t)f);
        assert(pc_slippi_session_queue_local_pad(&p1, f, pad.data, (uint64_t)f, pkt, sizeof pkt,
                                                 &len));
    }
    /* Last packet carried at most 128 pads. */
    assert(len == PC_SLIPPI_PAD_DATA_OFFSET + 128 * PC_SLIPPI_PAD_DATA_SIZE);
}

static void test_time_offset_middle_third(void) {
    PcSlippiSession p1;
    make_session(&p1, 0);
    /* Feed 9 offset samples directly via note_offset. With our last send fixed
     * and the same remote frame, each sample is (now - ping/2) - last_time. Use
     * ping 0, last_time set by queue_local_pad. */
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    size_t len;
    PcSlippiPad pad = mkpad(0, 0);
    /* Establish last_sent at frame 0, time 0. */
    assert(pc_slippi_session_queue_local_pad(&p1, 0, pad.data, 0, pkt, sizeof pkt, &len));
    /* Samples: now = 1000,2000,...,9000 us with remote frame 0. */
    for (int i = 1; i <= 9; i++) {
        pc_slippi_session_note_offset(&p1, 0, 0, (uint64_t)i * 1000);
    }
    /* Sorted 1000..9000; middle third drops 3 each end -> 4000..6000, mean 5000. */
    int32_t off = pc_slippi_session_time_offset_us(&p1);
    assert(off == 5000);
}

static void test_no_samples_is_zero(void) {
    PcSlippiSession p1;
    make_session(&p1, 0);
    assert(pc_slippi_session_time_offset_us(&p1) == 0);
}

static void test_pad_for_frame_lookup(void) {
    PcSlippiSession p1, p2;
    make_session(&p1, 0);
    make_session(&p2, 1);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    PcSlippiPad pads[4];
    for (int i = 0; i < 4; i++) {
        pads[i] = mkpad(4 - i, (uint8_t)(0x40 + i)); /* newest-first: 4,3,2,1 */
    }
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, pkt, sizeof pkt);
    assert(pc_slippi_write_pads(&w, 4, 0, 0, 0, pads, 4));
    uint8_t ack[16];
    size_t ack_len;
    assert(deliver_pad(&p2, pkt, w.len, ack, sizeof ack, &ack_len));

    assert(pc_slippi_session_remote_latest(&p2, 0) == 4);

    uint8_t out[PC_SLIPPI_PAD_DATA_SIZE];
    assert(pc_slippi_session_pad_for_frame(&p2, 0, 4, out));
    assert(out[0] == 0x40); /* frame 4 -> fill 0x40 */
    assert(pc_slippi_session_pad_for_frame(&p2, 0, 1, out));
    assert(out[0] == 0x43); /* frame 1 -> fill 0x43 */
    /* A frame outside the held range is refused. */
    assert(!pc_slippi_session_pad_for_frame(&p2, 0, 5, out));
    assert(!pc_slippi_session_pad_for_frame(&p2, 0, 0, out));
}

static void test_start_game_clears(void) {
    PcSlippiSession p1, p2;
    make_session(&p1, 0);
    make_session(&p2, 1);
    uint8_t pkt[PC_SLIPPI_PACKET_MAX];
    size_t len;
    PcSlippiPad pad = mkpad(1, 1);
    assert(pc_slippi_session_queue_local_pad(&p1, 1, pad.data, 1, pkt, sizeof pkt, &len));
    uint8_t ack[16];
    size_t ack_len;
    assert(deliver_pad(&p2, pkt, len, ack, sizeof ack, &ack_len));
    PcSlippiRemotePadOutput out;
    pc_slippi_session_remote_pad(&p2, 0, 7, &out);
    assert(out.count == 1);

    pc_slippi_session_start_game(&p2, 0);
    pc_slippi_session_remote_pad(&p2, 0, 7, &out);
    assert(out.count == 1 && out.latest_frame == 0); /* reset to empty */
}

int main(void) {
    test_pad_exchange_and_ack();
    test_ack_only_matches_exact_frame();
    test_remote_window_oldest_selected();
    test_drop_old();
    test_malformed_pad_refused();
    test_ack_floor_bounds_queue();
    test_time_offset_middle_third();
    test_no_samples_is_zero();
    test_pad_for_frame_lookup();
    test_start_game_clears();
    printf("test_slp_session: all checks passed\n");
    return 0;
}
