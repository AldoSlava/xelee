/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_MATCH_H
#define PC_NET_MATCH_H
#include "net_identity.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum PcNetMatchMode { PC_MATCH_DIRECT, PC_MATCH_UNRANKED, PC_MATCH_RANKED };
enum PcNetMatchState { PC_MATCH_SEARCH, PC_MATCH_CONNECT, PC_MATCH_READY, PC_MATCH_FAIL };
#define PC_NET_CONTACTS_MAX 16
typedef struct PcNetContact {
    char code[18];       /* NAME#SUFFIX as last seen */
    int64_t last_played; /* unix seconds */
} PcNetContact;
typedef struct PcNetMatchProgress {
    uint64_t elapsed_ms; /* since the search began; 0 once past it */
    bool network_ready;  /* the DHT has enough nodes to search */
    unsigned candidates; /* addresses tried so far */
    bool found;          /* a verified Hello arrived */
    bool calling;        /* direct, with a code: we are the caller */
    bool slippi;         /* served by Slippi matchmaking; network_ready = server answered */
} PcNetMatchProgress;
/* Opponents this profile has played, newest first; codes are only saved once
 * a match actually connected. */
int pc_net_match_contacts(PcNetContact* out, int max);
bool pc_net_match_clipboard_code(char out[18]);
bool pc_net_match_copy_code(void);
void pc_net_match_progress(PcNetMatchProgress* progress);
bool pc_net_match_publish_rank(void);
void pc_net_match_poll_publication(void);
/* 0 idle, 1 pending, 2 acknowledged, -1 failed (durable save preserved). */
int pc_net_match_publication(const char** reason);
bool pc_net_match_start(enum PcNetMatchMode mode, const char* target_code);
void pc_net_match_stop(void);
void pc_net_match_poll(void);
int pc_net_match_state(const char** why);
bool pc_net_match_is_host(void);
int32_t pc_net_match_start_frame(void);
uint32_t pc_net_match_seed(void);
const char* pc_net_match_local_code(void); /* the Slippi connect code during a Slippi search */
bool pc_net_match_is_slippi(void);
/* A Slippi account is signed in, so Unranked and Direct go through Slippi
 * matchmaking and Direct takes Slippi connect codes ("ABCD#123"). */
bool pc_net_match_slippi_ready(void);
/* The signed-in account's connect code, "" when there is none. */
const char* pc_net_match_slippi_code(void);
/* The external stage ids a Slippi Unranked ticket allows (0 when the match is
 * not an Unranked Slippi one: players pick). */
int pc_net_match_slippi_stages(uint16_t* out, int max);

/* A Slippi match once both sides' selections have crossed the link (state
 * PC_MATCH_READY): what the VS scene needs, by port (0 = P1, 1 = P2). */
typedef struct PcNetSlippiMatch {
    int local_player;      /* our port, 0 or 1 */
    uint8_t character[2];  /* external character ids */
    uint8_t color[2];
    uint16_t stage;        /* external stage id */
    uint8_t alt_stage_mode;
    uint32_t rng_offset;   /* the decider's */
    bool decider;
} PcNetSlippiMatch;
bool pc_net_match_slippi_match(PcNetSlippiMatch* out);
/* The local pick, made on the CSS before the search starts (Slippi's order). */
void pc_net_match_set_slippi_pick(uint8_t character, uint8_t color);
const char* pc_net_match_profile_error(void);
const char* pc_net_match_profile_directory(void);
enum PcNetMatchMode pc_net_match_mode(void);
const char* pc_net_match_opponent_code(void);
const PcNetIdentity* pc_net_match_identity(void);
const uint8_t* pc_net_match_peer_key(void);
#ifdef __cplusplus
}
#endif
#endif
