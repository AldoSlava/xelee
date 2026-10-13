/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi netplay ENet driver: the socket layer around the session core.
 *
 * Ported for native C use in melee-pc from project-slippi/Ishiiruka
 * SlippiNetplay.cpp (the ENet host/peer setup, the connect and disconnect
 * events, the send paths and the OnData dispatch) and Hero88go/melee-unlocked
 * port/runtime/hle/slippi_net.cpp. See docs/slippi-migration-plan.md.
 *
 * Design: this is itself split from the session core (slp_session.h) on
 * purpose. The core owns the bookkeeping; this file owns the host, the
 * service loop and the byte I/O. `pc_slippi_net_poll` is edge-driven and takes
 * the current time as an argument, so a harness can run two drivers in one
 * process over loopback and pump them deterministically, and the game adapter
 * can later either call poll from its frame loop or from a thread.
 *
 * Threading: this driver is single-threaded by contract. A caller that polls
 * from one thread and queues pads from another must serialise those calls; the
 * game adapter does (game thread owns sends, a service thread owns poll), and
 * takes its own lock. Keeping the lock out of here lets the harness stay
 * single-threaded and the tests deterministic.
 *
 * ENet must be initialised once per process: pc_slippi_net_global_init().
 * It is idempotent and safe to call from every init. */
#ifndef PC_SLIPPI_SLP_NET_H
#define PC_SLIPPI_SLP_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pc/slippi/slp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connection lifecycle, mirroring Slippi's SlippiConnectStatus. */
enum PcSlippiNetStatus {
    PC_SLIPPI_NET_UNSET = 0,
    PC_SLIPPI_NET_INITIATED,
    PC_SLIPPI_NET_CONNECTED,
    PC_SLIPPI_NET_FAILED,
    PC_SLIPPI_NET_DISCONNECTED,
};

/* Slippi's disconnect reason, sent as the ENet disconnect data field. */
enum PcSlippiDisconnectReason {
    PC_SLIPPI_DISCONNECT_UNSPECIFIED = 0,
    PC_SLIPPI_DISCONNECT_POOR_PERFORMANCE = 1,
};

typedef struct PcSlippiNetStats {
    uint32_t ping_us;       /* newest RTT sample, us */
    int connect_status;     /* enum PcSlippiNetStatus */
    int disconnect_reason;  /* 0 unless the peer sent one */
    bool connection_selected; /* NP_MSG_SLIPPI_CONN_SELECTED seen */
} PcSlippiNetStats;

/* Callbacks, all optional. `local_selections` is the merged local pick; a
 * remote's is delivered via pc_slippi_net_remote_selections. Called from
 * pc_slippi_net_poll, i.e. on the poller's thread. */
typedef struct PcSlippiNetCallbacks {
    void (*on_connected)(void* user);
    void (*on_disconnected)(void* user, int reason);
    void (*on_selections)(void* user, int remote_slot, const PcSlippiSelections* s);
    void (*on_chat)(void* user, int remote_slot, int message_id);
    void (*on_conn_selected)(void* user);
    void* user;
} PcSlippiNetCallbacks;

typedef struct PcSlippiNet PcSlippiNet;

/* Initialise the ENet library. Idempotent; returns false if it cannot. */
bool pc_slippi_net_global_init(void);
void pc_slippi_net_global_shutdown(void);

/* Create a driver for one remote. `local_udp_port` of 0 lets ENet pick one
 * (fine for tests and direct connects); a matchmaking hole punch needs the
 * same port the lobby used, so pass it. Returns NULL on failure. */
PcSlippiNet* pc_slippi_net_create(const PcSlippiSessionConfig* cfg, uint16_t local_udp_port,
                                  const PcSlippiNetCallbacks* cb);
void pc_slippi_net_destroy(PcSlippiNet* n);

/* Dial the remote. `ip` is a dotted quad or a host name ENet can resolve.
 * Returns false if the address cannot be resolved or the peer allocated. */
bool pc_slippi_net_connect(PcSlippiNet* n, const char* ip, uint16_t port);
/* Accept connections from any address (the other end dials us). */
void pc_slippi_net_listen(PcSlippiNet* n);

/* Service ENet once with no wait, dispatch every event, and run the time-sync
 * sample for any pad packets that arrived. Returns true if anything happened.
 * `now_us` is only used for the clock-offset sample. */
bool pc_slippi_net_poll(PcSlippiNet* n, uint64_t now_us);
/* Service with up to `timeout_ms` of wait; same dispatch. Returns true if any
 * event was handled. */
bool pc_slippi_net_service(PcSlippiNet* n, uint64_t now_us, uint32_t timeout_ms);

/* Queue a local pad for `frame` and send the resulting pad packet (unsequenced,
 * pad channel). `pad` is 8 wire bytes. Returns false if not connected or the
 * packet would not fit. */
bool pc_slippi_net_send_pad(PcSlippiNet* n, int32_t frame,
                            const uint8_t pad[PC_SLIPPI_PAD_DATA_SIZE], uint64_t now_us);

/* Send a selections message (reliable). Records it as the merged local pick. */
bool pc_slippi_net_send_selections(PcSlippiNet* n, const PcSlippiSelections* s);
/* Send a chat message (reliable). Refuses an id Slippi does not allow. */
bool pc_slippi_net_send_chat(PcSlippiNet* n, int message_id);
/* Send NP_MSG_SLIPPI_CONN_SELECTED (reliable): this is the connection we keep
 * when several came up during a hole punch. */
bool pc_slippi_net_send_conn_selected(PcSlippiNet* n);

/* Tell ENet to disconnect with `reason` in the disconnect data field. */
void pc_slippi_net_disconnect(PcSlippiNet* n, int reason);

/* Read state. */
const PcSlippiSession* pc_slippi_net_session(const PcSlippiNet* n);
PcSlippiSession* pc_slippi_net_session_mut(PcSlippiNet* n);
int pc_slippi_net_status(const PcSlippiNet* n);
void pc_slippi_net_stats(const PcSlippiNet* n, PcSlippiNetStats* out);
/* The merged remote selections for a slot (as received). */
bool pc_slippi_net_remote_selections(const PcSlippiNet* n, int remote_slot, PcSlippiSelections* out);
/* Set the local checksum carried in outgoing pad packets. */
void pc_slippi_net_set_local_checksum(PcSlippiNet* n, int32_t frame, uint32_t value);
/* Reset the per-game receive state (StartSlippiGame). */
void pc_slippi_net_start_game(PcSlippiNet* n, uint64_t now_us);
/* The offset estimate in us (positive = we are ahead). */
int32_t pc_slippi_net_time_offset_us(const PcSlippiNet* n);

#ifdef __cplusplus
}
#endif

#endif
