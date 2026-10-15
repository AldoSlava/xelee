/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay session core (slp_session.h). See that header for the
 * behaviour and the upstream sources it reproduces. */
#include "pc/slippi/slp_session.h"

#include <string.h>

/* ---- pad-queue ring helpers -------------------------------------------
 * Both queues are newest-first rings: logical position i (0 = newest) lives at
 * (head + QUEUE_MAX - i) % QUEUE_MAX. Writing a new newest advances head; when
 * the ring is full the write lands on the oldest slot, which is exactly the
 * drop Slippi's bounded queue performs. */
#define QMAX PC_SLIPPI_PAD_QUEUE_MAX

static const PcSlippiPad* local_at_c(const PcSlippiSession* s, int i) {
    int idx = ((s->local_head - i) % QMAX + QMAX) % QMAX;
    return &s->local[idx];
}
static void local_push_newest(PcSlippiSession* s, const PcSlippiPad* pad) {
    s->local_head = (s->local_head + 1) % QMAX;
    s->local[s->local_head] = *pad;
    if (s->local_count < QMAX) {
        s->local_count++;
    }
}

static const PcSlippiPad* rx_at_c(const PcSlippiPadQueue* q, int i) {
    int idx = ((q->head - i) % QMAX + QMAX) % QMAX;
    return &q->pads[idx];
}
static void rx_push_newest(PcSlippiPadQueue* q, const PcSlippiPad* pad) {
    q->head = (q->head + 1) % QMAX;
    q->pads[q->head] = *pad;
    if (q->count < QMAX) {
        q->count++;
    }
}
/* Drop the oldest `n` pads (from the back). */
static void rx_pop_oldest(PcSlippiPadQueue* q, int n) {
    if (n >= q->count) {
        q->count = 0;
        return;
    }
    q->count -= n;
}

void pc_slippi_session_init(PcSlippiSession* s, const PcSlippiSessionConfig* cfg) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    if (s->cfg.remote_count > PC_SLIPPI_REMOTE_PLAYER_MAX) {
        s->cfg.remote_count = PC_SLIPPI_REMOTE_PLAYER_MAX;
    }
    for (int i = 0; i < s->cfg.remote_count; i++) {
        s->remote_active[i] = true;
    }
}

void pc_slippi_session_start_game(PcSlippiSession* s, uint64_t now_us) {
    for (int i = 0; i < s->cfg.remote_count; i++) {
        s->rx[i].count = 0;
        s->rx[i].head = 0;
        s->rx_checksum[i].frame = 0;
        s->rx_checksum[i].value = 0;
        s->last_frame_acked[i] = 0;
        s->ack_timer_n[i] = 0;
        s->last_sent[i].frame = 0;
        s->last_sent[i].time_us = now_us;
        s->offset_n[i] = 0;
        s->offset_idx[i] = 0;
    }
    s->game_started = false;
    s->local_count = 0;
    s->local_head = 0;
    s->local_checksum_frame = 0;
    s->local_checksum = 0;
}

uint8_t pc_slippi_session_remote_port(const PcSlippiSession* s, int remote_slot) {
    return (uint8_t)(remote_slot >= s->cfg.local_port ? remote_slot + 1 : remote_slot);
}

void pc_slippi_session_set_remote_active(PcSlippiSession* s, int remote_slot, bool active) {
    if (remote_slot >= 0 && remote_slot < s->cfg.remote_count) {
        s->remote_active[remote_slot] = active;
    }
}

void pc_slippi_session_set_local_checksum(PcSlippiSession* s, int32_t frame, uint32_t value) {
    s->local_checksum_frame = frame;
    s->local_checksum = value;
}

/* Build the pad packet from the current local queue (newest frame, our port,
 * our checksum, then every queued pad newest-first). Shared by the queue and
 * resend paths so the wire image has one definition. */
