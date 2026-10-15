/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi transport adapter (slp_adapter.h). See that header for the design. */
#include "pc/slippi/slp_adapter.h"

void pc_slippi_adapter_init(PcSlippiAdapter* a, PcSlippiNet* net, PcSlippiFrameFn on_frame,
                            PcSlippiSetupFn on_setup, void* user) {
    a->net = net;
    a->on_frame = on_frame;
    a->on_setup = on_setup;
    a->user = user;
    for (int i = 0; i < PC_SLIPPI_REMOTE_PLAYER_MAX; i++) {
        a->delivered_upto[i] = 0;
    }
    a->started = false;
}

void pc_slippi_adapter_start_game(PcSlippiAdapter* a, uint64_t now_us) {
    for (int i = 0; i < PC_SLIPPI_REMOTE_PLAYER_MAX; i++) {
        a->delivered_upto[i] = 0;
    }
    a->started = true;
    pc_slippi_net_start_game(a->net, now_us);
}

bool pc_slippi_adapter_connected(const PcSlippiAdapter* a) {
    return pc_slippi_net_status(a->net) == PC_SLIPPI_NET_CONNECTED;
}

int pc_slippi_adapter_status(const PcSlippiAdapter* a) {
    return pc_slippi_net_status(a->net);
}

int pc_slippi_adapter_disconnect_reason(const PcSlippiAdapter* a) {
    PcSlippiNetStats st;
    pc_slippi_net_stats(a->net, &st);
    return st.disconnect_reason;
}

int32_t pc_slippi_adapter_time_offset_us(const PcSlippiAdapter* a) {
    return pc_slippi_net_time_offset_us(a->net);
}

/* Walk from delivered_upto+1 upward while the session still holds the next
 * frame. A frame the session no longer holds (dropped old, or a gap that never
 * arrived) stops the walk; the next pump retries from the same place. This is
 * the adapter's copy of on_inputs' contiguous-walk rule: `s_remote_have` only
 * advances over frames that are actually present, never over a hole. */
static void deliver_remote(PcSlippiAdapter* a, int remote_slot) {
    PcSlippiSession* s = pc_slippi_net_session_mut(a->net);
    if (s == NULL) {
        return;
    }
    const PcSlippiSession* cs = s;
    int32_t latest = pc_slippi_session_remote_latest(cs, remote_slot);
    if (latest <= a->delivered_upto[remote_slot]) {
        return;
    }
    uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE];
    for (int32_t f = a->delivered_upto[remote_slot] + 1; f <= latest; f++) {
        if (!pc_slippi_session_pad_for_frame(cs, remote_slot, f, pad)) {
            break; /* a hole: stop here, do not advance past it */
        }
        a->delivered_upto[remote_slot] = f;
        if (a->on_frame != NULL) {
            a->on_frame(a->user, remote_slot, f, pad);
        }
    }
}

bool pc_slippi_adapter_pump(PcSlippiAdapter* a, uint64_t now_us) {
    pc_slippi_net_poll(a->net, now_us);
    PcSlippiSession* s = pc_slippi_net_session_mut(a->net);
    if (s == NULL) {
        return false;
    }
    bool any = false;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        int32_t before = a->delivered_upto[i];
        deliver_remote(a, i);
        if (a->delivered_upto[i] != before) {
            any = true;
        }
    }
    return any;
}

bool pc_slippi_adapter_send_local(PcSlippiAdapter* a, int32_t frame, const void* wire_pad8,
                                  uint64_t now_us) {
    if (!pc_slippi_adapter_connected(a)) {
        return false;
    }
    return pc_slippi_net_send_pad(a->net, frame, (const uint8_t*)wire_pad8, now_us);
}

bool pc_slippi_adapter_send_setup(PcSlippiAdapter* a, uint32_t seed, int32_t start_frame) {
    if (!pc_slippi_adapter_connected(a)) {
        return false;
    }
    return pc_slippi_net_send_setup(a->net, seed, start_frame);
}

bool pc_slippi_adapter_resend_local(PcSlippiAdapter* a) {
    if (!pc_slippi_adapter_connected(a)) {
        return false;
    }
    return pc_slippi_net_resend_pad(a->net);
}

void pc_slippi_adapter_set_checksum(PcSlippiAdapter* a, int32_t frame, uint32_t checksum) {
    pc_slippi_net_set_local_checksum(a->net, frame, checksum);
}

bool pc_slippi_adapter_remote_checksum(const PcSlippiAdapter* a, int remote_slot,
                                       int32_t* frame, uint32_t* value) {
    const PcSlippiSession* s = pc_slippi_net_session(a->net);
    if (s == NULL || remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        return false;
    }
    if (frame != NULL) {
        *frame = s->rx_checksum[remote_slot].frame;
    }
    if (value != NULL) {
        *value = s->rx_checksum[remote_slot].value;
    }
    return true;
}

int32_t pc_slippi_adapter_remote_have(const PcSlippiAdapter* a) {
    const PcSlippiSession* s = pc_slippi_net_session(a->net);
    if (s == NULL) {
        return -1;
    }
    int32_t best = -1;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        if (a->delivered_upto[i] > best) {
            best = a->delivered_upto[i];
        }
    }
    return best;
}

int32_t pc_slippi_adapter_remote_newest(const PcSlippiAdapter* a) {
    const PcSlippiSession* s = pc_slippi_net_session(a->net);
    if (s == NULL) {
        return 0;
    }
    int32_t best = 0;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        int32_t latest = pc_slippi_session_remote_latest(s, i);
        if (latest > best) {
            best = latest;
        }
    }
    return best;
}

int32_t pc_slippi_adapter_last_acked(const PcSlippiAdapter* a, int remote_slot) {
    const PcSlippiSession* s = pc_slippi_net_session(a->net);
    if (s == NULL || remote_slot < 0) {
        return 0;
    }
    return pc_slippi_session_last_acked(s, remote_slot);
}
