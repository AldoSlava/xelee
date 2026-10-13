/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay session core: the protocol state a peer keeps, independent of
 * any socket.
 *
 * Ported for native C use in melee-pc from project-slippi/Ishiiruka
 * SlippiNetplay.cpp (the OnData handlers, SendSlippiPad, GetSlippiRemotePad,
 * CalcTimeOffsetUs, the ack timing) and Hero88go/melee-unlocked
 * port/runtime/hle/slippi_net.cpp. See docs/slippi-migration-plan.md.
 *
 * Why a transport-free core: the exact behaviour that has to match Dolphin is
 * the *bookkeeping* -- which frames count as delivered, what gets acked, how
 * the clock offset is sampled, how a pad queue is trimmed. That behaviour is
 * testable in one process by wiring two sessions together, with no ENet and no
 * timing flakiness, if the core takes `now_us` as an argument and returns the
 * bytes to send instead of sending them. slp_net.c (the ENet driver) is then a
 * thin pump around this.
 *
 * Threading: this core is single-threaded. slp_net.c owns the receive thread
 * and takes its own lock around calls here, the same split the legacy netplay
 * layer uses (src/pc/net_internal.h).
 */
#ifndef PC_SLIPPI_SLP_SESSION_H
#define PC_SLIPPI_SLP_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "pc/slippi/slp_msg.h"
#include "pc/slippi/slp_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Microseconds per Melee frame, Slippi's constant (1000000 / 59.94). Used in
 * the time-offset sample; a different value desyncs the clock estimate. */
#define PC_SLIPPI_FRAME_US 16683

/* Frames of unacked local input a pad packet carries, and the most a receiver
 * accepts. Both come from Slippi (its local queue is trimmed so it never needs
 * to send more than 128, and it rejects more). */
#define PC_SLIPPI_PAD_QUEUE_MAX 128

/* Offset samples kept per remote: Slippi's SLIPPI_ONLINE_LOCKSTEP_INTERVAL. */
#define PC_SLIPPI_OFFSET_SAMPLES 30

/* Ack-timer entries kept per remote. Bounded so a peer that stops acking
 * cannot grow the list without limit; Slippi's queue is similarly bounded by
 * the local pad queue it trims. */
#define PC_SLIPPI_ACK_TIMERS_MAX 256

typedef struct PcSlippiSessionConfig {
    uint8_t local_port;        /* controller port this peer drives (0..3) */
    uint8_t remote_count;      /* 1..PC_SLIPPI_REMOTE_PLAYER_MAX */
    uint8_t remote_ports[PC_SLIPPI_REMOTE_PLAYER_MAX]; /* each remote's port */
} PcSlippiSessionConfig;

typedef struct PcSlippiChecksum {
    int32_t frame;
    uint32_t value;
} PcSlippiChecksum;

typedef struct PcSlippiFrameTiming {
    int32_t frame;
    uint64_t time_us;
} PcSlippiFrameTiming;

/* A fixed-capacity ring of the newest remote pads, oldest at `head`. The core
 * never allocates, so it can run on the game thread. */
typedef struct PcSlippiPadQueue {
    PcSlippiPad pads[PC_SLIPPI_PAD_QUEUE_MAX];
    int count; /* 0..PC_SLIPPI_PAD_QUEUE_MAX */
    int head;  /* index of the oldest pad */
} PcSlippiPadQueue;

/* What the game reads for one remote each frame. `data` is newest-first,
 * `count` entries of the full 12-byte pad (8 wire bytes + 4 zero), matching
 * Slippi's SlippiRemotePadOutput, whose padBuf is PAD_FULL_SIZE. `latest_frame`
 * is the newest frame held. */
#define PC_SLIPPI_REMOTE_PAD_WINDOW PC_SLIPPI_ROLLBACK_MAX_FRAMES

typedef struct PcSlippiRemotePadOutput {
    int32_t latest_frame;
    int32_t checksum_frame;
    uint32_t checksum;
    uint8_t player_idx; /* on-wire port of this remote */
    bool is_disconnected;
    uint8_t data[PC_SLIPPI_REMOTE_PAD_WINDOW * PC_SLIPPI_PAD_FULL_SIZE];
    int count;
} PcSlippiRemotePadOutput;

typedef struct PcSlippiSession {
    PcSlippiSessionConfig cfg;

    /* Per-remote receive state, indexed by remote slot (0..remote_count-1). */
    PcSlippiPadQueue rx[PC_SLIPPI_REMOTE_PLAYER_MAX];
    PcSlippiChecksum rx_checksum[PC_SLIPPI_REMOTE_PLAYER_MAX];

    /* Per-remote time sync: last frame/time we told them about, and the ring of
     * offset samples. Indexed by remote slot. */
    PcSlippiFrameTiming last_sent[PC_SLIPPI_REMOTE_PLAYER_MAX];
    bool game_started;
    int32_t offset_buf[PC_SLIPPI_REMOTE_PLAYER_MAX][PC_SLIPPI_OFFSET_SAMPLES];
    unsigned offset_n[PC_SLIPPI_REMOTE_PLAYER_MAX];
    unsigned offset_idx[PC_SLIPPI_REMOTE_PLAYER_MAX];

    /* Per-remote ack tracking, indexed by remote slot. */
    int32_t last_frame_acked[PC_SLIPPI_REMOTE_PLAYER_MAX];
    struct {
        int32_t frame;
        uint64_t time_us;
    } ack_timers[PC_SLIPPI_REMOTE_PLAYER_MAX][PC_SLIPPI_ACK_TIMERS_MAX];
    int ack_timer_n[PC_SLIPPI_REMOTE_PLAYER_MAX];
    uint32_t ping_us[PC_SLIPPI_REMOTE_PLAYER_MAX];

    /* Local pads waiting to be sent, newest at `local_head`. Slippi keeps the
     * whole queue in one packet and trims the tail to the lowest ack. */
    PcSlippiPad local[PC_SLIPPI_PAD_QUEUE_MAX];
    int local_count;
    int local_head;

    /* The newest local pad's checksum, carried in every pad packet. */
    int32_t local_checksum_frame;
    uint32_t local_checksum;

    bool remote_active[PC_SLIPPI_REMOTE_PLAYER_MAX];
} PcSlippiSession;