static bool build_pad_packet(const PcSlippiSession* s, void* out, size_t out_cap,
                             size_t* out_len) {
    if (s->local_count == 0) {
        return false;
    }
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, out, out_cap);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_PAD);
    pc_slippi_put_i32(&w, local_at_c(s, 0)->frame);
    pc_slippi_put_u8(&w, s->cfg.local_port);
    pc_slippi_put_i32(&w, s->local_checksum_frame);
    pc_slippi_put_u32(&w, s->local_checksum);
    for (int i = 0; i < s->local_count; i++) {
        pc_slippi_put_bytes(&w, local_at_c(s, i)->data, PC_SLIPPI_PAD_DATA_SIZE);
    }
    *out_len = w.len;
    return w.ok;
}

bool pc_slippi_session_resend(PcSlippiSession* s, void* out, size_t out_cap, size_t* out_len) {
    *out_len = 0;
    if (s->local_count == 0) {
        return false;
    }
    /* Trim acked frames the same way a fresh send would, so a peer that has
     * acked cannot keep the window from shrinking. */
    int32_t min_ack = INT32_MAX;
    bool any_active = false;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        if (!s->remote_active[i]) {
            continue;
        }
        any_active = true;
        if (s->last_frame_acked[i] < min_ack) {
            min_ack = s->last_frame_acked[i];
        }
    }
    if (!any_active) {
        min_ack = local_at_c(s, 0)->frame;
    }
    int32_t floor_frame = local_at_c(s, 0)->frame - PC_SLIPPI_MAX_INPUT_FRAMES;
    if (min_ack < floor_frame) {
        min_ack = floor_frame;
    }
    while (s->local_count > 1 && local_at_c(s, s->local_count - 1)->frame < min_ack) {
        s->local_count--;
    }
    return build_pad_packet(s, out, out_cap, out_len);
}

bool pc_slippi_session_queue_local_pad(PcSlippiSession* s, int32_t frame,
                                       const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE], uint64_t now_us,
                                       void* out, size_t out_cap, size_t* out_len) {
    PcSlippiPad p;
    p.frame = frame;
    memcpy(p.data, pad, PC_SLIPPI_PAD_DATA_SIZE);
    local_push_newest(s, &p);

    /* Trim acked frames off the back, exactly as SendSlippiPad does: the
     * lowest ack across active remotes, floored at newest-128 so a peer that
     * stops acking cannot pin the whole queue forever. */
    int32_t min_ack = INT32_MAX;
    bool any_active = false;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        if (!s->remote_active[i]) {
            continue;
        }
        any_active = true;
        if (s->last_frame_acked[i] < min_ack) {
            min_ack = s->last_frame_acked[i];
        }
    }
    if (!any_active) {
        min_ack = frame; /* no peer to ack: keep only the newest */
    }
    int32_t newest = local_at_c(s, 0)->frame;
    int32_t floor_frame = newest - PC_SLIPPI_MAX_INPUT_FRAMES;
    if (min_ack < floor_frame) {
        min_ack = floor_frame;
    }
    while (s->local_count > 1 && local_at_c(s, s->local_count - 1)->frame < min_ack) {
        s->local_count--; /* drop the oldest */
    }

    /* Build the packet: newest frame, our port, newest pad's checksum, then
     * every queued pad newest-first. */
    PcSlippiWriter w;
    pc_slippi_writer_init(&w, out, out_cap);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_PAD);
    pc_slippi_put_i32(&w, frame);
    pc_slippi_put_u8(&w, s->cfg.local_port);
    pc_slippi_put_i32(&w, s->local_checksum_frame);
    pc_slippi_put_u32(&w, s->local_checksum);
    for (int i = 0; i < s->local_count; i++) {
        pc_slippi_put_bytes(&w, local_at_c(s, i)->data, PC_SLIPPI_PAD_DATA_SIZE);
    }
    *out_len = w.len;
    if (!w.ok) {
        return false;
    }

    /* Record the send for the peer's offset sample and for our ack timers, on
     * every remote. */
    for (int i = 0; i < s->cfg.remote_count; i++) {
        s->last_sent[i].frame = frame;
        s->last_sent[i].time_us = now_us;
        if (s->ack_timer_n[i] < PC_SLIPPI_ACK_TIMERS_MAX) {
            s->ack_timers[i][s->ack_timer_n[i]].frame = frame;
            s->ack_timers[i][s->ack_timer_n[i]].time_us = now_us;
            s->ack_timer_n[i]++;
        } else {
            /* Full: drop the oldest so the newest stays timed. */
            memmove(&s->ack_timers[i][0], &s->ack_timers[i][1],
                    (PC_SLIPPI_ACK_TIMERS_MAX - 1) * sizeof s->ack_timers[i][0]);
            s->ack_timers[i][PC_SLIPPI_ACK_TIMERS_MAX - 1].frame = frame;
            s->ack_timers[i][PC_SLIPPI_ACK_TIMERS_MAX - 1].time_us = now_us;
        }
    }
    s->game_started = true;
    return true;
}

