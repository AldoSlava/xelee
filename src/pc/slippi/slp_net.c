/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay ENet driver (slp_net.h). See that header for the design and
 * the upstream sources. */
#include "pc/slippi/slp_net.h"

#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>

/* A pad packet carries up to 128 pads plus its header. */
#define PKT_MAX PC_SLIPPI_PACKET_MAX

struct PcSlippiNet {
    ENetHost* host;
    ENetPeer* peer;
    PcSlippiSession session;
    PcSlippiNetCallbacks cb;
    int status;
    int disconnect_reason;
    bool conn_selected;
    /* Merged local selections, as Slippi keeps matchInfo.local. */
    PcSlippiSelections local_selections;
    /* Merged remote selections per slot. */
    PcSlippiSelections remote_selections[PC_SLIPPI_REMOTE_PLAYER_MAX];
    bool remote_selection_seen[PC_SLIPPI_REMOTE_PLAYER_MAX];
};

/* ENet's global init is refcounted by the caller, not by ENet; track it so a
 * second initialise is a no-op and shutdown only runs at zero. */
static int g_enet_refs = 0;

bool pc_slippi_net_global_init(void) {
    if (g_enet_refs > 0) {
        g_enet_refs++;
        return true;
    }
    if (enet_initialize() != 0) {
        return false;
    }
    g_enet_refs = 1;
    return true;
}

void pc_slippi_net_global_shutdown(void) {
    if (g_enet_refs > 0) {
        g_enet_refs--;
        if (g_enet_refs == 0) {
            enet_deinitialize();
        }
    }
}

void pc_slippi_net_set_callbacks(PcSlippiNet* n, const PcSlippiNetCallbacks* cb) {
    if (n == NULL) {
        return;
    }
    if (cb != NULL) {
        n->cb = *cb;
    } else {
        memset(&n->cb, 0, sizeof n->cb);
    }
}

PcSlippiNet* pc_slippi_net_create(const PcSlippiSessionConfig* cfg, uint16_t local_udp_port,
                                  const PcSlippiNetCallbacks* cb) {
    if (local_udp_port != 0 && g_enet_refs == 0) {
        return NULL; /* ENet must be initialised before binding a port */
    }
    PcSlippiNet* n = calloc(1, sizeof *n);
    if (n == NULL) {
        return NULL;
    }
    pc_slippi_session_init(&n->session, cfg);
    if (cb != NULL) {
        n->cb = *cb;
    }
    n->status = PC_SLIPPI_NET_UNSET;
    for (int i = 0; i < cfg->remote_count; i++) {
        pc_slippi_selections_reset(&n->remote_selections[i]);
    }
    pc_slippi_selections_reset(&n->local_selections);

    /* 10 peers, 3 channels -- Slippi's exact enet_host_create arguments. */
    ENetAddress addr;
    ENetAddress* local = NULL;
    if (local_udp_port != 0) {
        addr.host = ENET_HOST_ANY;
        addr.port = local_udp_port;
        local = &addr;
    }
    n->host = enet_host_create(local, 10, PC_SLIPPI_ENET_CHANNELS, 0, 0);
    if (n->host == NULL) {
        free(n);
        return NULL;
    }
    return n;
}

void pc_slippi_net_destroy(PcSlippiNet* n) {
    if (n == NULL) {
        return;
    }
    if (n->host != NULL) {
        if (n->peer != NULL) {
            enet_peer_reset(n->peer);
        }
        enet_host_destroy(n->host);
    }
    free(n);
}

bool pc_slippi_net_connect(PcSlippiNet* n, const char* ip, uint16_t port) {
    if (n == NULL || n->host == NULL || n->peer != NULL) {
        return false;
    }
    ENetAddress addr;
    if (enet_address_set_host(&addr, ip) != 0) {
        return false;
    }
    addr.port = port;
    n->peer = enet_host_connect(n->host, &addr, PC_SLIPPI_ENET_CHANNELS, 0);
    if (n->peer == NULL) {
        return false;
    }
    n->status = PC_SLIPPI_NET_INITIATED;
    return true;
}

void pc_slippi_net_listen(PcSlippiNet* n) {
    /* Nothing to do beyond having a host: ENet accepts incoming connections on
     * it automatically. Present so a caller states its intent. */
    (void)n;
}

