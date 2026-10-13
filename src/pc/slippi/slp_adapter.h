/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi transport adapter: the seam between the Slippi ENet driver
 * (slp_net.h) and melee-pc's netplay engine (src/pc/net.c).
 *
 * Approach A (docs/slippi-migration-plan.md section 6): our rollback loop
 * already implements Slippi's cadence (verified against slippi-ssbm-asm), so
 * this adapter does NOT reimplement rollback. It is a byte transport in the
 * shape net.c's input path already consumes: newly-arrived contiguous remote
 * frames are delivered one at a time through a callback, exactly as net.c's
 * on_inputs() walks `s_remote_have+1 .. last`. net.c keeps its rings, barrier,
 * snapshot ring, pad-lock and audio machinery unchanged.
 *
 * Why a separate module: the delivery bookkeeping (which frame is next, when a
 * gap stops the walk, when the peer left) is exactly the part that is worth
 * testing without a running game. tools/test_slp_adapter.c drives two adapters
 * back to back over loopback and checks the delivered frame sequence.
 *
 * Scope: this file owns the *transport-facing* state only. It does not know
 * about WirePad's C layout (net.c does) or the game; the pad is passed as the
 * 8 wire bytes Slippi uses, which is WirePad's exact image (net_internal.h).
 *
 * Not a threadsafe object: the caller serialises pump() and send_local().
 * net.c's game thread owns sends; its receive thread (or the game thread) owns
 * pump(). The adapter keeps no lock, matching slp_net's single-threaded
 * contract; the caller takes one if it splits the two. */
#ifndef PC_SLIPPI_SLP_ADAPTER_H
#define PC_SLIPPI_SLP_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "pc/slippi/slp_net.h"
#include "pc/slippi/slp_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per newly-arrived contiguous remote frame, in ascending frame
 * order, oldest first. `remote_slot` is the session's remote index; `frame` is
 * the absolute game frame; `pad` is the 8 wire bytes. This is the adapter's
 * equivalent of one iteration of net.c's on_inputs loop, and lets net.c apply
 * the frame to its ring and set its rollback frame exactly as it does today. */
typedef void (*PcSlippiFrameFn)(void* user, int remote_slot, int32_t frame,
                                const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE]);

typedef struct PcSlippiAdapter {
    PcSlippiNet* net;
    PcSlippiFrameFn on_frame;
    void* user;
    /* Newest frame handed to the game per remote. Starts at 0 (nothing
     * delivered); a packet carrying frames 1..N walks 1..N and leaves this at
     * N. A gap (a frame missing from the session queue) stops the walk there,
     * and the next pump resumes from it. */
    int32_t delivered_upto[PC_SLIPPI_REMOTE_PLAYER_MAX];
    bool started; /* start_game clears delivery bookkeeping */
} PcSlippiAdapter;

/* Bind an adapter to a connected (or connecting) driver. `on_frame` may be NULL
 * if the caller only wants the send/query surface. */
void pc_slippi_adapter_init(PcSlippiAdapter* a, PcSlippiNet* net, PcSlippiFrameFn on_frame,
                            void* user);

/* Service ENet once (no wait) and deliver every newly-arrived contiguous remote
 * frame through on_frame. Returns true if any frame was delivered. `now_us`
 * stamps the clock-offset sample, as slp_net expects. */
bool pc_slippi_adapter_pump(PcSlippiAdapter* a, uint64_t now_us);

/* Send the local pad sampled for `frame` (the game's frame, before delay; the
 * session adds the delay convention). Returns false when not connected. */
bool pc_slippi_adapter_send_local(PcSlippiAdapter* a, int32_t frame, const void* wire_pad8,
                                  uint64_t now_us);

/* Set the finalized-frame and Slippi wire checksum carried on outgoing pad
 * packets. The adapter forwards to the session. */
void pc_slippi_adapter_set_checksum(PcSlippiAdapter* a, int32_t frame, uint32_t checksum);

/* Reset per-game delivery state and the session's receive queues, as a match
 * starting does. */
void pc_slippi_adapter_start_game(PcSlippiAdapter* a, uint64_t now_us);

/* ---- queries the engine needs ----------------------------------------- */

/* Newest contiguous remote frame delivered so far, across remotes (the value
 * net.c tracks as s_remote_have). -1 when nothing has been delivered. */
int32_t pc_slippi_adapter_remote_have(const PcSlippiAdapter* a);

/* Newest remote frame the session holds at all (contiguous or not), across
 * remotes. This is what net.c's stall logic measures against. 0 when none. */
int32_t pc_slippi_adapter_remote_newest(const PcSlippiAdapter* a);

/* Newest frame the remote has acked to us. */
int32_t pc_slippi_adapter_last_acked(const PcSlippiAdapter* a, int remote_slot);

/* Connection status (enum PcSlippiNetStatus) and the peer's disconnect reason. */
int pc_slippi_adapter_status(const PcSlippiAdapter* a);
int pc_slippi_adapter_disconnect_reason(const PcSlippiAdapter* a);

/* Trimmed-mean clock offset in us (positive = we are ahead). */
int32_t pc_slippi_adapter_time_offset_us(const PcSlippiAdapter* a);

/* True once a session is connected. */
bool pc_slippi_adapter_connected(const PcSlippiAdapter* a);

#ifdef __cplusplus
}
#endif

#endif
