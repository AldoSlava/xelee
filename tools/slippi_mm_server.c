/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A local stand-in for Slippi's matchmaking server (mm.slippi.gg:43113), for
 * testing netplay without Slippi's backend: our own builds and a Slippi
 * Dolphin can queue against it and be paired.
 *
 * The protocol is Ishiiruka's SlippiMatchmaking.cpp, from the server's side:
 * an ENet host, one reliable JSON packet per message.
 *   client -> "create-ticket" {user{uid,playKey,connectCode,displayName},
 *             search{mode, connectCode:[bytes]}, appVersion, ipAddressLan}
 *   server -> "create-ticket-resp" (or {"error": "..."})
 *   server -> "get-ticket-resp" once paired: matchId, isHost (the decider),
 *             players[] (uid, displayName, connectCode, port 1..4,
 *             isLocalPlayer, ipAddress "ip:port", ipAddressLan), stages[] (-s).
 * Modes: 1 Unranked pairs any two tickets; 2 Direct pairs two tickets that
 * name each other's connect codes. Ranked, Teams and Party are refused.
 * Players' game ports are the ones the clients advertise in ipAddressLan
 * (the port their ENet netplay host is bound to).
 *
 * To point a Slippi Dolphin here, which has the host name compiled in, map
 * mm.slippi.gg to this machine in /etc/hosts (127.0.0.1 for the same machine).
 * Our own builds take MELEE_SLIPPI_MM_HOST / MELEE_SLIPPI_MM_PORT.
 *
 * -R base routes every match through tools/udp_delay_relay.py (latency, jitter
 * and loss, so rollback runs on one machine): each player is told the other
 * lives at 127.0.0.1:base (the decider) or base+1, and the relay hears
 * "MAP <decider game port> <other game port>" on base+2.
 *
 *   slippi_mm_server [-p port] [-v] [-s stage,stage,...] [-R base]
 */
#include "pc/slippi/slp_json.h"

#include <enet/enet.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_TICKETS 64
#define CODE_MAX 24

typedef struct Ticket {
    bool used;
    ENetPeer* peer;
    int mode;
    char uid[40], display[40], code[CODE_MAX], want[CODE_MAX];
    char ip[48];
    unsigned game_port; /* from ipAddressLan: where the client's game host listens */
    char lan[80];
    time_t queued;
} Ticket;

static Ticket tickets[MAX_TICKETS];
static volatile sig_atomic_t stop_flag;
static bool verbose;
static unsigned match_serial;
static unsigned relay_base; /* -R: 0 = players reach each other directly */

static void on_signal(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...) {
    char ts[16];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* This machine's address on the LAN: the source of its route outward (a UDP
 * connect sends nothing). Left alone if there is no route. */
static void self_lan_ip(char* out, size_t cap) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return;
    }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(53)};
    struct sockaddr_in me;
    socklen_t len = sizeof me;
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    if (connect(s, (struct sockaddr*)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr*)&me, &len) == 0) {
        inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)cap);
    }
    close(s);
}

static void send_json(ENetPeer* peer, const char* body) {
    ENetPacket* p = enet_packet_create(body, strlen(body), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, p);
    if (verbose) {
        logf_("-> %s", body);
    }
}