uint32_t pc_slippi_session_on_pad_ack(PcSlippiSession* s, int remote_slot, int32_t frame,
                                      uint64_t now_us) {
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        return 0;
    }
    if (frame > s->last_frame_acked[remote_slot]) {
        s->last_frame_acked[remote_slot] = frame;
    }
    /* Drop timers older than the ack. */
    int n = s->ack_timer_n[remote_slot];
    int drop = 0;
    while (drop < n && s->ack_timers[remote_slot][drop].frame < frame) {
        drop++;
    }
    if (drop > 0) {
        memmove(&s->ack_timers[remote_slot][0], &s->ack_timers[remote_slot][drop],
                (size_t)(n - drop) * sizeof s->ack_timers[remote_slot][0]);
        s->ack_timer_n[remote_slot] = n - drop;
        n -= drop;
    }
    /* Only a timer for exactly this frame is a ping. */
    if (n == 0 || s->ack_timers[remote_slot][0].frame != frame) {
        return 0;
    }
    uint64_t sent = s->ack_timers[remote_slot][0].time_us;
    memmove(&s->ack_timers[remote_slot][0], &s->ack_timers[remote_slot][1],
            (size_t)(n - 1) * sizeof s->ack_timers[remote_slot][0]);
    s->ack_timer_n[remote_slot] = n - 1;
    uint32_t rtt = (uint32_t)(now_us - sent);
    s->ping_us[remote_slot] = rtt;
    return rtt;
}

uint32_t pc_slippi_session_ping_us(const PcSlippiSession* s, int remote_slot) {
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        return 0;
    }
    return s->ping_us[remote_slot];
}

int32_t pc_slippi_session_remote_latest(const PcSlippiSession* s, int remote_slot) {
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count || s->rx[remote_slot].count == 0) {
        return 0;
    }
    return rx_at_c(&s->rx[remote_slot], 0)->frame;
}

bool pc_slippi_session_pad_for_frame(const PcSlippiSession* s, int remote_slot, int32_t frame,
                                     uint8_t out[PC_SLIPPI_PAD_DATA_SIZE]) {
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        return false;
    }
    const PcSlippiPadQueue* q = &s->rx[remote_slot];
    if (q->count == 0) {
        return false;
    }
    /* Newest is at logical index 0, older at count-1, contiguous by one. */
    int32_t newest = rx_at_c(q, 0)->frame;
    int32_t oldest = rx_at_c(q, q->count - 1)->frame;
    if (frame > newest || frame < oldest) {
        return false;
    }
    int idx = newest - frame; /* position from newest */
    const PcSlippiPad* p = rx_at_c(q, idx);
    if (p->frame != frame) {
        return false; /* a gap slipped in; do not hand out a wrong frame */
    }
    memcpy(out, p->data, PC_SLIPPI_PAD_DATA_SIZE);
    return true;
}

int32_t pc_slippi_session_last_acked(const PcSlippiSession* s, int remote_slot) {
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        return 0;
    }
    return s->last_frame_acked[remote_slot];
}

