/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Slippi replay playback; see slp.h. */
#include "compat.h"
#include "pc/pc.h"
#include "pc/slp.h"
#include "pc/slp_format.h"
#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#include <melee/ft/fighter.h>
#include <melee/ft/types.h>
#include <melee/mn/types.h>

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool pc_exit_requested;

/* Frames past the replay's last that the game runs before it leaves, so
 * the replay it records ends as the original did. */
#define TAIL_FRAMES 30

typedef struct PlayFrame {
    PADStatus pad[4];
    float c[4][2];  /* the C-stick as the game normalized it */
    uint32_t cdir[4]; /* and the direction bits it set (HSD_PadADConvert) */
    float lr[4][2]; /* the triggers likewise (nml_analogL, nml_analogR) */
    bool have[4];
} PlayFrame;

static struct {
    int state; /* 0 not looked at, 1 playing, -1 off */
    uint8_t info[SLP_INFO_BLOCK_SIZE];
    uint32_t seed; /* the game start's: Slippi's rngOffset (SyncRNG) */
    bool frozen_ps; /* the game start's frozen Pokemon Stadium */
    bool raw_c;    /* the replay has the raw C-stick (3.17 on) */
    int32_t last;  /* the replay's last frame */
    int32_t count; /* frames held, from SLP_FIRST_FRAME */
    PlayFrame* frames;
    bool present[4]; /* the port has a fighter somewhere in the replay */
    bool started;  /* the VS scene has set up */
    int32_t next;  /* the fight frame the next tick is */
    bool converted;
    unsigned c_misses, lr_misses;
} P;

