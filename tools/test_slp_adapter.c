/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the Slippi transport adapter (src/pc/slippi/slp_adapter.c).
 *
 * Two adapters run over a real loopback ENet session, so the delivery walk
 * (which frames reach the game, in what order) is tested end to end. The unit
 * checks that do not need a socket drive the session directly, so a gap and a
 * stale frame can be exercised deterministically.
 *
 * Build (needs the vendored ENet):
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 \
 *      -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 \
 *      -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1 \
 *      -I src -I src/sdk_include -I extern/enet/include \
 *      tools/test_slp_adapter.c extern/enet/{callbacks,compress,host,list,packet,peer,protocol,unix}.c \
 *      -o /tmp/test_slp_adapter -lpthread
 */
#include "../src/pc/slippi/slp_packet.c"
#include "../src/pc/slippi/slp_msg.c"
#include "../src/pc/slippi/slp_session.c"
#include "../src/pc/slippi/slp_checksum.c"
#include "../src/pc/slippi/slp_net.c"
#include "../src/pc/slippi/slp_adapter.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ---- unit: the delivery walk stops at a gap ------------------------- */

typedef struct DeliverLog {
    int32_t frames[64];
    uint8_t pads[64];
    int n;
} DeliverLog;

static void log_frame(void* user, int remote_slot, int32_t frame,
                      const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE]) {
    (void)remote_slot;
    DeliverLog* l = (DeliverLog*)user;
    if (l->n < 64) {
        l->frames[l->n] = frame;
        l->pads[l->n] = pad[0];
        l->n++;
    }
}

/* A peer whose session we drive by hand (no socket) so a hole can be
 * exercised: frames 1 and 2 arrive, then 4 tries to, and the session must
 * refuse it because 3 is missing. This is the rule the delivery walk relies on
 * (deliver_remote stops at the gap). */
static void unit_gap_refused(void) {
    PcSlippiSessionConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_port = 1;
    cfg.remote_count = 1;
    cfg.remote_ports[0] = 0;
    PcSlippiSession session;
    pc_slippi_session_init(&session, &cfg);

    uint8_t buf[PC_SLIPPI_PACKET_MAX];
    PcSlippiWriter w;
    PcSlippiReader r;
    size_t ack_len = 0;

    /* frames 1 and 2 in one packet, newest-first. */
    PcSlippiPad pads[2];
    memset(pads, 0, sizeof pads);
    pads[0].frame = 2;
    pads[1].frame = 1;
    pads[0].data[0] = 0x22;
    pads[1].data[0] = 0x11;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(pc_slippi_write_pads(&w, 2, 0, 0, 0, pads, 2));
    pc_slippi_reader_init(&r, buf + 1, w.len - 1);
    assert(pc_slippi_session_on_pads(&session, &r, NULL, buf, sizeof buf, &ack_len));

    /* frame 4 alone: a gap (3 missing), so the session refuses it and holds at
     * 2. This is the "hole" the adapter's walk must not step over. */
    PcSlippiPad p4;
    memset(&p4, 0, sizeof p4);
    p4.frame = 4;
    p4.data[0] = 0x44;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    assert(pc_slippi_write_pads(&w, 4, 0, 0, 0, &p4, 1));
    pc_slippi_reader_init(&r, buf + 1, w.len - 1);
    assert(!pc_slippi_session_on_pads(&session, &r, NULL, buf, sizeof buf, &ack_len));

    uint8_t out[PC_SLIPPI_PAD_DATA_SIZE];
    assert(pc_slippi_session_pad_for_frame(&session, 0, 1, out) && out[0] == 0x11);
    assert(pc_slippi_session_pad_for_frame(&session, 0, 2, out) && out[0] == 0x22);
    assert(!pc_slippi_session_pad_for_frame(&session, 0, 3, out)); /* the hole */
    assert(!pc_slippi_session_pad_for_frame(&session, 0, 4, out)); /* never stored */
}

/* ---- integration: two adapters over loopback ------------------------ */

typedef struct Peer {
    PcSlippiNet* net;
    PcSlippiAdapter adapter;
    DeliverLog log;
} Peer;

static PcSlippiNet* make_net(uint8_t local_port, uint16_t udp_port) {
    PcSlippiSessionConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_port = local_port;
    cfg.remote_count = 1;
    cfg.remote_ports[0] = local_port == 0 ? 1 : 0;
    return pc_slippi_net_create(&cfg, udp_port, NULL);
}

static bool pump_until(PcSlippiAdapter* a1, PcSlippiAdapter* a2, bool (*pred)(void*), void* ctx,
                       int ms) {
    uint64_t deadline = now_us() + (uint64_t)ms * 1000ull;
    while (now_us() < deadline) {
        uint64_t t = now_us();
        pc_slippi_adapter_pump(a1, t);
        pc_slippi_adapter_pump(a2, t);
        if (pred(ctx)) {
            return true;
        }
        struct timespec s = {0, 1000000};
        nanosleep(&s, NULL);
    }
    return pred(ctx);
}

static bool both_connected(void* ctx) {
    Peer** p = ctx;
    return pc_slippi_adapter_connected(&p[0]->adapter) &&
           pc_slippi_adapter_connected(&p[1]->adapter);
}
static bool got_three(void* ctx) {
    Peer* p2 = (Peer*)ctx;
    return p2->log.n >= 3;
}

int main(void) {
    assert(pc_slippi_net_global_init());
    unit_gap_refused();

    const uint16_t b_port = 14713;
    Peer p1, p2;
    memset(&p1, 0, sizeof p1);
    memset(&p2, 0, sizeof p2);
    p1.net = make_net(0, 0);
    p2.net = make_net(1, b_port);
    assert(p1.net != NULL && p2.net != NULL);
    pc_slippi_adapter_init(&p1.adapter, p1.net, log_frame, NULL, &p1.log);
    pc_slippi_adapter_init(&p2.adapter, p2.net, log_frame, NULL, &p2.log);
    pc_slippi_net_listen(p2.net);
    assert(pc_slippi_net_connect(p1.net, "127.0.0.1", b_port));

    Peer* conn[2] = {&p1, &p2};
    assert(pump_until(&p1.adapter, &p2.adapter, both_connected, conn, 5000));
    printf("test_slp_adapter: connected\n");

    /* P1 sends frames 1..3; P2's adapter should deliver them in order. */
    pc_slippi_adapter_start_game(&p1.adapter, now_us());
    pc_slippi_adapter_start_game(&p2.adapter, now_us());
    for (int32_t f = 1; f <= 3; f++) {
        uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE];
        memset(pad, (int)(0x10 + f), sizeof pad);
        assert(pc_slippi_adapter_send_local(&p1.adapter, f, pad, now_us()));
    }
    assert(pump_until(&p1.adapter, &p2.adapter, got_three, &p2, 3000));
    assert(p2.log.n >= 3);
    assert(p2.log.frames[0] == 1 && p2.log.pads[0] == 0x11);
    assert(p2.log.frames[1] == 2 && p2.log.pads[1] == 0x12);
    assert(p2.log.frames[2] == 3 && p2.log.pads[2] == 0x13);
    assert(pc_slippi_adapter_remote_have(&p2.adapter) == 3);
    printf("test_slp_adapter: delivered 3 frames in order\n");

    pc_slippi_net_destroy(p1.net);
    pc_slippi_net_destroy(p2.net);
    pc_slippi_net_global_shutdown();
    printf("test_slp_adapter: all checks passed\n");
    return 0;
}