bool pc_slippi_session_on_pads(PcSlippiSession* s, PcSlippiReader* r, int* out_remote_slot,
                               void* ack_out, size_t ack_cap, size_t* ack_len) {
    /* The port in the packet identifies which remote sent it. */
    int32_t frame = pc_slippi_get_i32(r);
    uint8_t port = pc_slippi_get_u8(r);
    int32_t ck_frame = pc_slippi_get_i32(r);
    uint32_t ck = pc_slippi_get_u32(r);
    if (!r->ok) {
        return false;
    }
    uint8_t idx = pc_slippi_player_idx_from_port(port, s->cfg.local_port);
    if (idx >= s->cfg.remote_count) {
        return false;
    }
    if (out_remote_slot != NULL) {
        *out_remote_slot = idx;
    }

    /* How many new frames this packet carries. headFrame is our newest held
     * frame (0 when empty), and the payload must hold the rest. */
    int32_t head_frame = s->rx[idx].count > 0 ? rx_at_c(&s->rx[idx], 0)->frame : 0;
    int64_t copy = (int64_t)frame - (int64_t)head_frame;
    if (copy < 0) {
        copy = 0; /* all old; not useful */
    }
    if (copy > PC_SLIPPI_MAX_INPUT_FRAMES) {
        return false;
    }
    size_t remaining = pc_slippi_reader_remaining(r);
    if ((int64_t)(remaining / PC_SLIPPI_PAD_DATA_SIZE) < copy) {
        return false;
    }

    /* The packet's pads are newest-first: pad i is frame - i. Insert them in
     * ascending frame order so the newest ends up front, matching Slippi's
     * push_front loop. */
    for (int64_t i = copy - 1; i >= 0; i--) {
        PcSlippiPad p;
        p.frame = (int32_t)(frame - i);
        memcpy(p.data, r->data + r->pos + (size_t)i * PC_SLIPPI_PAD_DATA_SIZE,
               PC_SLIPPI_PAD_DATA_SIZE);
        rx_push_newest(&s->rx[idx], &p);
    }
    s->rx_checksum[idx].frame = ck_frame;
    s->rx_checksum[idx].value = ck;

    if (copy <= 0) {
        return false; /* nothing new, and Slippi only acks when it copied */
    }

    PcSlippiWriter w;
    pc_slippi_writer_init(&w, ack_out, ack_cap);
    pc_slippi_put_u8(&w, PC_SLIPPI_MSG_PAD_ACK);
    pc_slippi_put_i32(&w, frame);
    pc_slippi_put_u8(&w, s->cfg.local_port);
    *ack_len = w.len;
    return w.ok;
}

void pc_slippi_session_remote_pad(const PcSlippiSession* s, int remote_slot, int max_frames,
                                  PcSlippiRemotePadOutput* out) {
    memset(out, 0, sizeof *out);
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        out->count = 1; /* one empty 12-byte pad, as Slippi returns for bad index */
        return;
    }
    const PcSlippiPadQueue* q = &s->rx[remote_slot];
    out->player_idx = pc_slippi_session_remote_port(s, remote_slot);
    out->is_disconnected = !s->remote_active[remote_slot];
    if (q->count == 0) {
        out->latest_frame = 0;
        out->count = 1; /* one empty pad */
        return;
    }
    out->checksum_frame = s->rx_checksum[remote_slot].frame;
    out->checksum = s->rx_checksum[remote_slot].value;

    /* Slippi copies up to max_frames starting from the OLDEST held pad (it
     * iterates rbegin->rend and breaks at the limit), not the newest: the
     * consumer is the rollback predictor, which needs the oldest unconfirmed
     * frames first. The output `data` is then arranged NEWEST-first (Slippi
     * inserts each at data.begin() as it walks oldest->newest), so position 0
     * is the newest of the selected set and position take-1 the oldest. */
    int avail = q->count;
    int take = avail < max_frames ? avail : max_frames;
    int base = avail - take; /* oldest position among those taken */
    out->latest_frame = 0;
    int n = 0;
    for (int i = base; i < avail; i++) {
        const PcSlippiPad* p = rx_at_c(q, i);
        if (p->frame > out->latest_frame) {
            out->latest_frame = p->frame;
        }
        /* Slippi walks the queue oldest->newest inserting each at data.begin(),
         * so the result is newest-first: selected-queue index i (0 = newest,
         * base = oldest selected) lands at output slot i - base. */
        int dst = i - base;
        memcpy(out->data + (size_t)dst * PC_SLIPPI_PAD_FULL_SIZE, p->data, PC_SLIPPI_PAD_DATA_SIZE);
        /* bytes 8..11 are the 4 bytes Slippi leaves zero on the game-facing pad */
        n++;
    }
    out->count = n;
}