/* Event fields, as offsets from the command byte (SPEC.md). */
#define START_SEED 0x13D
#define START_FROZEN_PS 0x1A2
#define PRE_FRAME 0x1
#define PRE_PORT 0x5
#define PRE_FOLLOWER 0x6
#define PRE_C_X 0x21
#define PRE_C_Y 0x25
#define PRE_BUTTONS 0x2D
#define C_DIRS 0x00F00000u /* the C-stick's direction bits in the processed buttons */
#define PRE_PHYS_BUTTONS 0x31
#define PRE_PHYS_L 0x33
#define PRE_PHYS_R 0x37
#define PRE_RAW_X 0x3B
#define PRE_RAW_Y 0x40
#define PRE_RAW_CX 0x41
#define PRE_RAW_CY 0x42

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static float bef(const uint8_t* p) {
    const uint32_t u = be32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* The C-stick and triggers come back as the game normalized them, so the
 * raw values that normalize so are found by running the pad library's own
 * clamp, direction bits and scale over every raw value
 * (HSD_PadPcNormalize), once it has its settings. Several raws normalize
 * alike (inside the deadzone, past the rim): of those that also set the
 * direction bits the replay's buttons show, one stands for them all. Any
 * gives the game the same normalized stick, but not the same raw one, and
 * UCF reads the raw: past the rim with the other axis near the centre it
 * snaps the stick to exactly 1.0 (ftucf.c), which automatic smash DI then
 * moves the fighter by. A stick held off centre is almost always at the
 * rim, and one held along an axis in its notch: for those a raw UCF
 * snaps stands for them if there is one, then the largest; at rest, (0, 0),
 * the smallest. (Replays from Slippi 3.17 on carry the raw C-stick
 * itself.) A
 * table keyed by the two floats' bits and the direction bits. */
#define C_SLOTS (1u << 17)
static struct {
    uint64_t key; /* x bits << 32 | y bits, plus one: 0 is empty */
    uint32_t dir;
    int8_t x, y;
} s_c[C_SLOTS];
static float s_lr[256]; /* nml_analogL for each raw trigger value */

static uint64_t c_key(float x, float y) {
    uint32_t a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    return (((uint64_t)a << 32) | b) + 1;
}

static uint32_t c_hash(uint64_t k) {
    return (uint32_t)(k ^ (k >> 29) ^ (k >> 47)) * 2654435761u;
}

static void build_tables(void) {
    memset(s_c, 0, sizeof s_c);
    for (int x = -128; x < 128; x++) {
        for (int y = -128; y < 128; y++) {
            HSD_PadStatus mp;
            memset(&mp, 0, sizeof mp);
            mp.subStickX = (s8)x;
            mp.subStickY = (s8)y;
            HSD_PadPcNormalize(&mp);
            /* The replay holds the fighter's C-stick, which Fighter_procInput
             * has put through its deadzones after the pad library set the
             * direction bits: a raw (-28, 20) reads (-0.35, 0) with the
             * stick's left bit set. */
            float nx = mp.nml_subStickX, ny = mp.nml_subStickY;
            if (fabsf(nx) <= p_ftCommonData->horizontal_stick_deadzone) {
                nx = 0.0f;
            }
            if (fabsf(ny) <= p_ftCommonData->vertical_stick_deadzone) {
                ny = 0.0f;
            }
            const uint64_t k = c_key(nx, ny);
            const uint32_t dir = mp.button & C_DIRS;
            for (uint32_t h = c_hash(k ^ dir);; h++) {
                const uint32_t i = h & (C_SLOTS - 1);
                if (s_c[i].key == 0 || (s_c[i].key == k && s_c[i].dir == dir)) {
                    /* UCF's cardinal snap (ftucf.c snapCardinal): at the rim on
                     * one axis, within 6 of centre on the other */
#define SNAPS(px, py) ((abs(px) >= 80 && abs(py) <= 6) || (abs(py) >= 80 && abs(px) <= 6))
                    const int m = abs(x) + abs(y), held = abs(s_c[i].x) + abs(s_c[i].y);
                    const bool snaps = SNAPS(x, y), held_snaps = SNAPS(s_c[i].x, s_c[i].y);
                    const bool rest = nx == 0.0f && ny == 0.0f;
#undef SNAPS
                    if (s_c[i].key == 0 || (rest ? m < held : snaps != held_snaps ? snaps : m > held)) {
                        s_c[i].x = (int8_t)x;
                        s_c[i].y = (int8_t)y;
                    }
                    s_c[i].key = k;
                    s_c[i].dir = dir;
                    break;
                }
            }
        }
    }
    for (int v = 0; v < 256; v++) {
        HSD_PadStatus mp;
        memset(&mp, 0, sizeof mp);
        mp.analogL = (u8)v;
        HSD_PadPcNormalize(&mp);
        s_lr[v] = mp.nml_analogL;
    }
}

static bool c_raw(float x, float y, uint32_t dir, s8* rx, s8* ry) {
    const uint64_t k = c_key(x, y);
    for (uint32_t h = c_hash(k ^ dir);; h++) {
        const uint32_t i = h & (C_SLOTS - 1);
        if (s_c[i].key == 0) {
            return false;
        }
        if (s_c[i].key == k && s_c[i].dir == dir) {
            *rx = s_c[i].x;
            *ry = s_c[i].y;
            return true;
        }
    }
}

static bool lr_raw(float v, u8* raw) {
    for (int i = 0; i < 256; i++) {
        if (s_lr[i] == v) {
            *raw = (u8)i;
            return true;
        }
    }
    return false;
}

static bool load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        pc_log_line("slp play: cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = size > 15 ? malloc((size_t)size) : NULL;
    const bool read = data != NULL && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read || memcmp(data, "{U\x03raw[$U#l", 11) != 0) {
        pc_log_line("slp play: %s is not a Slippi replay", path);
        free(data);
        return false;
    }
    const uint32_t raw_len = be32(data + 11);
    const uint8_t* e = data + 15;
    const uint8_t* end = e + (raw_len <= (uint32_t)(size - 15) ? raw_len : (uint32_t)(size - 15));
    uint16_t sizes[256] = {0};
    bool have_start = false;
    int32_t cap = 0;
    while (e < end) {
        const uint8_t cmd = e[0];
        if (cmd == SLP_CMD_EVENT_PAYLOADS) {
            const uint8_t n = e[1];
            for (int k = 2; k + 2 <= n && e + k + 2 < end; k += 3) {
                sizes[e[k]] = (uint16_t)((e[k + 1] << 8) | e[k + 2]);
            }
            e += 1 + n;
            continue;
        }
        if (sizes[cmd] == 0 || e + 1 + sizes[cmd] > end) {
            break; /* an event without a size, or cut short: the replay ends here */
        }
        const size_t len = 1u + sizes[cmd];
        if (cmd == SLP_CMD_GAME_START && len >= START_SEED + 4) {
            memcpy(P.info, e + 5, SLP_INFO_BLOCK_SIZE);
            P.seed = be32(e + START_SEED);
            P.frozen_ps = len > START_FROZEN_PS && e[START_FROZEN_PS] != 0;
            have_start = true;
            pc_log_line("slp play: %s, Slippi %u.%u.%u, stage %u, rngOffset %08x", path, e[1], e[2], e[3],
                (unsigned)((e[5 + 0xE] << 8) | e[5 + 0xF]), P.seed);
        } else if (cmd == SLP_CMD_PRE_FRAME && len > PRE_RAW_X && !e[PRE_FOLLOWER] && e[PRE_PORT] < 4) {
            const int32_t idx = (int32_t)be32(e + PRE_FRAME) - SLP_FIRST_FRAME;
            if (idx >= 0 && idx < 1000000) {
                if (idx >= cap) {
                    const int32_t ncap = idx + 4096;
                    PlayFrame* grown = realloc(P.frames, sizeof *grown * (size_t)ncap);
                    if (grown == NULL) {
                        break;
                    }
                    memset(grown + cap, 0, sizeof *grown * (size_t)(ncap - cap));
                    P.frames = grown;
                    cap = ncap;
                }
                PlayFrame* fr = &P.frames[idx];
                const int port = e[PRE_PORT];
                PADStatus* pad = &fr->pad[port];
                /* The physical pad: its button word, the raw main stick
                 * (UCF reads it), the raw C-stick when there is one. */
                pad->button = (u16)((e[PRE_PHYS_BUTTONS] << 8) | e[PRE_PHYS_BUTTONS + 1]);
                pad->stickX = (s8)e[PRE_RAW_X];
                pad->stickY = len > PRE_RAW_Y ? (s8)e[PRE_RAW_Y] : 0;
                if (len > PRE_RAW_CY) {
                    P.raw_c = true;
                    pad->substickX = (s8)e[PRE_RAW_CX];
                    pad->substickY = (s8)e[PRE_RAW_CY];
                }
                fr->c[port][0] = bef(e + PRE_C_X);
                fr->c[port][1] = bef(e + PRE_C_Y);
                fr->cdir[port] = be32(e + PRE_BUTTONS) & C_DIRS;
                fr->lr[port][0] = bef(e + PRE_PHYS_L);
                fr->lr[port][1] = bef(e + PRE_PHYS_R);
                fr->have[port] = true;
                P.present[port] = true;
                if (idx + 1 > P.count) {
                    P.count = idx + 1;
                }
            }
        }
        e += len;
    }
    free(data);
    if (!have_start || P.count == 0) {
        pc_log_line("slp play: %s has no game start or no frames", path);
        return false;
    }
    P.last = SLP_FIRST_FRAME + P.count - 1;
    return true;
}