/* Send one already-built packet on the right channel. Pads and acks are
 * UNSEQUENCED so loss/reorder is handled by the Slippi protocol; everything
 * else is RELIABLE. Reproduces upstream's channel choice exactly, including
 * the inline ack's channel 2. */
static void send_raw(PcSlippiNet* n, const void* data, size_t len, uint8_t mid) {
    if (n->host == NULL || n->peer == NULL) {
        return;
    }
    enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
    uint8_t channel = PC_SLIPPI_CH_RELIABLE;
    if (mid == PC_SLIPPI_MSG_PAD || mid == PC_SLIPPI_MSG_PAD_ACK) {
        flags = ENET_PACKET_FLAG_UNSEQUENCED;
        channel = PC_SLIPPI_CH_PAD;
    }
    ENetPacket* p = enet_packet_create(data, len, flags);
    if (p == NULL) {
        return;
    }
    if (enet_peer_send(n->peer, channel, p) != 0) {
        enet_packet_destroy(p);
    }
}

/* The inline ack is sent on channel 2, as upstream does. */
static void send_ack_raw(PcSlippiNet* n, const void* data, size_t len) {
    if (n->host == NULL || n->peer == NULL) {
        return;
    }
    ENetPacket* p = enet_packet_create(data, len, ENET_PACKET_FLAG_UNSEQUENCED);
    if (p == NULL) {
        return;
    }
    if (enet_peer_send(n->peer, PC_SLIPPI_CH_PAD_ACK, p) != 0) {
        enet_packet_destroy(p);
    }
}

