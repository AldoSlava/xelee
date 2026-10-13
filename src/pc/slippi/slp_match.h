/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi matchmaking: the ticket protocol and the peer list it yields.
 *
 * Ported for native C use in melee-pc from project-slippi/Ishiiruka
 * Source/Core/Core/Slippi/SlippiMatchmaking.cpp and Hero88go/melee-unlocked
 * port/runtime/hle/slippi_net.cpp (the Matchmaking class). See
 * docs/slippi-migration-plan.md.
 *
 * The flow is ENet plus JSON to mm.slippi.gg:43113:
 *   1. create-ticket  ->  create-ticket-resp (accepted, or an error string);
 *   2. poll            ->  get-ticket-resp (a match: players, ports, addresses,
 *                          decider, stages), or nothing while still searching.
 * The reply's player list is the whole risk surface: every count and index
 * taken from it indexes a fixed per-player array later, so it is parsed whole
 * and refused whole (Slippi's own parser, and melee-unlocked's ParsedTicket).
 *
 * This module is split so the parser is testable with no socket:
 *   - pc_slippi_ticket_parse() is pure: JSON text -> PcSlippiTicket;
 *   - PcSlippiTicket is exactly what the netplay client needs (addresses,
 *     ports, remote count, local port and index, decider);
 *   - a local-peer mode synthesizes the same ticket on 127.0.0.1, so Direct
 *     can be tested between two of our instances with no server (matching
 *     melee-unlocked's local_peer test path).
 *
 * Ranked is out of scope (docs/slippi-migration-plan.md section 7). The mode
 * is carried so Unranked and Direct are both expressible. */
#ifndef PC_SLIPPI_SLP_MATCH_H
#define PC_SLIPPI_SLP_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pc/slippi/slp_proto.h"
#include "pc/slippi/slp_user.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On-wire ports are 1..4, so at most four players (one local + three remote). */
#define PC_SLIPPI_MATCH_PLAYERS_MAX PC_SLIPPI_PLAYER_COUNT_MAX
#define PC_SLIPPI_MATCH_REMOTES_MAX PC_SLIPPI_REMOTE_PLAYER_MAX
#define PC_SLIPPI_MATCH_STAGES_MAX 8
#define PC_SLIPPI_ADDR_MAX 64

/* One player as the ticket describes it. */
typedef struct PcSlippiMatchPlayer {
    uint8_t port; /* 1..4 */
    bool is_local;
    bool is_bot;
    char uid[PC_SLIPPI_UID_MAX];
    char display_name[PC_SLIPPI_DISPLAY_NAME_MAX];
    char connect_code[PC_SLIPPI_CONNECT_CODE_MAX];
    /* The address this player can be reached at, "ip:port". Already resolved
     * against the local external IP (LAN when on the same one). */
    char address[PC_SLIPPI_ADDR_MAX];
} PcSlippiMatchPlayer;

/* A parsed get-ticket-resp. `ok` is false (with `error` set) for anything the
 * netplay client could not safely use: fewer than two or more than four
 * players, a port outside 1..4 or used twice, or no local player. */
typedef struct PcSlippiTicket {
    bool ok;
    char error[64];
    char match_id[64];
    PcSlippiMatchPlayer players[PC_SLIPPI_MATCH_PLAYERS_MAX];
    int player_count;
    /* The local player's index into players[] (its port - 1). */
    int local_player_index;
    /* Remote addresses in players[] order, local excluded (the netplay client's
     * per-remote input). */
    char remote_addresses[PC_SLIPPI_MATCH_REMOTES_MAX][PC_SLIPPI_ADDR_MAX];
    int remote_count;
    bool is_host; /* the decider */
    uint16_t stages[PC_SLIPPI_MATCH_STAGES_MAX];
    int stage_count;
    uint32_t items;
} PcSlippiTicket;

/* Parse a get-ticket-resp body. Pure: no sockets, no state, never throws; a
 * malformed body yields ok == false with `error` saying why. `local_port` is
 * the UDP port this instance is listening on, used to fill each player's
 * address when the ticket gives only an IP (it never does, so this is a
 * fallback) and for the local player's own entry. */
bool pc_slippi_ticket_parse(const char* text, size_t len, uint16_t local_port,
                            PcSlippiTicket* out);

/* Build a ticket for local test peering (no matchmaking server): `n` players
 * on 127.0.0.1, port 1 is the decider, everyone else a remote at
 * base_port + index. Runs through the same parser the server's reply does, so
 * a local Direct test exercises the real path. Returns false on bad arguments
 * (n outside 2..4). */
bool pc_slippi_ticket_local(uint16_t local_port, uint16_t base_remote_port, int n,
                            int local_index, PcSlippiTicket* out);

/* The default allowed stage list when a ticket names none (Slippi's own):
 * Pokemon, Yoshi's Story, Dream Land, Battlefield, Final Destination, plus
 * Fountain of Dreams in singles. */
void pc_slippi_ticket_default_stages(int player_count, uint16_t out[PC_SLIPPI_MATCH_STAGES_MAX],
                                     int* count);

/* ---- matchmaking client (ENet + JSON) -------------------------------- */

typedef enum PcSlippiMatchState {
    PC_SLIPPI_MATCH_IDLE = 0,
    PC_SLIPPI_MATCH_INITIALIZING,
    PC_SLIPPI_MATCH_SEARCHING,
    PC_SLIPPI_MATCH_CONNECTING,
    PC_SLIPPI_MATCH_SUCCESS,
    PC_SLIPPI_MATCH_ERROR,
} PcSlippiMatchState;

typedef struct PcSlippiMatch PcSlippiMatch;

/* Begin a search. `mode` is PC_SLIPPI_MODE_UNRANKED or _DIRECT (Ranked is
 * refused). `connect_code` is the Direct target ("" for Unranked). The search
 * runs on its own thread and is polled with pc_slippi_match_state(). */
PcSlippiMatch* pc_slippi_match_start(const PcSlippiAccount* account, int mode,
                                     const char* connect_code);
void pc_slippi_match_stop(PcSlippiMatch* m);
PcSlippiMatchState pc_slippi_match_state(const PcSlippiMatch* m);
/* The error text for PC_SLIPPI_MATCH_ERROR, else "". */
const char* pc_slippi_match_error(const PcSlippiMatch* m);
/* On success, the parsed ticket (addresses, ports, decider, local index). */
bool pc_slippi_match_result(const PcSlippiMatch* m, PcSlippiTicket* out);

#ifdef __cplusplus
}
#endif

#endif