/* Once the pad library has its settings: every frame's normalized C-stick
 * and triggers back to raw values. */
static void convert(void) {
    const PadLibData* lib = &HSD_PadLibData;
    pc_log_line("slp play: pad library: clamp type %d shift %d min %d max %d, adc type %d threshold %d angle %.4f, "
                "stick scale %d, triggers clamp %d..%d shift %d scale %d",
        lib->clamp_stickType, lib->clamp_stickShift, lib->clamp_stickMin, lib->clamp_stickMax, lib->adc_type,
        lib->adc_th, lib->adc_angle, lib->scale_stick, lib->clamp_analogLRMin, lib->clamp_analogLRMax,
        lib->clamp_analogLRShift, lib->scale_analogLR);
    build_tables();
    for (int32_t i = 0; i < P.count; i++) {
        PlayFrame* fr = &P.frames[i];
        for (int port = 0; port < 4; port++) {
            if (!fr->have[port]) {
                continue;
            }
            PADStatus* pad = &fr->pad[port];
            if (!P.raw_c &&
                !c_raw(fr->c[port][0], fr->c[port][1], fr->cdir[port], &pad->substickX, &pad->substickY))
            {
                if (P.c_misses++ < 5) {
                    pc_log_line("slp play: frame %d port %d: no raw C-stick gives %.4f,%.4f with directions %06x",
                        SLP_FIRST_FRAME + i, port, fr->c[port][0], fr->c[port][1], fr->cdir[port]);
                }
            }
            if (!lr_raw(fr->lr[port][0], &pad->triggerLeft) || !lr_raw(fr->lr[port][1], &pad->triggerRight)) {
                if (P.lr_misses++ < 5) {
                    pc_log_line("slp play: frame %d port %d: no raw trigger gives L %.4f R %.4f",
                        SLP_FIRST_FRAME + i, port, fr->lr[port][0], fr->lr[port][1]);
                }
            }
        }
    }
    /* A frame a port's fighter sent nothing for (asleep, or out of the
     * match) keeps the pad it last had: the controller stays plugged in. */
    for (int32_t i = 1; i < P.count; i++) {
        for (int port = 0; port < 4; port++) {
            if (!P.frames[i].have[port] && P.present[port]) {
                P.frames[i].pad[port] = P.frames[i - 1].pad[port];
                P.frames[i].have[port] = P.frames[i - 1].have[port];
            }
        }
    }
    P.converted = true;
    pc_log_line("slp play: %d frames (%d to %d); C-stick %s, %u unmatched; %u unmatched triggers", P.count,
        SLP_FIRST_FRAME, P.last, P.raw_c ? "raw" : "from its normalized values", P.c_misses, P.lr_misses);
}