bool pc_slippi_net_send_pad(PcSlippiNet* n, int32_t frame,
                            const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE], uint64_t now_us) {
    if (n == NULL || n->host == NULL || n->peer == NULL) {
        return false;
    }
    if (n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    uint8_t buf[PKT_MAX];
    size_t len = 0;
    if (!pc_slippi_session_queue_local_pad(&n->session, frame, pad, now_us, buf, sizeof buf, &len)) {
        return false;
    }
    send_raw(n, buf, len, buf[0]);
    return true;
}

bool pc_slippi_net_resend_pad(PcSlippiNet* n) {
    if (n == NULL || n->host == NULL || n->peer == NULL
        || n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    uint8_t buf[PKT_MAX];
    size_t len = 0;
    if (!pc_slippi_session_resend(&n->session, buf, sizeof buf, &len)) {
        return false;
    }
    send_raw(n, buf, len, buf[0]);
    return true;
}

bool pc_slippi_net_send_selections(PcSlippiNet* n, const PcSlippiSelections* s) {
    if (n == NULL || n->host == NULL || n->peer == NULL || n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    /* Merge into the local pick the way Slippi does, then send the whole
     * merged state (not just the delta). */
    pc_slippi_selections_merge(&n->local_selections, s);
    n->local_selections.player_idx = n->session.cfg.local_port;
    uint8_t buf[64];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    pc_slippi_write_selections(&w, &n->local_selections);
    if (!w.ok) {
        return false;
    }
    send_raw(n, buf, w.len, buf[0]);
    return true;
}

bool pc_slippi_net_send_chat(PcSlippiNet* n, int message_id) {
    if (n == NULL || n->host == NULL || n->peer == NULL || n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    uint8_t buf[16];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    if (!pc_slippi_write_chat(&w, message_id, n->session.cfg.local_port)) {
        return false;
    }
    send_raw(n, buf, w.len, buf[0]);
    return true;
}

bool pc_slippi_net_send_conn_selected(PcSlippiNet* n) {
    if (n == NULL || n->host == NULL || n->peer == NULL || n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    n->conn_selected = true;
    uint8_t buf[1];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_CONN_SELECTED);
    send_raw(n, buf, w.len, buf[0]);
    return true;
}

bool pc_slippi_net_send_setup(PcSlippiNet* n, uint32_t seed, int32_t start_frame) {
    if (n == NULL || n->host == NULL || n->peer == NULL || n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    uint8_t buf[16];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_SETUP);
    pc_slippi_put_u32(&w, seed);
    pc_slippi_put_i32(&w, start_frame);
    send_raw(n, buf, w.len, buf[0]);
    return true;
}

bool pc_slippi_net_send_rel(PcSlippiNet* n, uint8_t type, const void* payload, int len) {
    if (n == NULL || n->host == NULL || n->peer == NULL || n->status != PC_SLIPPI_NET_CONNECTED) {
        return false;
    }
    if (len < 0 || len > 255) {
        return false;
    }
    uint8_t buf[1 + 1 + 255];
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, buf, sizeof buf);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_REL);
    pc_slippi_put_u8(&w, type);
    if (len > 0) {
        pc_slippi_put_bytes(&w, payload, (size_t)len);
    }
    if (!w.ok) {
        return false;
    }
    send_raw(n, buf, w.len, buf[0]);
    return true;
}

void pc_slippi_net_disconnect(PcSlippiNet* n, int reason) {
    if (n == NULL) {
        return;
    }
    n->disconnect_reason = reason;
    if (n->peer != NULL) {
        /* ENet carries the reason in the disconnect data field (u32). */
        enet_peer_disconnect(n->peer, (enet_uint32)reason);
    }
    n->status = PC_SLIPPI_NET_DISCONNECTED;
}

const PcSlippiSession* pc_slippi_net_session(const PcSlippiNet* n) {
    return n != NULL ? &n->session : NULL;
}
PcSlippiSession* pc_slippi_net_session_mut(PcSlippiNet* n) {
    return n != NULL ? &n->session : NULL;
}
int pc_slippi_net_status(const PcSlippiNet* n) { return n != NULL ? n->status : PC_SLIPPI_NET_UNSET; }

void pc_slippi_net_stats(const PcSlippiNet* n, PcSlippiNetStats* out) {
    memset(out, 0, sizeof *out);
    if (n == NULL) {
        return;
    }
    out->connect_status = n->status;
    out->disconnect_reason = n->disconnect_reason;
    out->connection_selected = n->conn_selected;
    for (int i = 0; i < n->session.cfg.remote_count; i++) {
        uint32_t p = pc_slippi_session_ping_us(&n->session, i);
        if (p > out->ping_us) {
            out->ping_us = p;
        }
    }
}

bool pc_slippi_net_remote_selections(const PcSlippiNet* n, int remote_slot,
                                     PcSlippiSelections* out) {
    if (n == NULL || remote_slot < 0 || remote_slot >= n->session.cfg.remote_count) {
        return false;
    }
    *out = n->remote_selections[remote_slot];
    return n->remote_selection_seen[remote_slot];
}

void pc_slippi_net_set_local_checksum(PcSlippiNet* n, int32_t frame, uint32_t value) {
    pc_slippi_session_set_local_checksum(&n->session, frame, value);
}

void pc_slippi_net_start_game(PcSlippiNet* n, uint64_t now_us) {
    pc_slippi_session_start_game(&n->session, now_us);
    for (int i = 0; i < n->session.cfg.remote_count; i++) {
        pc_slippi_selections_reset(&n->remote_selections[i]);
        n->remote_selection_seen[i] = false;
    }
}

int32_t pc_slippi_net_time_offset_us(const PcSlippiNet* n) {
    return n != NULL ? pc_slippi_session_time_offset_us(&n->session) : 0;
}

/* Handle one received packet's body (after the message id). */
static void on_message(PcSlippiNet* n, uint8_t mid, PcSlippiReader* r, uint64_t now_us) {
    (void)now_us;
    switch (mid) {
    case PC_SLIPPI_MSG_PAD: {
        uint8_t ack[16];
        size_t ack_len = 0;
        int slot = -1;
        if (pc_slippi_session_on_pads(&n->session, r, &slot, ack, sizeof ack, &ack_len)) {
            send_ack_raw(n, ack, ack_len);
        }
        break;
    }
    case PC_SLIPPI_MSG_PAD_ACK: {
        int32_t frame;
        uint8_t port;
        if (pc_slippi_read_pad_ack(r, &frame, &port)) {
            uint8_t idx = pc_slippi_player_idx_from_port(port, n->session.cfg.local_port);
            if (idx < n->session.cfg.remote_count) {
                pc_slippi_session_on_pad_ack(&n->session, idx, frame, now_us);
            }
        }
        break;
    }
    case PC_SLIPPI_MSG_MATCH_SELECTIONS: {
        PcSlippiSelections s;
        if (pc_slippi_read_selections(r, &s) && !s.error) {
            uint8_t idx = pc_slippi_player_idx_from_port(s.player_idx, n->session.cfg.local_port);
            if (idx < n->session.cfg.remote_count) {
                pc_slippi_selections_merge(&n->remote_selections[idx], &s);
                n->remote_selection_seen[idx] = true;
                if (n->cb.on_selections != NULL) {
                    n->cb.on_selections(n->cb.user, idx, &n->remote_selections[idx]);
                }
            }
        }
        break;
    }
    case PC_SLIPPI_MSG_CHAT: {
        PcSlippiSelections s;
        if (pc_slippi_read_chat(r, &s) && !s.error) {
            uint8_t idx = pc_slippi_player_idx_from_port(s.player_idx, n->session.cfg.local_port);
            if (idx < n->session.cfg.remote_count && n->cb.on_chat != NULL) {
                n->cb.on_chat(n->cb.user, idx, s.message_id);
            }
        }
        break;
    }
    case PC_SLIPPI_MSG_CONN_SELECTED:
        n->conn_selected = true;
        if (n->cb.on_conn_selected != NULL) {
            n->cb.on_conn_selected(n->cb.user);
        }
        break;
    case PC_SLIPPI_MSG_SETUP: {
        /* melee-pc direct-session setup: seed + start frame (see slp_proto.h). */
        uint32_t seed = pc_slippi_get_u32(r);
        int32_t start_frame = pc_slippi_get_i32(r);
        if (r->ok && n->cb.on_setup != NULL) {
            n->cb.on_setup(n->cb.user, seed, start_frame);
        }
        break;
    }
    case PC_SLIPPI_MSG_REL: {
        /* Tunnelled engine reliable message: type u8, then raw payload. */
        uint8_t type = pc_slippi_get_u8(r);
        size_t remaining = pc_slippi_reader_remaining(r);
        if (r->ok && remaining <= PC_SLIPPI_PACKET_MAX && n->cb.on_rel != NULL) {
            n->cb.on_rel(n->cb.user, type, r->data + r->pos, (int)remaining);
        }
        break;
    }
    default:
        /* Unknown or Ranked-only id: ignored, exactly as the reference does. */
        break;
    }
}

/* Service one event. Returns true if it handled one. */
static bool service_one(PcSlippiNet* n, uint64_t now_us, uint32_t timeout_ms, bool* disconnected) {
    ENetEvent ev;
    int rc = enet_host_service(n->host, &ev, timeout_ms);
    if (rc <= 0) {
        return false;
    }
    switch (ev.type) {
    case ENET_EVENT_TYPE_CONNECT:
        if (n->peer == NULL) {
            n->peer = ev.peer;
        }
        if (n->status != PC_SLIPPI_NET_CONNECTED) {
            n->status = PC_SLIPPI_NET_CONNECTED;
            if (n->cb.on_connected != NULL) {
                n->cb.on_connected(n->cb.user);
            }
        }
        break;
    case ENET_EVENT_TYPE_RECEIVE: {
        const uint8_t* data = ev.packet->data;
        size_t len = ev.packet->dataLength;
        if (len >= 1) {
            PcSlippiReader r;
            pc_slippi_reader_init(&r, data + 1, len - 1);
            on_message(n, data[0], &r, now_us);
        }
        enet_packet_destroy(ev.packet);
        break;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
        /* ENet puts the peer's disconnect reason in ev.data. */
        n->status = PC_SLIPPI_NET_DISCONNECTED;
        n->disconnect_reason = (int)ev.data;
        *disconnected = true;
        if (n->cb.on_disconnected != NULL) {
            n->cb.on_disconnected(n->cb.user, (int)ev.data);
        }
        break;
    default:
        break;
    }
    return true;
}

bool pc_slippi_net_poll(PcSlippiNet* n, uint64_t now_us) {
    if (n == NULL || n->host == NULL) {
        return false;
    }
    bool any = false;
    bool disconnected = false;
    /* Drain everything ready. */
    while (service_one(n, now_us, 0, &disconnected)) {
        any = true;
    }
    return any;
}

bool pc_slippi_net_service(PcSlippiNet* n, uint64_t now_us, uint32_t timeout_ms) {
    if (n == NULL || n->host == NULL) {
        return false;
    }
    bool disconnected = false;
    if (!service_one(n, now_us, timeout_ms, &disconnected)) {
        return false;
    }
    /* Then drain any rest. */
    while (service_one(n, now_us, 0, &disconnected)) {
    }
    return true;
}