static void json_escape(char* out, size_t cap, const char* in) {
    size_t w = 0;
    for (; *in && w + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c < 0x20) {
            w += (size_t)snprintf(out + w, cap - w, "\\u%04x", c);
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

/* The target code arrives as game bytes: ASCII, or Shift-JIS full-width for
 * the '#' and letters/digits the game's own keyboard produces. */
static void decode_code(const PcSlippiJson* arr, char* out, size_t cap) {
    size_t w = 0;
    int n = pc_slippi_json_is_array(arr) ? arr->child_count : 0;
    for (int i = 0; i < n && w + 1 < cap; i++) {
        int b = pc_slippi_json_int(pc_slippi_json_at(arr, i), 0);
        if (b == 0) {
            break;
        }
        if (b == 0x81 && i + 1 < n && pc_slippi_json_int(pc_slippi_json_at(arr, i + 1), 0) == 0x94) {
            out[w++] = '#';
            i++;
        } else if (b == 0x82 && i + 1 < n) {
            int c = pc_slippi_json_int(pc_slippi_json_at(arr, ++i), 0);
            if (c >= 0x4F && c <= 0x58) {
                out[w++] = (char)('0' + c - 0x4F);
            } else if (c >= 0x60 && c <= 0x79) {
                out[w++] = (char)('A' + c - 0x60);
            } else if (c >= 0x81 && c <= 0x9A) {
                out[w++] = (char)('A' + c - 0x81);
            }
        } else if (b > 0 && b < 0x80) {
            out[w++] = (char)toupper(b);
        }
    }
    out[w] = '\0';
}

static bool code_eq(const char* a, const char* b) {
    return strcasecmp(a, b) == 0;
}

static void drop_ticket(Ticket* t) {
    t->used = false;
    t->peer = NULL;
}

static void player_json(char* out, size_t cap, const Ticket* t, int port, bool local,
                        unsigned relay_port) {
    char uid[96], disp[96], code[64];
    json_escape(uid, sizeof uid, t->uid);
    json_escape(disp, sizeof disp, t->display);
    json_escape(code, sizeof code, t->code);
    snprintf(out, cap,
             "{\"uid\":\"%s\",\"displayName\":\"%s\",\"connectCode\":\"%s\",\"port\":%d,"
             "\"isLocalPlayer\":%s,\"ipAddress\":\"%s:%u\",\"ipAddressLan\":\"%s\"}",
             uid, disp, code, port, local ? "true" : "false", t->ip, t->game_port, t->lan);
    if (relay_port != 0) {
        /* The other player reaches this one through the relay, whichever
         * address it picks. */
        snprintf(out, cap,
                 "{\"uid\":\"%s\",\"displayName\":\"%s\",\"connectCode\":\"%s\",\"port\":%d,"
                 "\"isLocalPlayer\":false,\"ipAddress\":\"127.0.0.1:%u\","
                 "\"ipAddressLan\":\"127.0.0.1:%u\"}",
                 uid, disp, code, port, relay_port, relay_port);
    }
}

/* Tell the relay which game ports this match's two players listen on. */
static void relay_map(const Ticket* a, const Ticket* b) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)(relay_base + 2));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char msg[48];
    int n = snprintf(msg, sizeof msg, "MAP %u %u", a->game_port, b->game_port);
    sendto(fd, msg, (size_t)n, 0, (struct sockaddr*)&to, sizeof to);
    close(fd);
    logf_("relay: %s via %u, %s via %u", a->code, relay_base, b->code, relay_base + 1);
}

/* The legal stages each ticket names (-s): Slippi's singles six by default;
 * one stage forces it, for testing a stage at a time. */
static char stage_list[128] = "3,8,28,31,32,2";

static void pair(Ticket* a, Ticket* b) {
    /* a queued first and is the decider. Both see the same player order. */
    if (relay_base != 0) {
        relay_map(a, b);
    }
    for (int side = 0; side < 2; side++) {
        Ticket* me = side ? b : a;
        char pa[600], pb[600], body[2048];
        player_json(pa, sizeof pa, a, 1, me == a, relay_base != 0 && me != a ? relay_base : 0);
        player_json(pb, sizeof pb, b, 2, me == b, relay_base != 0 && me != b ? relay_base + 1 : 0);
        snprintf(body, sizeof body,
                 "{\"type\":\"get-ticket-resp\",\"matchId\":\"mode.%s.local-%u\","
                 "\"isHost\":%s,\"players\":[%s,%s],\"stages\":[%s],\"items\":0}",
                 a->mode == 1 ? "unranked" : "direct", match_serial, me == a ? "true" : "false",
                 pa, pb, stage_list);
        send_json(me->peer, body);
    }
    logf_("matched %s (%s) with %s (%s), match %u, %s decides", a->code, a->ip, b->code, b->ip,
          match_serial, a->code);
    match_serial++;
    drop_ticket(a);
    drop_ticket(b);
}

static void try_match(Ticket* t) {
    Ticket* best = NULL;
    for (int i = 0; i < MAX_TICKETS; i++) {
        Ticket* o = &tickets[i];
        if (!o->used || o == t || o->mode != t->mode || o->peer == t->peer) {
            continue;
        }
        if (t->mode == 2 && !(code_eq(o->code, t->want) && code_eq(o->want, t->code))) {
            continue;
        }
        if (best == NULL || o->queued < best->queued) {
            best = o;
        }
    }
    if (best != NULL) {
        /* The earlier ticket is the decider. */
        if (best->queued <= t->queued) {
            pair(best, t);
        } else {
            pair(t, best);
        }
    }
}

