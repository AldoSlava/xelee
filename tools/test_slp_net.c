/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the Slippi ENet driver (src/pc/slippi/slp_net.c).
 *
 * Two drivers run in one process over loopback, the way two instances would on
 * one machine. This is the M2 gate: pads, acks and selections cross the real
 * ENet socket layer and land in the session core with the right values.
 *
 * ENet's handshake is asynchronous, so the test pumps both sides in a loop
 * with a bounded wall-clock budget rather than assuming a fixed number of
 * iterations. It fails loudly if the connection never comes up.
 *
 * Build (needs the vendored ENet; the four slp_*.c files are #included, so only
 * ENet is linked). ENet's unix.c needs the POSIX feature defines, exactly as the
 * CMake target sets them:
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 \
 *      -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 \
 *      -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1 \
 *      -I src -I src/sdk_include -I extern/enet/include \
 *      tools/test_slp_net.c extern/enet/callbacks.c extern/enet/compress.c \
 *      extern/enet/host.c extern/enet/list.c extern/enet/packet.c \
 *      extern/enet/peer.c extern/enet/protocol.c extern/enet/unix.c \
 *      -o /tmp/test_slp_net -lpthread
 * On Windows the same command needs ws2_32/winmm instead of -lpthread.
 */
#include "../src/pc/slippi/slp_packet.c"
#include "../src/pc/slippi/slp_msg.c"
#include "../src/pc/slippi/slp_session.c"
#include "../src/pc/slippi/slp_net.c"

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

typedef struct Seen {
    int connected;
    int selections;
    PcSlippiSelections last;
} Seen;

static void on_connected(void* u) { ((Seen*)u)->connected++; }
static void on_selections(void* u, int slot, const PcSlippiSelections* s) {
    (void)slot;
    Seen* seen = (Seen*)u;
    seen->selections++;
    seen->last = *s;
}

static PcSlippiNet* make_peer(uint8_t local_port, uint16_t udp_port, Seen* seen) {
    PcSlippiSessionConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_port = local_port;
    cfg.remote_count = 1;
    cfg.remote_ports[0] = local_port == 0 ? 1 : 0;
    PcSlippiNetCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.on_connected = on_connected;
    cb.on_selections = on_selections;
    cb.user = seen;
    return pc_slippi_net_create(&cfg, udp_port, &cb);
}

/* Pump both drivers until `pred` is true or the budget runs out. */
static bool pump_until(PcSlippiNet* a, PcSlippiNet* b, bool (*pred)(void*), void* ctx, int ms) {
    uint64_t deadline = now_us() + (uint64_t)ms * 1000ull;
    while (now_us() < deadline) {
        uint64_t t = now_us();
        pc_slippi_net_poll(a, t);
        pc_slippi_net_poll(b, t);
        if (pred(ctx)) {
            return true;
        }
        struct timespec s = {0, 1000000}; /* 1 ms */
        nanosleep(&s, NULL);
    }
    return pred(ctx);
}

static bool both_connected(void* ctx) {
    PcSlippiNet** p = ctx;
    return pc_slippi_net_status(p[0]) == PC_SLIPPI_NET_CONNECTED &&
           pc_slippi_net_status(p[1]) == PC_SLIPPI_NET_CONNECTED;
}
static bool got_selection(void* ctx) { return ((Seen*)ctx)->selections > 0; }

/* True once P2's held pad is frame 1 with our bytes. */
static bool p2_has_pad(void* ctx) {
    PcSlippiNet* b = ctx;
    PcSlippiRemotePadOutput out;
    pc_slippi_session_remote_pad(pc_slippi_net_session(b), 0, 7, &out);
    return out.latest_frame == 1 && out.data[0] == 0x11;
}

/* True once P1's newest acked frame is at least 1 (the ack came back). */
static bool p1_acked(void* ctx) {
    PcSlippiNet* a = ctx;
    return pc_slippi_session_last_acked(pc_slippi_net_session(a), 0) >= 1;
}

int main(void) {
    assert(pc_slippi_net_global_init());

    /* P2 binds a fixed loopback port; P1 dials it. */
    const uint16_t b_port = 14712;
    Seen seen_a = {0}, seen_b = {0};
    PcSlippiNet* a = make_peer(0, 0, &seen_a);    /* P1, any port */
    PcSlippiNet* b = make_peer(1, b_port, &seen_b); /* P2, fixed port */
    assert(a != NULL && b != NULL);
    pc_slippi_net_listen(b);
    assert(pc_slippi_net_connect(a, "127.0.0.1", b_port));

    PcSlippiNet* conn[2] = {a, b};
    assert(pump_until(a, b, both_connected, conn, 5000));
    printf("test_slp_net: connected P1<->P2\n");

    /* P1 sends selections; P2 receives them. */
    PcSlippiSelections sel;
    pc_slippi_selections_reset(&sel);
    sel.character_id = 9;
    sel.character_color = 2;
    sel.character_selected = true;
    sel.stage_id = 0x1F;
    sel.stage_selected = true;
    sel.rng_offset = 0x1234;
    assert(pc_slippi_net_send_selections(a, &sel));
    assert(pump_until(a, b, got_selection, &seen_b, 3000));
    assert(seen_b.last.character_id == 9 && seen_b.last.character_color == 2);
    assert(seen_b.last.stage_id == 0x1F && seen_b.last.rng_offset == 0x1234);

    /* P1 sends a pad; P2 holds it and acks it; the ack returns to P1. */
    uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE] = {0x11, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    assert(pc_slippi_net_send_pad(a, 1, pad, now_us()));
    assert(pump_until(a, b, p2_has_pad, b, 3000));
    printf("test_slp_net: pad reached P2\n");
    assert(pump_until(a, b, p1_acked, a, 3000));
    PcSlippiNetStats stats;
    pc_slippi_net_stats(a, &stats);
    assert(pc_slippi_net_status(a) == PC_SLIPPI_NET_CONNECTED);
    printf("test_slp_net: ack returned to P1 (ping %u us)\n", stats.ping_us);

    pc_slippi_net_destroy(a);
    pc_slippi_net_destroy(b);
    pc_slippi_net_global_shutdown();
    printf("test_slp_net: all checks passed\n");
    return 0;
}