void pc_slippi_session_drop_old(PcSlippiSession* s, int32_t finalized_frame) {
    for (int i = 0; i < s->cfg.remote_count; i++) {
        PcSlippiPadQueue* q = &s->rx[i];
        int drop = 0;
        /* The oldest is at logical index count-1; drop while older than
         * finalized, keeping at least one. */
        while (drop < q->count - 1 && rx_at_c(q, q->count - 1 - drop)->frame < finalized_frame) {
            drop++;
        }
        rx_pop_oldest(q, drop);
    }
}

int32_t pc_slippi_session_time_offset_us(const PcSlippiSession* s) {
    bool any_samples = false;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        if (s->remote_active[i] && s->offset_n[i] > 0) {
            any_samples = true;
            break;
        }
    }
    if (!any_samples) {
        return 0;
    }
    bool have = false;
    int32_t best = 0;
    for (int i = 0; i < s->cfg.remote_count; i++) {
        if (!s->remote_active[i] || s->offset_n[i] == 0) {
            continue;
        }
        int32_t buf[PC_SLIPPI_OFFSET_SAMPLES];
        int n = (int)s->offset_n[i];
        memcpy(buf, s->offset_buf[i], (size_t)n * sizeof buf[0]);
        /* Insertion sort: n <= 30. */
        for (int a = 1; a < n; a++) {
            int32_t key = buf[a];
            int b = a - 1;
            while (b >= 0 && buf[b] > key) {
                buf[b + 1] = buf[b];
                b--;
            }
            buf[b + 1] = key;
        }
        int off = n / 3;
        int end = n - off;
        int count = end - off;
        if (count <= 0) {
            continue;
        }
        int64_t sum = 0;
        for (int k = off; k < end; k++) {
            sum += buf[k];
        }
        int32_t mean = (int32_t)(sum / count);
        if (!have || mean < best) {
            best = mean;
            have = true;
        }
    }
    return have ? best : 0;
}

/* The offset sample is computed here rather than in on_pads because it needs
 * `now_us`, which the caller has. Kept as a separate entry point so the core
 * stays free of a clock. */
void pc_slippi_session_note_offset(PcSlippiSession* s, int remote_slot, int32_t remote_frame,
                                   uint64_t now_us) {
    if (remote_slot < 0 || remote_slot >= s->cfg.remote_count) {
        return;
    }
    uint32_t ping = s->ping_us[remote_slot];
    int64_t opponent_send_time = (int64_t)now_us - (int64_t)(ping / 2);
    PcSlippiFrameTiming timing = s->last_sent[remote_slot];
    if (!s->game_started) {
        timing.frame = 0;
        timing.time_us = (int64_t)now_us;
    }
    int64_t frame_diff = (int64_t)PC_SLIPPI_FRAME_US * ((int64_t)timing.frame - (int64_t)remote_frame);
    int64_t offset = opponent_send_time - (int64_t)timing.time_us + frame_diff;
    unsigned n = s->offset_n[remote_slot];
    unsigned idx = s->offset_idx[remote_slot];
    s->offset_buf[remote_slot][idx] = (int32_t)offset;
    s->offset_idx[remote_slot] = (idx + 1) % PC_SLIPPI_OFFSET_SAMPLES;
    if (n < PC_SLIPPI_OFFSET_SAMPLES) {
        s->offset_n[remote_slot] = n + 1;
    }
}