static void handle_message(ENetPeer* peer, const char* text, size_t len) {
    PcSlippiJson* root = NULL;
    char err[64];
    if (!pc_slippi_json_parse(text, len, &root, err, sizeof err)) {
        logf_("bad json from %x: %s", peer->address.host, err);
        return;
    }
    char type[40];
    pc_slippi_json_string(pc_slippi_json_get(root, "type"), type, sizeof type);
    if (strcmp(type, "create-ticket") != 0) {
        logf_("ignoring message type '%s'", type);
        pc_slippi_json_free(root);
        return;
    }
    const PcSlippiJson* user = pc_slippi_json_get(root, "user");
    const PcSlippiJson* search = pc_slippi_json_get(root, "search");
    Ticket t;
    memset(&t, 0, sizeof t);
    t.peer = peer;
    t.queued = time(NULL);
    pc_slippi_json_string(pc_slippi_json_get(user, "uid"), t.uid, sizeof t.uid);
    pc_slippi_json_string(pc_slippi_json_get(user, "connectCode"), t.code, sizeof t.code);
    pc_slippi_json_string(pc_slippi_json_get(user, "displayName"), t.display, sizeof t.display);
    t.mode = pc_slippi_json_int(pc_slippi_json_get(search, "mode"), -1);
    decode_code(pc_slippi_json_get(search, "connectCode"), t.want, sizeof t.want);
    pc_slippi_json_string(pc_slippi_json_get(root, "ipAddressLan"), t.lan, sizeof t.lan);
    enet_address_get_host_ip(&peer->address, t.ip, sizeof t.ip);
    const char* colon = strrchr(t.lan, ':');
    /* A client on this machine reaches us over loopback (mm.slippi.gg mapped
     * to 127.0.0.1), and a Slippi Dolphin then advertises 127.0.0.1 as its LAN
     * address too, which a player elsewhere on the LAN (a headset) cannot dial
     * back: give out this machine's LAN address for both. A peer on the same
     * /16 takes ipAddressLan, so ipAddress alone is not enough. */
    if (strncmp(t.ip, "127.", 4) == 0 && relay_base == 0) {
        self_lan_ip(t.ip, sizeof t.ip);
        if (strncmp(t.lan, "127.", 4) == 0 && strncmp(t.ip, "127.", 4) != 0 && colon != NULL) {
            char port[16];
            snprintf(port, sizeof port, "%s", colon);
            snprintf(t.lan, sizeof t.lan, "%s%s", t.ip, port);
            colon = strrchr(t.lan, ':');
        }
    }
    t.game_port = colon != NULL ? (unsigned)atoi(colon + 1) : 0;
    char app[32];
    pc_slippi_json_string(pc_slippi_json_get(root, "appVersion"), app, sizeof app);
    pc_slippi_json_free(root);

    if (t.mode != 1 && t.mode != 2) {
        logf_("%s asked for unsupported mode %d", t.code, t.mode);
        send_json(peer, "{\"type\":\"create-ticket-resp\",\"error\":\"This mode is not supported by the local server\"}");
        return;
    }
    if (t.code[0] == '\0' || t.game_port == 0) {
        logf_("refusing ticket without a connect code or game port (lan '%s')", t.lan);
        send_json(peer, "{\"type\":\"create-ticket-resp\",\"error\":\"Missing user or address\"}");
        return;
    }
    if (t.mode == 2 && t.want[0] == '\0') {
        send_json(peer, "{\"type\":\"create-ticket-resp\",\"error\":\"No connect code given\"}");
        return;
    }
    /* One ticket per connection: a new one replaces the old. */
    Ticket* slot = NULL;
    for (int i = 0; i < MAX_TICKETS; i++) {
        if (tickets[i].used && tickets[i].peer == peer) {
            slot = &tickets[i];
            break;
        }
    }
    for (int i = 0; slot == NULL && i < MAX_TICKETS; i++) {
        if (!tickets[i].used) {
            slot = &tickets[i];
        }
    }
    if (slot == NULL) {
        send_json(peer, "{\"type\":\"create-ticket-resp\",\"error\":\"Queue full\"}");
        return;
    }
    t.used = true;
    *slot = t;
    logf_("ticket: %s (%s) mode %d%s%s, game port %u, app %s", t.code, t.display, t.mode,
          t.mode == 2 ? " wants " : "", t.mode == 2 ? t.want : "", t.game_port, app);
    send_json(peer, "{\"type\":\"create-ticket-resp\"}");
    try_match(slot);
}

int main(int argc, char** argv) {
    unsigned port = 43113;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc &&
                   strspn(argv[i + 1], "0123456789,") == strlen(argv[i + 1]) &&
                   strlen(argv[i + 1]) < sizeof stage_list) {
            snprintf(stage_list, sizeof stage_list, "%s", argv[++i]);
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            relay_base = (unsigned)atoi(argv[++i]);
        } else {
            fprintf(stderr, "usage: %s [-p port] [-v] [-s stage,stage,...] [-R relay_base]\n",
                    argv[0]);
            return 2;
        }
    }
    if (enet_initialize() != 0) {
        fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = (enet_uint16)port;
    ENetHost* host = enet_host_create(&addr, 64, 3, 0, 0);
    if (host == NULL) {
        fprintf(stderr, "cannot listen on UDP %u (already running?)\n", port);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    logf_("local Slippi matchmaking listening on UDP %u", port);
    while (!stop_flag) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, 100) > 0) {
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (verbose) {
                    logf_("client connected");
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                if (verbose) {
                    logf_("<- %.*s", (int)ev.packet->dataLength, (char*)ev.packet->data);
                }
                handle_message(ev.peer, (const char*)ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                for (int i = 0; i < MAX_TICKETS; i++) {
                    if (tickets[i].used && tickets[i].peer == ev.peer) {
                        logf_("%s left the queue", tickets[i].code);
                        drop_ticket(&tickets[i]);
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