bool pc_slp_play_active(void) {
    if (P.state == 0) {
        const char* path = getenv("MELEE_SLP_PLAY");
        P.state = path != NULL && path[0] != '\0' && load(path) ? 1 : -1;
    }
    return P.state > 0;
}

uint32_t pc_slp_play_rng_offset(void) {
    return P.seed;
}

bool pc_slp_play_frozen_stadium(bool* frozen) {
    if (!pc_slp_play_active()) {
        return false;
    }
    *frozen = P.frozen_ps;
    return true;
}

#define BIT(v, n) (((v) >> (n)) & 1)

void pc_slp_play_setup(struct StartMeleeData* start) {
    if (!pc_slp_play_active()) {
        return;
    }
    SlpInfoBlock b;
    slp_info_parse(P.info, &b);
    StartMeleeRules* r = &start->rules;
    r->match_kind = b.match_kind;
    r->x0_3 = b.x0_3;
    r->timer_enabled = b.timer_enabled;
    r->timer_counts_up = b.timer_counts_up;
    r->x1_0 = BIT(b.bits1, 7), r->x1_1 = BIT(b.bits1, 6), r->x1_2 = BIT(b.bits1, 5), r->x1_3 = BIT(b.bits1, 4);
    r->x1_4 = BIT(b.bits1, 3), r->x1_5 = BIT(b.bits1, 2), r->timer_shows_hours = BIT(b.bits1, 1);
    r->friendly_fire = BIT(b.bits1, 0);
    r->is_stock = BIT(b.bits2, 7), r->x2_1 = BIT(b.bits2, 6), r->x2_2 = BIT(b.bits2, 5);
    r->single_button = BIT(b.bits2, 4), r->disable_pausing = BIT(b.bits2, 3), r->x2_5 = BIT(b.bits2, 2);
    r->x2_6 = BIT(b.bits2, 1), r->x2_7 = BIT(b.bits2, 0);
    r->x3_0 = BIT(b.bits3, 7), r->x3_1 = BIT(b.bits3, 6), r->x3_2 = BIT(b.bits3, 5), r->x3_3 = BIT(b.bits3, 4);
    r->x3_4 = BIT(b.bits3, 3), r->x3_5 = BIT(b.bits3, 2), r->x3_6 = BIT(b.bits3, 1), r->x3_7 = BIT(b.bits3, 0);
    r->x4_0 = BIT(b.bits4, 7), r->is_vs = BIT(b.bits4, 6), r->x4_2 = BIT(b.bits4, 5), r->x4_3 = BIT(b.bits4, 4);
    r->x4_4 = BIT(b.bits4, 3), r->x4_5 = BIT(b.bits4, 2), r->x4_6 = BIT(b.bits4, 1), r->x4_7 = BIT(b.bits4, 0);
    r->x5_0 = BIT(b.bits5, 7), r->x5_1 = BIT(b.bits5, 6), r->x5_2 = BIT(b.bits5, 5), r->x5_3 = BIT(b.bits5, 4);
    r->x5_4 = BIT(b.bits5, 3), r->x5_5 = BIT(b.bits5, 2), r->x5_6 = BIT(b.bits5, 1), r->x5_7 = BIT(b.bits5, 0);
    r->x6 = b.x6;
    r->x7 = b.x7;
    r->is_teams = b.is_teams;
    r->x9 = b.x9;
    r->xA = b.xA;
    r->item_freq = b.item_freq;
    r->sd_penalty = b.sd_penalty;
    r->xD = b.xD;
    r->stkind = b.stkind;
    r->time_limit = b.time_limit;
    r->x14 = b.x14;
    r->x18 = b.x18;
    r->x1C_pad[0] = b.x1C;
    r->x20 = b.item_mask;
    r->x28 = b.x28;
    r->x2C = b.x2C;
    r->x30 = b.damage_ratio;
    r->game_speed = b.game_speed;
    for (int i = 0; i < 6; i++) {
        const SlpInfoPlayer* o = &b.players[i];
        PlayerInitData* p = &start->players[i];
        p->ckind = o->ckind;
        p->slot_type = o->slot_type;
        p->stocks = o->stocks;
        p->color = o->color;
        p->slot = o->slot;
        p->spawn_pos = o->spawn_pos;
        p->spawn_dir = o->spawn_dir;
        p->sub_color = o->sub_color;
        p->handicap = o->handicap;
        p->team = o->team;
        p->nametag = o->nametag;
        p->xB = o->xB;
        p->rumble_enabled = BIT(o->flags_c, 7), p->xC_b1 = BIT(o->flags_c, 6), p->vs_metal = BIT(o->flags_c, 5);
        p->xC_b3 = BIT(o->flags_c, 4), p->vs_invisible = BIT(o->flags_c, 3), p->xC_b5 = BIT(o->flags_c, 2);
        p->xC_b6 = BIT(o->flags_c, 1), p->xC_b7 = BIT(o->flags_c, 0);
        p->xD_b0 = BIT(o->flags_d, 7), p->xD_b1 = BIT(o->flags_d, 6), p->xD_b2 = BIT(o->flags_d, 5);
        p->xD_b3 = BIT(o->flags_d, 4), p->xD_b4 = BIT(o->flags_d, 3), p->xD_b5 = BIT(o->flags_d, 2);
        p->xD_b6 = BIT(o->flags_d, 1), p->xD_b7 = BIT(o->flags_d, 0);
        p->cpu_kind = o->cpu_kind;
        p->cpu_level = o->cpu_level;
        p->damage = o->damage;
        p->damage1 = o->damage1;
        p->hp = o->hp;
        p->attack_ratio = o->attack_ratio;
        p->defense_ratio = o->defense_ratio;
        p->model_scale = o->model_scale;
    }
    /* A replay holds no paused frames (Slippi records fight frames only),
     * so the frame after a pause is the first one unpaused: played back
     * with pausing on, the Start that paused it would pause this game for
     * good, the next frame's pads never coming. */
    r->disable_pausing = true;
    pc_log_line("slp play: match set up from the replay (stage %u)", (unsigned)r->stkind);
}