/* Initialise from a config. Zeroes every queue and per-remote state. */
void pc_slippi_session_init(PcSlippiSession* s, const PcSlippiSessionConfig* cfg);

/* ---- local side ------------------------------------------------------- */

/* Remember a newly sampled local pad for `frame` and frame it as a pad packet
 * to send. Returns true and fills `out`/`out_len` when there is something to
 * send (there always is after this call, unless the buffer is too small).
 *
 * `now_us` timestamps the send for the peer's offset sample and for the ack
 * timer. `player_port` is our on-wire port. The packet carries every local pad
 * not yet acked (newest first), capped at what the receiver accepts. */
bool pc_slippi_session_queue_local_pad(PcSlippiSession* s, int32_t frame,
                                       const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE], uint64_t now_us,
                                       void* out, size_t out_cap, size_t* out_len);

/* Feed an acknowledgement from a remote (NP_MSG_SLIPPI_PAD_ACK body): records
 * the newest frame it holds, drops ack timers at or below it, and samples RTT
 * from the matching timer. Returns the RTT sample in microseconds (0 if the
 * frame did not match a live timer). */
uint32_t pc_slippi_session_on_pad_ack(PcSlippiSession* s, int remote_slot, int32_t frame,
                                      uint64_t now_us);

/* The checksum to attach to outgoing pad packets. */
void pc_slippi_session_set_local_checksum(PcSlippiSession* s, int32_t frame, uint32_t value);

/* ---- remote side ------------------------------------------------------ */

/* Handle an NP_MSG_SLIPPI_PAD payload (the id already consumed): enqueue the
 * new frames from the named port, record its checksum, and build the ack to
 * send back if any new frame arrived. Returns true when an ack should be sent,
 * filling `ack_out`. An unknown/short/malformed packet returns false and
 * changes nothing.
 *
 * `head_frame` is taken from the session's own queue for that remote, exactly
 * as Slippi does, so the receiver never trusts the sender's idea of what it
 * holds.
 *
 * The clock-offset sample needs a clock, which this core does not have; the
 * caller records it with pc_slippi_session_note_offset after a successful
 * call, using the packet's newest frame and the `out_remote_slot` this
 * function reports (the slot the sender's port maps to). `out_remote_slot`
 * may be NULL. */
bool pc_slippi_session_on_pads(PcSlippiSession* s, PcSlippiReader* r, int* out_remote_slot,
                               void* ack_out, size_t ack_cap, size_t* ack_len);

/* Build the window of remote pads for `remote_slot`, newest-first, up to
 * `max_frames`. Mirrors GetSlippiRemotePad. */
void pc_slippi_session_remote_pad(const PcSlippiSession* s, int remote_slot, int max_frames,
                                  PcSlippiRemotePadOutput* out);

/* Drop remote pads older than `finalized_frame`, keeping at least one. Mirrors
 * DropOldRemoteInputs. */
void pc_slippi_session_drop_old(PcSlippiSession* s, int32_t finalized_frame);

/* Mark a remote's connection state for GetSlippiRemotePad's is_disconnected. */
void pc_slippi_session_set_remote_active(PcSlippiSession* s, int remote_slot, bool active);
/* On-wire port of a remote slot (Slippi's inverse of PlayerIdxFromPort). */
uint8_t pc_slippi_session_remote_port(const PcSlippiSession* s, int remote_slot);

/* Clear the receive queues and per-game state at the start of a match, as
 * StartSlippiGame does. Keeps the config and activity flags. */
void pc_slippi_session_start_game(PcSlippiSession* s, uint64_t now_us);

/* ---- time sync -------------------------------------------------------- */

/* The trimmed-mean clock offset in microseconds over the active remotes, or 0
 * with no samples. Positive means this side is ahead. Mirrors
 * CalcTimeOffsetUs: per remote, sort the samples, average the middle third,
 * then take the minimum across remotes. `inactive` slots are skipped. */
int32_t pc_slippi_session_time_offset_us(const PcSlippiSession* s);

/* Current RTT estimate for a remote, in microseconds (0 if unknown). */
uint32_t pc_slippi_session_ping_us(const PcSlippiSession* s, int remote_slot);

/* Record a clock-offset sample for a remote from a pad packet whose newest
 * frame is `remote_frame`, observed at local time `now_us`. The offset is
 * `(now - ping/2) - ourLastSendTime + FRAME_US * (ourLastFrame - remoteFrame)`,
 * exactly Slippi's formula; a positive value means this side is ahead. The
 * caller invokes this once per newly arrived pad packet, before
 * pc_slippi_session_queue_local_pad for the same tick updates last_sent. */
void pc_slippi_session_note_offset(PcSlippiSession* s, int remote_slot, int32_t remote_frame,
                                   uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