void pc_slp_play_match_start(void) {
    if (pc_slp_play_active()) {
        P.started = true;
        P.next = SLP_FIRST_FRAME;
    }
}

void pc_slp_play_feed(PADStatus* head) {
    if (!pc_slp_play_active() || !P.started) {
        return;
    }
    if (!P.converted) {
        convert();
    }
    if (P.next > P.last + TAIL_FRAMES) {
        if (!pc_exit_requested) {
            pc_log_line("slp play: replay done at frame %d", P.next - 1);
            pc_exit_requested = true;
        }
        return;
    }
    /* The match's first frame starts from the agreed seed, as a netplay
     * session's does (net.c fresh_tick, at its start frame): the draws the
     * match setup made are behind it. In Slippi's terms, the rngOffset
     * InitOnlinePlay seeded. */
    if (P.next == SLP_FIRST_FRAME) {
        *HSD_RandSeedPtr = P.seed;
    }
    const int32_t idx = (P.next < P.last ? P.next : P.last) - SLP_FIRST_FRAME;
    const PlayFrame* fr = &P.frames[idx];
    for (int port = 0; port < 4; port++) {
        if (fr->have[port] || P.present[port]) {
            head[port] = fr->pad[port];
            head[port].err = PAD_ERR_NONE;
        } else {
            memset(&head[port], 0, sizeof head[port]);
            head[port].err = PAD_ERR_NO_CONTROLLER;
        }
    }
}

void pc_slp_play_match_end(void) {
    if (P.state > 0 && P.started && !pc_exit_requested) {
        pc_log_line("slp play: the match ended at frame %d (the replay's last is %d)", P.next - 1, P.last);
        pc_exit_requested = true;
    }
}

void pc_slp_play_tick_end(bool fight_frame) {
    if (P.state > 0 && P.started && fight_frame) {
        P.next++;
    }
}
