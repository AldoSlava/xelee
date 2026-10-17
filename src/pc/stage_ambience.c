/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stage ambience; see stage_ambience.h. */
#include "compat.h"
#include "pc/stage_ambience.h"

#include "pc/pc.h"
#include "pc/spatial_audio.h"
#include "pc/stage_mix.h"
#include "pc/xr_place.h"
#include <melee/gr/forward.h>
#include <melee/gr/grbigblue.h>
#include <melee/gr/ground.h>
#include <melee/gr/types.h>
#include <melee/gr/stage.h>
#include <melee/mp/mplib.h>
#include <melee/lb/lb_00B0.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/synth.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Without a drawn fight frame for this long, the ambience fades out. */
#define AMB_STALE_NS 250000000ull
#define AMB_FADE_S 1.0f    /* out, full scale */
#define AMB_FADE_IN_S 0.3f /* in, as the fight starts: quickly */
#define AMB_DUCK_S 0.15f /* the pause duck's glide, full scale */

enum { AMB_NONE, AMB_WIND, AMB_FALLS, AMB_FOUNTAIN, AMB_RUSH };

static uint64_t s_stamp; /* game thread writes, audio thread reads (__atomic) */
static int s_kind;
static float s_duck_target = 1.f; /* __atomic: written by pc_ambience_set_duck */
static float s_duck = 1.f;        /* audio thread: the duck, gliding to the target */
static float s_duck0 = 1.f;       /* audio thread: s_duck a frame ago */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool s_pref = true; /* the launcher's Audio setting */

void pc_ambience_set_enabled(bool on) {
    s_pref = on;
}

static bool enabled(void) {
    const char* v = getenv("MELEE_AMBIENCE");
    return v != NULL && v[0] != '\0' ? v[0] != '0' : s_pref;
}

void pc_ambience_set_duck(float gain) {
    __atomic_store(&s_duck_target, &gain, __ATOMIC_RELAXED);
}

float pc_ambience_duck(void) {
    float gain;
    __atomic_load(&s_duck_target, &gain, __ATOMIC_RELAXED);
    return gain;
}

/* Fountain of Dreams' side platforms ride jets of water (grizumi.c: part 4,
 * one gobj each): the root at the jet's foot in the stage's floor, its
 * child the column, scaled in height with the platform (scale.y 0.01 to
 * about 1), its grandchild under the platform. Sunk, the root is hidden.
 * Published by the game thread each drawn frame, read by the audio thread
 * (a sequence count around the copy). */
#define JETS 2
typedef struct Jets {
    float pos[JETS][3]; /* game units: on the column, a little up from its foot */
    float height[JETS]; /* the column's scale, 0 sunk; left jet first */
} Jets;
static Jets s_jets;
static unsigned s_jets_seq;

static void jets_frame(void) {
    Jets j = {0};
    for (HSD_GObj* g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_GROUND]; g != NULL; g = g->next) {
        const Ground* gp = g->user_data;
        HSD_JObj* root = g->hsd_obj;
        if (gp == NULL || root == NULL || gp->map_id != 4) {
            continue;
        }
        HSD_JObj* column = root->child;
        HSD_JObj* top = column != NULL ? column->child : NULL;
        Vec3 foot, under;
        lb_8000B1CC(root, NULL, &foot);
        const int k = foot.x < 0.f ? 0 : 1;
        if (top == NULL || (root->flags & JOBJ_HIDDEN)) {
            continue;
        }
        lb_8000B1CC(top, NULL, &under);
        j.pos[k][0] = foot.x;
        j.pos[k][1] = foot.y + 0.4f * (under.y - foot.y);
        j.pos[k][2] = foot.z;
        j.height[k] = fmaxf(column->scale.y, 0.f);
    }
    __atomic_add_fetch(&s_jets_seq, 1, __ATOMIC_ACQ_REL);
    s_jets = j;
    __atomic_add_fetch(&s_jets_seq, 1, __ATOMIC_RELEASE);
}

/* Big Blue's racers, one a lane (grbigblue.c): where each car is and how
 * much of it there is (its fade), published by the game thread each drawn
 * frame under a sequence count, like the jets. */
#define RACERS 4
typedef struct Racers {
    float pos[RACERS][3];
    float presence[RACERS]; /* 0: the lane's empty */
} Racers;
static Racers s_racers;
static unsigned s_racers_seq;

static void racers_frame(void) {
    Racers r = {0};
    for (int k = 0; k < RACERS; k++) {
        if (!grBigBlue_pc_racer(k, r.pos[k], &r.presence[k])) {
            r.presence[k] = 0.f;
        }
        r.presence[k] = fminf(fmaxf(r.presence[k], 0.f), 1.f);
    }
    __atomic_add_fetch(&s_racers_seq, 1, __ATOMIC_ACQ_REL);
    s_racers = r;
    __atomic_add_fetch(&s_racers_seq, 1, __ATOMIC_RELEASE);
}

/* Big Blue's other craft: the passing ones fighters ride (part 32, depth
 * first from its root: two hover platforms, bodies at joints 2 and 7, each
 * on two jets, and a smaller round one at 12 on one), and the Falcon Flyer
 * the fight starts on (part 35, its two rear engines at joints 6 and 9),
 * which flies off after about 45 s. Where each is and whether it's there,
 * published with the racers' sequence count. */
#define CRAFT 4 /* the two platforms, the ball, the Flyer */
typedef struct Craft {
    float pos[CRAFT][3];
    bool here[CRAFT];
} Craft;
static Craft s_craft;

static HSD_JObj* joint_at(HSD_JObj* root, int want) {
    HSD_JObj* stack[64];
    int sp = 0, idx = 0;
    stack[sp++] = root;
    while (sp > 0) {
        HSD_JObj* j = stack[--sp];
        if (idx++ == want) {
            return j;
        }
        if (j->next != NULL && sp < 64) {
            stack[sp++] = j->next;
        }
        if (j->child != NULL && sp < 64) {
            stack[sp++] = j->child;
        }
    }
    return NULL;
}

static bool joint_shown(HSD_JObj* root, int k, float pos[3]) {
    HSD_JObj* j = joint_at(root, k);
    if (j == NULL || (j->flags & JOBJ_HIDDEN)) {
        return false;
    }
    Vec3 w;
    lb_8000B1CC(j, NULL, &w);
    pos[0] = w.x;
    pos[1] = w.y;
    pos[2] = w.z;
    return isfinite(w.x) && isfinite(w.y) && isfinite(w.z);
}

static void craft_frame(void) {
    Craft c = {0};
    for (HSD_GObj* g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_GROUND]; g != NULL; g = g->next) {
        const Ground* gp = g->user_data;
        HSD_JObj* root = g->hsd_obj;
        if (gp == NULL || root == NULL) {
            continue;
        }
        if (gp->map_id == 32) {
            static const int bodies[3] = {2, 7, 12};
            for (int k = 0; k < 3; k++) {
                c.here[k] = joint_shown(root, bodies[k], c.pos[k]);
                c.pos[k][1] -= 8.f; /* down at its jets */
            }
        } else if (gp->map_id == 35) {
            float a[3], b[3];
            if (joint_shown(root, 6, a) && joint_shown(root, 9, b)) {
                for (int i = 0; i < 3; i++) {
                    c.pos[3][i] = 0.5f * (a[i] + b[i]);
                }
                c.here[3] = true;
            }
        }
    }
    __atomic_add_fetch(&s_racers_seq, 1, __ATOMIC_ACQ_REL);
    s_craft = c;
    __atomic_add_fetch(&s_racers_seq, 1, __ATOMIC_RELEASE);
}

bool pc_ambience_racers(void) {
    return enabled() && pc_spatial_enabled();
}

/* Kongo Jungle's jungle: Melee plays a stereo recording (sound 0x55D, 16
 * kHz, 18.4 s) once as the stage loads; it rises over half a second, holds
 * to about 7.5 s (a louder call at 5.8 to 6.5 s) and fades over the rest.
 * Here it plays from its start to 7 s, then on through the fight from
 * stretches of its steady part picked at random, each crossfading into the
 * next, so nothing comes round on a cycle; the call comes back now and
 * then. Decoded from the bank as the stage loads (a few ms, in the load),
 * into whichever of two buffers the audio thread isn't on. */
#define JUNGLE_SFX 0x55D
#define JUNGLE_MAX 300000 /* samples a channel: 18.75 s at 16 kHz */
static int16_t s_jungle_buf[2][2][JUNGLE_MAX];
static int s_jungle_len[2];
static int s_jungle_rate[2];
static int s_jungle_ready = -1; /* the buffer last decoded (__atomic); -1: none */
static unsigned s_jungle_serial; /* bumped each load (__atomic): play it from the start */
static int s_jungle_playing = -1; /* audio thread writes: the buffer it's on (__atomic) */

bool pc_ambience_kongo_jungle(void) {
    if (!enabled() || !pc_spatial_enabled()) {
        return false;
    }
    const int b = __atomic_load_n(&s_jungle_playing, __ATOMIC_ACQUIRE) == 0 ? 1 : 0;
    int n[2] = {0, 0}, rate = 0;
    for (int c = 0; c < 2; c++) {
        const void *addr, *adpcm;
        int voices;
        if (!HSD_SynthSFXSample(JUNGLE_SFX, c, &addr, &adpcm, &rate, &voices) || voices != 2) {
            return false;
        }
        n[c] = pc_audio_decode_sample(addr, adpcm, s_jungle_buf[b][c], JUNGLE_MAX);
    }
    if (n[0] < rate * 8 || n[0] != n[1] || rate <= 0) {
        return false; /* not the recording expected: the stage plays its own */
    }
    s_jungle_len[b] = n[0];
    s_jungle_rate[b] = rate;
    __atomic_store_n(&s_jungle_ready, b, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_jungle_serial, 1, __ATOMIC_RELEASE);
    return true;
}

static bool s_frozen; /* game thread: the last tick held the stage's procs */

void pc_ambience_tick(uint64_t proc_mask) {
    s_frozen = (proc_mask >> HSD_GOBJ_PLINK_GROUND) & 1u;
}

void pc_ambience_frame(void) {
    pc_stage_mix_frame(stage_info.grkind); /* Melee's own placed stage sounds mix there too */
    { /* MELEE_STAGE_SURVEY=1: the stage's floors, camera bounds and blast
       * zones, logged once a fight is up: what xr_scene.c's s_placements is
       * measured from (with MELEE_DEBUG_VS_STAGE, any stage kind). */
        static int n;
        if (getenv("MELEE_STAGE_SURVEY") != NULL && ++n == 120) {
            const float bl = Stage_GetBlastZoneLeftOffset(), br = Stage_GetBlastZoneRightOffset();
            const float bb = Stage_GetBlastZoneBottomOffset(), bt = Stage_GetBlastZoneTopOffset();
            float top, mnx, mxx, lo = 1e9f, hi = -1e9f, xl = 1e9f, xr = -1e9f;
            for (float y = bb; y < bt; y += 5.f) {
                if (mpLib_FloorExtent(bl, br, y, y + 5.f, &top, &mnx, &mxx)) {
                    lo = fminf(lo, top);
                    hi = fmaxf(hi, top);
                    xl = fminf(xl, mnx);
                    xr = fmaxf(xr, mxx);
                }
            }
            pc_log_line("survey: grkind 0x%02X floors x %.0f..%.0f y %.0f..%.0f; camera x %.0f..%.0f y %.0f..%.0f; "
                        "blast x %.0f..%.0f y %.0f..%.0f",
                        stage_info.grkind, xl, xr, lo, hi, Stage_GetCamBoundsLeftOffset(),
                        Stage_GetCamBoundsRightOffset(), Stage_GetCamBoundsBottomOffset(),
                        Stage_GetCamBoundsTopOffset(), bl, br, bb, bt);
        }
    }
    if (!enabled()) {
        return;
    }
    /* None while the stage is held for placing: they start with the fight.
     * None once it's frozen for GAME! or TIME! (frozen in a pause, they
     * duck with the music instead). */
    float duck;
    __atomic_load(&s_duck_target, &duck, __ATOMIC_RELAXED);
    const bool over = s_frozen && duck >= 1.f;
    const int kind = pc_xr_place_holding() || over         ? AMB_NONE
                     : stage_info.grkind == Gr_Kind_Corneria ? AMB_WIND
                     : stage_info.grkind == Gr_Kind_Kongo  ? AMB_FALLS
                     : stage_info.grkind == Gr_Kind_Izumi  ? AMB_FOUNTAIN
                     : stage_info.grkind == Gr_Kind_BigBlue ? AMB_RUSH
                                                           : AMB_NONE;
    if (kind == AMB_FOUNTAIN) {
        jets_frame();
    } else if (kind == AMB_RUSH) {
        racers_frame();
        craft_frame();
    }
    __atomic_store_n(&s_kind, kind, __ATOMIC_RELAXED);
    __atomic_store_n(&s_stamp, now_ns(), __ATOMIC_RELEASE);
}

/* ---- audio thread ----------------------------------------------------- */

static uint32_t rnd(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static float rnd_unit(uint32_t* s) { /* [0, 1) */
    return (float)(rnd(s) >> 8) * (1.0f / 16777216.0f);
}

/* A slow random walk between gusts: a new target every so often, glided to. */
typedef struct Gust {
    uint32_t seed;
    float value, target;
    float hold; /* seconds until the next target */
} Gust;

static void gust_step(Gust* g, float dt) {
    g->hold -= dt;
    if (g->hold <= 0.f) {
        g->target = 0.15f + 0.85f * rnd_unit(&g->seed);
        g->hold = 0.6f + 2.4f * rnd_unit(&g->seed);
    }
    g->value += (g->target - g->value) * (1.f - expf(-dt / 0.9f));
}

/* Wind, one channel: noise through two one-pole lowpasses and a highpass
 * (a rush whose brightness follows the gust), plus a little of the same
 * noise through a soft band-pass (the whistle over the hull). */
typedef struct WindChannel {
    uint32_t seed;
    Gust own;
    float lp1, lp2, hp;  /* filter states */
    float bp_low, bp_band;
} WindChannel;

static struct {
    bool init;
    float level; /* 0..1, the fade */
    Gust shared;
    WindChannel ch[2];
} s_wind;

static void wind_init(void) {
    s_wind.shared = (Gust){.seed = 0x9E3779B9u, .value = 0.5f, .target = 0.5f};
    for (int c = 0; c < 2; c++) {
        WindChannel* w = &s_wind.ch[c];
        *w = (WindChannel){0};
        w->seed = c == 0 ? 0x1234567u : 0x7654321u;
        w->own = (Gust){.seed = c == 0 ? 0xA5A5A5A5u : 0x5A5A5A5Au, .value = 0.5f, .target = 0.5f};
    }
    s_wind.init = true;
}

static float ambience_gain(void) {
    static float cached = -1.f;
    if (cached < 0.f) {
        const char* v = getenv("MELEE_AMBIENCE_GAIN");
        cached = powf(10.f, (v != NULL ? (float)atof(v) : 0.f) / 20.f);
    }
    return cached;
}

static void wind_mix(float* out, int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_wind.level <= 0.f) {
        s_wind.init = false; /* the next fight starts fresh */
        return;
    }
    if (!s_wind.init) {
        wind_init();
    }
    const float step = dt / AMB_FADE_S;
    const float level0 = s_wind.level;
    s_wind.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - step);

    gust_step(&s_wind.shared, dt);
    const float volume = pc_get_sfx_volume() * ambience_gain();
    const float duck0 = s_duck0, duck1 = s_duck;
    const float two_pi_over_rate = 6.2831853f / (float)rate;
    for (int c = 0; c < 2; c++) {
        WindChannel* w = &s_wind.ch[c];
        gust_step(&w->own, dt);
        const float g = 0.7f * s_wind.shared.value + 0.3f * w->own.value;
        /* Brightness and level rise with the gust. */
        const float cutoff = 180.f + 650.f * g;
        const float a = 1.f - expf(-two_pi_over_rate * cutoff);
        const float a_hp = 1.f - expf(-two_pi_over_rate * 70.f);
        const float centre = 500.f + 900.f * g;
        const float f = 2.f * sinf(3.14159265f * centre / (float)rate);
        const float q = 0.35f; /* damping: a soft band */
        /* The rush's level falls with its cutoff; this keeps it about even. */
        const float makeup = sqrtf(2400.f / cutoff);
        /* About -37 dBFS on average and -32 in gusts, a fight's mix running
         * about -19: under the fight, there between its sounds (4 dB down
         * from the first take, which the player found a bit loud). */
        const float amp = (0.25f + 0.75f * g * g) * 0.125f * volume;
        for (int i = 0; i < frames; i++) {
            const float n = rnd_unit(&w->seed) * 2.f - 1.f;
            w->lp1 += a * (n - w->lp1);
            w->lp2 += a * (w->lp1 - w->lp2);
            w->hp += a_hp * (w->lp2 - w->hp);
            const float rush = (w->lp2 - w->hp) * makeup;
            const float high = n - w->bp_low - q * w->bp_band;
            w->bp_band += f * high;
            w->bp_low += f * w->bp_band;
            const float whistle = w->bp_band * 0.35f;
            const float t = (float)i / (float)frames;
            const float lv = (level0 + (s_wind.level - level0) * t) * (duck0 + (duck1 - duck0) * t);
            out[i * 2 + c] += (rush + whistle) * amp * lv;
        }
    }
}

/* Kongo Jungle's waterfall, which the stage has no sound for. It pours off
 * the river's lip (part 4 joint 27's bones, z -32, x -97 to 118) and hangs
 * under the plateau (joint 3 at y -48), and is heard from two points across
 * its face, each its own noise so the pair reads wide: a steady roar (pink
 * noise, its lows and highs rolled off, a body around 500 Hz) and a fizz of
 * spray over it (bright noise in short random bursts), breathing slowly.
 * Placed sounds (pc_spatial_render_placed): heard where the waterfall
 * stands, nearer or farther as the stage is placed. */
#define FALLS_SOURCES 2
static const float s_falls_pos[FALLS_SOURCES][3] = {{-55.f, -40.f, -28.f}, {75.f, -40.f, -28.f}};

typedef struct FallsSource {
    uint32_t seed;
    Gust swell;
    float p0, p1, p2;  /* pink noise */
    float hp, lp;      /* the roar's roll-offs */
    float bp_low, bp_band;
    float fz_hp, fz_env, fz_target; /* the fizz */
    int fz_hold;
    StageMixState mix;
} FallsSource;

static struct {
    bool init;
    float level;
    FallsSource src[FALLS_SOURCES];
} s_falls;

static void falls_init(void) {
    for (int c = 0; c < FALLS_SOURCES; c++) {
        FallsSource* f = &s_falls.src[c];
        *f = (FallsSource){0};
        f->seed = c == 0 ? 0x2545F491u : 0x9E3779B1u;
        f->swell = (Gust){.seed = c == 0 ? 0xC0FFEE11u : 0x0DDBA11u, .value = 0.8f, .target = 0.8f};
    }
    s_falls.init = true;
}

static void falls_mix(float* out, int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_falls.level <= 0.f) {
        s_falls.init = false;
        return;
    }
    if (!s_falls.init) {
        falls_init();
    }
    const float level0 = s_falls.level;
    s_falls.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
    const float volume = pc_get_sfx_volume() * ambience_gain();
    const float duck0 = s_duck0, duck1 = s_duck;
    const float w = 6.2831853f / (float)rate;
    const float a_hp = 1.f - expf(-w * 90.f);
    const float a_lp = 1.f - expf(-w * 3200.f);
    const float a_fz = 1.f - expf(-w * 2500.f);
    const float f_bp = 2.f * sinf(3.14159265f * 500.f / (float)rate);
    float mono[160];
    if (frames > 160) {
        frames = 160;
    }
    for (int c = 0; c < FALLS_SOURCES; c++) {
        FallsSource* f = &s_falls.src[c];
        gust_step(&f->swell, dt * 0.5f); /* half speed: a slow breath */
        /* About -34.5 dBFS each before placing; placed, the pair came to
         * about -32 in a flat desktop mix (fight and music about -23): a
         * little over the wind, the waterfall being right there. On the
         * headset it was a bit quiet against the jungle: 3 dB up. */
        const float amp = (0.75f + 0.25f * f->swell.value) * 0.093f * volume;
        for (int i = 0; i < frames; i++) {
            const float n = rnd_unit(&f->seed) * 2.f - 1.f;
            f->p0 = 0.99765f * f->p0 + n * 0.0990460f;
            f->p1 = 0.96300f * f->p1 + n * 0.2965164f;
            f->p2 = 0.57000f * f->p2 + n * 1.0526913f;
            const float pink = (f->p0 + f->p1 + f->p2 + n * 0.1848f) * 0.25f;
            f->lp += a_lp * (pink - f->lp);
            f->hp += a_hp * (f->lp - f->hp);
            const float roar = f->lp - f->hp;
            const float high = pink - f->bp_low - 0.7f * f->bp_band;
            f->bp_band += f_bp * high;
            f->bp_low += f_bp * f->bp_band;
            /* Spray: white noise over 2.5 kHz, gated by a random envelope
             * that jumps every few milliseconds and glides. */
            if (--f->fz_hold <= 0) {
                const float r = rnd_unit(&f->seed);
                f->fz_target = r * r;
                f->fz_hold = 40 + (int)(rnd_unit(&f->seed) * 160.f);
            }
            f->fz_env += (f->fz_target - f->fz_env) * 0.01f;
            f->fz_hp += a_fz * (n - f->fz_hp);
            const float fizz = (n - f->fz_hp) * f->fz_env;
            const float t = (float)i / (float)frames;
            const float lv = (level0 + (s_falls.level - level0) * t) * (duck0 + (duck1 - duck0) * t);
            mono[i] = (roar + 0.35f * f->bp_band + 0.18f * fizz) * amp * lv;
        }
        pc_stage_mix_apply(STAGE_MIX_FALLS, &f->mix, mono, frames, 1);
        if (!pc_spatial_render_placed(c, s_falls_pos[c], mono)) {
            for (int i = 0; i < frames; i++) { /* not placed (spatial audio bypassed): plain, centred */
                out[i * 2] += mono[i] * 0.7071f;
                out[i * 2 + 1] += mono[i] * 0.7071f;
            }
        }
    }
}

/* Fountain of Dreams' fountain, which the stage has no sound for: a basin
 * behind the stage (part 3, joint 38: x 0, z -20.2, its rim at y 20.4)
 * whose water falls all around it as a curtain onto the stage's pool
 * (ripples at y 0.6). Heard from two points across the curtain's foot,
 * each its own noise: a soft pour, a patter of spray and the plinks of
 * drops and bubbles (Minnaert's: a sine whose pitch rises as it dies).
 * Gentler than Kongo's waterfall: a garden fountain at night. And the jets
 * the side platforms ride, a hiss with a low gurgle, as loud as the jet is
 * tall, gone when the platform sinks. Placed sounds, slots 0-1 the curtain,
 * 2-3 the jets. */
#define CURTAIN_SOURCES 2
static const float s_curtain_pos[CURTAIN_SOURCES][3] = {{-11.f, 4.f, -20.2f}, {11.f, 4.f, -20.2f}};
#define DROPS 6

typedef struct Drop {
    float phase, freq, amp, env, decay, rise; /* decay: per sample; rise: pitch per sample */
} Drop;

typedef struct CurtainSource {
    uint32_t seed;
    Gust swell;
    float p0, p1, p2;     /* pink noise */
    float pour_hp, pour_lp;
    float sp_hp, sp_lp, sp_env, sp_target; /* the patter */
    int sp_hold;
    Drop drop[DROPS];
    float next_drop; /* samples */
    StageMixState mix;
} CurtainSource;

typedef struct JetSource {
    uint32_t seed;
    float level;               /* glides to the jet's height */
    float pos[3];              /* where it was last seen, to fade out there */
    float bp_low, bp_band;     /* the hiss */
    float g_lp, g_env, g_target; /* the gurgle */
    int g_hold;
    StageMixState mix;
} JetSource;

static struct {
    bool init;
    float level;
    CurtainSource src[CURTAIN_SOURCES];
    JetSource jet[JETS];
} s_fountain;

static void fountain_init(void) {
    for (int c = 0; c < CURTAIN_SOURCES; c++) {
        CurtainSource* f = &s_fountain.src[c];
        *f = (CurtainSource){0};
        f->seed = c == 0 ? 0x68E31DA4u : 0xB5297A4Du;
        f->swell = (Gust){.seed = c == 0 ? 0x1B56C4E9u : 0x7FB5D329u, .value = 0.7f, .target = 0.7f};
    }
    for (int k = 0; k < JETS; k++) {
        s_fountain.jet[k] = (JetSource){.seed = k == 0 ? 0x2C1B3C6Du : 0x297A2D39u};
    }
    s_fountain.init = true;
}

static float curtain_sample(CurtainSource* f, const float a[4], float rate) {
    const float n = rnd_unit(&f->seed) * 2.f - 1.f;
    f->p0 = 0.99765f * f->p0 + n * 0.0990460f;
    f->p1 = 0.96300f * f->p1 + n * 0.2965164f;
    f->p2 = 0.57000f * f->p2 + n * 1.0526913f;
    const float pink = (f->p0 + f->p1 + f->p2 + n * 0.1848f) * 0.25f;
    /* The pour: pink noise, 250 Hz to 1.6 kHz. */
    f->pour_lp += a[0] * (pink - f->pour_lp);
    f->pour_hp += a[1] * (f->pour_lp - f->pour_hp);
    const float pour = f->pour_lp - f->pour_hp;
    /* The patter: white noise, 1.5 to 7 kHz, through an envelope that
     * jumps every millisecond or few. */
    if (--f->sp_hold <= 0) {
        const float r = rnd_unit(&f->seed);
        f->sp_target = 0.2f + 0.8f * r * r;
        f->sp_hold = 24 + (int)(rnd_unit(&f->seed) * 96.f);
    }
    f->sp_env += (f->sp_target - f->sp_env) * 0.02f;
    f->sp_lp += a[2] * (n - f->sp_lp);
    f->sp_hp += a[3] * (f->sp_lp - f->sp_hp);
    const float patter = (f->sp_lp - f->sp_hp) * f->sp_env;
    /* The drops: about 30 a second, 700 Hz to 3 kHz, 4 to 18 ms. */
    if (--f->next_drop <= 0.f) {
        for (int d = 0; d < DROPS; d++) {
            Drop* p = &f->drop[d];
            if (p->env < 1e-3f) {
                const float tau = (0.004f + 0.014f * rnd_unit(&f->seed)) * rate;
                p->freq = (700.f + 2300.f * rnd_unit(&f->seed) * rnd_unit(&f->seed)) / rate;
                p->decay = expf(-1.f / tau);
                p->rise = p->freq * 1.5f / tau;
                p->amp = 0.3f + 0.7f * rnd_unit(&f->seed);
                p->env = 1.f;
                p->phase = 0.f;
                break;
            }
        }
        f->next_drop = -logf(1.f - rnd_unit(&f->seed) * 0.999f) * rate / 30.f;
    }
    float drops = 0.f;
    for (int d = 0; d < DROPS; d++) {
        Drop* p = &f->drop[d];
        if (p->env < 1e-3f) {
            continue;
        }
        drops += sinf(6.2831853f * p->phase) * p->env * p->amp;
        p->phase += p->freq;
        p->phase -= floorf(p->phase);
        p->freq += p->rise;
        p->env *= p->decay;
    }
    return pour * 1.1f + patter * 0.55f + drops * 0.09f;
}

static float jet_sample(JetSource* j, float f_bp, float a_lp) {
    const float n = rnd_unit(&j->seed) * 2.f - 1.f;
    /* The hiss: white noise through a soft band around 2.2 kHz. */
    const float high = n - j->bp_low - 0.9f * j->bp_band;
    j->bp_band += f_bp * high;
    j->bp_low += f_bp * j->bp_band;
    /* The gurgle: noise under 350 Hz, in uneven bursts. */
    if (--j->g_hold <= 0) {
        const float r = rnd_unit(&j->seed);
        j->g_target = r * r;
        j->g_hold = 200 + (int)(rnd_unit(&j->seed) * 900.f);
    }
    j->g_env += (j->g_target - j->g_env) * 0.004f;
    j->g_lp += a_lp * (n - j->g_lp);
    return j->bp_band * 0.5f + j->g_lp * j->g_env * 2.2f;
}

static void fountain_mix(float* out, int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_fountain.level <= 0.f) {
        s_fountain.init = false;
        return;
    }
    if (!s_fountain.init) {
        fountain_init();
    }
    const float level0 = s_fountain.level;
    s_fountain.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
    const float volume = pc_get_sfx_volume() * ambience_gain();
    const float duck0 = s_duck0, duck1 = s_duck;
    const float w = 6.2831853f / (float)rate;
    const float a[4] = {1.f - expf(-w * 1600.f), 1.f - expf(-w * 250.f), 1.f - expf(-w * 7000.f),
                        1.f - expf(-w * 1500.f)};
    float mono[160];
    if (frames > 160) {
        frames = 160;
    }
    for (int c = 0; c < CURTAIN_SOURCES; c++) {
        CurtainSource* f = &s_fountain.src[c];
        gust_step(&f->swell, dt * 0.4f);
        /* With the jets, about -37.5 dBFS in a flat desktop mix between the
         * fight's sounds (music muted): over Corneria's wind, under Kongo's
         * waterfall. */
        const float amp = (0.8f + 0.2f * f->swell.value) * 0.045f * volume;
        for (int i = 0; i < frames; i++) {
            const float t = (float)i / (float)frames;
            const float lv = (level0 + (s_fountain.level - level0) * t) * (duck0 + (duck1 - duck0) * t);
            mono[i] = curtain_sample(f, a, (float)rate) * amp * lv;
        }
        pc_stage_mix_apply(STAGE_MIX_FOUNTAIN, &f->mix, mono, frames, 1);
        if (!pc_spatial_render_placed(c, s_curtain_pos[c], mono)) {
            for (int i = 0; i < frames; i++) { /* not placed (spatial audio bypassed): plain, centred */
                out[i * 2] += mono[i] * 0.7071f;
                out[i * 2 + 1] += mono[i] * 0.7071f;
            }
        }
    }
    /* The jets, where the game last drew them. */
    Jets jets;
    unsigned seq;
    do {
        seq = __atomic_load_n(&s_jets_seq, __ATOMIC_ACQUIRE);
        jets = s_jets;
    } while ((seq & 1u) != 0 || seq != __atomic_load_n(&s_jets_seq, __ATOMIC_ACQUIRE));
    const float f_bp = 2.f * sinf(3.14159265f * 2200.f / (float)rate);
    const float a_lp = 1.f - expf(-w * 350.f);
    for (int k = 0; k < JETS; k++) {
        JetSource* j = &s_fountain.jet[k];
        const float want = sqrtf(fminf(jets.height[k], 1.f));
        if (jets.height[k] > 0.f) {
            memcpy(j->pos, jets.pos[k], sizeof(j->pos));
        }
        const float jl0 = j->level;
        j->level += (want - j->level) * (1.f - expf(-dt / 0.25f));
        if (jl0 < 1e-3f && j->level < 1e-3f) {
            continue; /* sunk */
        }
        const float amp = 0.024f * volume;
        for (int i = 0; i < frames; i++) {
            const float t = (float)i / (float)frames;
            const float lv = (level0 + (s_fountain.level - level0) * t) * (duck0 + (duck1 - duck0) * t) *
                             (jl0 + (j->level - jl0) * t);
            mono[i] = jet_sample(j, f_bp, a_lp) * amp * lv;
        }
        pc_stage_mix_apply(STAGE_MIX_JETS, &j->mix, mono, frames, 1);
        if (!pc_spatial_render_placed(CURTAIN_SOURCES + k, j->pos, mono)) {
            for (int i = 0; i < frames; i++) {
                out[i * 2] += mono[i] * 0.7071f;
                out[i * 2 + 1] += mono[i] * 0.7071f;
            }
        }
    }
}

/* The jungle's playing (audio thread): up to two stretches of the
 * recording at once, the newer crossfading in over the older. */
#define JUNGLE_INTRO_S 7.0f      /* from the start, as Melee plays it */
#define JUNGLE_STEADY_S 0.75f    /* the steady part: from here... */
#define JUNGLE_STEADY_END_S 5.6f /* ...to here, short of the call */
#define JUNGLE_CALL_S 4.4f       /* a stretch with the call starts here */
#define JUNGLE_CALL_P 0.15f      /* that often */
#define JUNGLE_SEG_S 2.5f
#define JUNGLE_XFADE_S 0.8f
/* Its level as Melee plays it: 10 dB under the decoded recording, measured
 * against the original in a flat desktop mix. */
#define JUNGLE_GAIN 0.31f

typedef struct JungleSeg {
    bool on;
    double pos;   /* source samples */
    double end;   /* where it has faded out */
    double fade_in_end; /* where its fade-in is done (pos at start + crossfade) */
    double fade_out_start;
} JungleSeg;

static struct {
    unsigned serial;
    int buf;
    float level;
    float played; /* seconds since it started */
    uint32_t seed;
    JungleSeg seg[2];
} s_jungle = {.buf = -1};

static void jungle_seg(JungleSeg* g, double start, double len, double xf, bool fade_in) {
    g->on = true;
    g->pos = start;
    g->end = start + len;
    g->fade_in_end = fade_in ? start + xf : start;
    g->fade_out_start = g->end - xf;
}

static void jungle_mix(float* out, int frames, int rate, bool on) {
    const unsigned serial = __atomic_load_n(&s_jungle_serial, __ATOMIC_ACQUIRE);
    if (on && serial != s_jungle.serial) {
        /* A new load: from the start, on its buffer. */
        s_jungle.serial = serial;
        s_jungle.buf = __atomic_load_n(&s_jungle_ready, __ATOMIC_ACQUIRE);
        __atomic_store_n(&s_jungle_playing, s_jungle.buf, __ATOMIC_RELEASE);
        if (s_jungle.buf >= 0) {
            const double sr = s_jungle_rate[s_jungle.buf];
            s_jungle.seed = 0x3C6EF372u ^ serial;
            s_jungle.seg[1].on = false;
            jungle_seg(&s_jungle.seg[0], 0.0, JUNGLE_INTRO_S * sr, JUNGLE_XFADE_S * sr, false);
            s_jungle.level = 1.f; /* the recording rises by itself */
            s_jungle.played = 0.f;
        }
    }
    const int b = s_jungle.buf;
    if (b < 0 || (!s_jungle.seg[0].on && !s_jungle.seg[1].on)) {
        return;
    }
    const float dt = (float)frames / (float)rate;
    s_jungle.played += dt;
    const float level0 = s_jungle.level;
    s_jungle.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
    if (!on && s_jungle.level <= 0.f) {
        if (s_jungle.played < 2.f) {
            s_jungle.serial--; /* cut off as it began: from the start next time */
        }
        s_jungle.seg[0].on = s_jungle.seg[1].on = false;
        s_jungle.buf = -1;
        __atomic_store_n(&s_jungle_playing, -1, __ATOMIC_RELEASE);
        return;
    }
    const double sr = s_jungle_rate[b];
    const double step = sr / (double)rate;
    const double xf = JUNGLE_XFADE_S * sr;
    const int len = s_jungle_len[b];
    const float volume = pc_get_sfx_volume() * ambience_gain() * JUNGLE_GAIN;
    const float duck0 = s_duck0, duck1 = s_duck;
    for (int i = 0; i < frames; i++) {
        float l = 0.f, r = 0.f;
        for (int k = 0; k < 2; k++) {
            JungleSeg* g = &s_jungle.seg[k];
            if (!g->on) {
                continue;
            }
            /* The next stretch starts as this one starts fading out. */
            if (g->pos >= g->fade_out_start && !s_jungle.seg[k ^ 1].on) {
                double start = (JUNGLE_STEADY_S + (JUNGLE_STEADY_END_S - JUNGLE_STEADY_S - JUNGLE_SEG_S) *
                                                      rnd_unit(&s_jungle.seed)) * sr;
                if (rnd_unit(&s_jungle.seed) < JUNGLE_CALL_P) {
                    start = JUNGLE_CALL_S * sr;
                }
                jungle_seg(&s_jungle.seg[k ^ 1], start, JUNGLE_SEG_S * sr, xf, true);
            }
            float gain = 1.f;
            if (g->pos < g->fade_in_end) {
                gain = sinf(1.5707963f * (float)((g->pos - (g->fade_in_end - xf)) / xf));
            } else if (g->pos >= g->fade_out_start) {
                gain = cosf(1.5707963f * (float)((g->pos - g->fade_out_start) / xf));
            }
            const int p = (int)g->pos;
            if (p + 1 < len) {
                const float f = (float)(g->pos - p);
                const int16_t* bl = s_jungle_buf[b][0];
                const int16_t* br = s_jungle_buf[b][1];
                l += ((float)bl[p] + ((float)bl[p + 1] - (float)bl[p]) * f) * gain;
                r += ((float)br[p] + ((float)br[p + 1] - (float)br[p]) * f) * gain;
            }
            g->pos += step;
            if (g->pos >= g->end || p + 1 >= len) {
                g->on = false;
            }
        }
        const float t = (float)i / (float)frames;
        const float lv = (level0 + (s_jungle.level - level0) * t) * (duck0 + (duck1 - duck0) * t) * volume *
                         (1.f / 32768.f);
        out[i * 2] += l * lv;
        out[i * 2 + 1] += r * lv;
    }
}

/* Big Blue's rush of air: the fight rides cars racing down the track, and
 * the wind of that speed is all around (the crowd's ring, like Corneria's
 * wind). Steadier and brighter than Corneria's gusts: noise from 150 Hz to
 * about 2 kHz, its brightness and level surging slowly, buffeted by quick
 * turbulence (two random flutters, around 6 and 13 Hz), with a thin
 * whistle of air past edges near 3.5 kHz. */
typedef struct Flutter {
    uint32_t seed;
    float value, target, rate; /* rate: new targets a second */
    float hold;
} Flutter;

static float flutter_step(Flutter* f, float dt) {
    f->hold -= dt;
    if (f->hold <= 0.f) {
        f->target = rnd_unit(&f->seed);
        f->hold = 1.f / f->rate;
    }
    f->value += (f->target - f->value) * (1.f - expf(-dt * f->rate * 2.f));
    return f->value;
}

typedef struct RushChannel {
    uint32_t seed;
    Flutter slow, fast;
    float lp1, lp2, hp;
    float bp_low, bp_band;
} RushChannel;

static struct {
    bool init;
    float level;
    Gust surge;
    RushChannel ch[2];
} s_rush;

static void rush_init(void) {
    s_rush.surge = (Gust){.seed = 0x51ED270Bu, .value = 0.6f, .target = 0.6f};
    for (int c = 0; c < 2; c++) {
        RushChannel* r = &s_rush.ch[c];
        *r = (RushChannel){0};
        r->seed = c == 0 ? 0x3B9ACA07u : 0x2545F491u;
        r->slow = (Flutter){.seed = c == 0 ? 0x1F123BB5u : 0x5F356495u, .value = 0.5f, .rate = 6.f};
        r->fast = (Flutter){.seed = c == 0 ? 0x2F8D3E61u : 0x6C8E9CF5u, .value = 0.5f, .rate = 13.f};
    }
    s_rush.init = true;
}

static void rush_mix(float* out, int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_rush.level <= 0.f) {
        s_rush.init = false;
        return;
    }
    if (!s_rush.init) {
        rush_init();
    }
    const float level0 = s_rush.level;
    s_rush.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
    gust_step(&s_rush.surge, dt);
    const float volume = pc_get_sfx_volume() * ambience_gain();
    const float duck0 = s_duck0, duck1 = s_duck;
    const float w = 6.2831853f / (float)rate;
    const float a_hp = 1.f - expf(-w * 150.f);
    const float f_bp = 2.f * sinf(3.14159265f * 3500.f / (float)rate);
    for (int c = 0; c < 2; c++) {
        RushChannel* r = &s_rush.ch[c];
        const float g = s_rush.surge.value;
        const float buffet = 0.7f + 0.3f * (0.6f * flutter_step(&r->slow, dt) + 0.4f * flutter_step(&r->fast, dt));
        const float cutoff = 1200.f + 1000.f * g;
        const float a = 1.f - expf(-w * cutoff);
        /* The rush's level falls with its cutoff; this keeps it about even. */
        const float makeup = sqrtf(2400.f / cutoff);
        /* About -36 dBFS in a flat desktop mix between the fight's sounds
         * (the racers' engines about -39 there), Corneria's wind's level. */
        const float amp = (0.55f + 0.45f * g) * buffet * 0.071f * volume;
        for (int i = 0; i < frames; i++) {
            const float n = rnd_unit(&r->seed) * 2.f - 1.f;
            r->lp1 += a * (n - r->lp1);
            r->lp2 += a * (r->lp1 - r->lp2);
            r->hp += a_hp * (r->lp2 - r->hp);
            const float rush = (r->lp2 - r->hp) * makeup;
            const float high = n - r->bp_low - 0.25f * r->bp_band;
            r->bp_band += f_bp * high;
            r->bp_low += f_bp * r->bp_band;
            const float t = (float)i / (float)frames;
            const float lv = (level0 + (s_rush.level - level0) * t) * (duck0 + (duck1 - duck0) * t);
            out[i * 2 + c] += (rush + r->bp_band * 0.06f) * amp * lv;
        }
    }
}

/* Big Blue's racers' engines, in place of Melee's (its loops, 0x6DDD2-5,
 * are a hiss that disappeared into the rush of air): hover ships, so no
 * pistons and no exhaust. Each lane's its own machine, heard from its car:
 *  - the hum of its anti-gravity, a sawtooth around 90-115 Hz rolled off
 *    past 900 Hz, throbbing about 20 times a second as it rides the track;
 *  - the whine of its turbine ten-odd times higher, a sine and a little of
 *    its octave, wavering;
 *  - a little air off its thrusters (noise around 2.4 kHz).
 * Doppler from how fast the car closes on or leaves the listener, so it
 * sweeps down as it passes. Placed (slots 0-3, one a lane); the mixer's
 * ENGINES channel. */
#define RACER_DOPPLER_C 2400.f /* game units a second: the speed of sound here */

typedef struct RacerSource {
    uint32_t seed;
    float level;    /* glides to the car's presence */
    float pos[3];   /* where it was last seen */
    float dist, vr; /* from the listener (game units), and its rate (smoothed) */
    bool have_dist;
    float pitch;    /* Doppler, glided */
    Gust rpm;       /* the engine's own wander: the cars mostly hold their places */
    float ph_hum, ph_whine, ph_whine2, ph_vib, ph_throb;
    float lp1, lp2;
    float bp_low, bp_band;
    StageMixState mix;
} RacerSource;

static const struct {
    float hum, whine, throb;
} s_racer_voice[RACERS] = {
    {88.f, 11.0f, 19.f}, {101.f, 10.4f, 23.f}, {113.f, 12.1f, 21.f}, {95.f, 9.7f, 17.f}};

static struct {
    bool init;
    float level;
    RacerSource src[RACERS];
    int log_frames;
} s_racer_mix;

static float poly_blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

static void racers_mix(float* out, int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_racer_mix.level <= 0.f) {
        s_racer_mix.init = false;
        return;
    }
    if (!s_racer_mix.init) {
        for (int k = 0; k < RACERS; k++) {
            s_racer_mix.src[k] = (RacerSource){.seed = 0x9E3779B9u * (uint32_t)(k + 1), .pitch = 1.f};
            s_racer_mix.src[k].rpm = (Gust){.seed = 0x6A09E667u ^ (uint32_t)k, .value = 0.5f, .target = 0.5f};
            s_racer_mix.src[k].ph_throb = 0.25f * (float)k;
        }
        s_racer_mix.init = true;
    }
    const float level0 = s_racer_mix.level;
    s_racer_mix.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
    const float volume = pc_get_sfx_volume() * ambience_gain();
    const float duck0 = s_duck0, duck1 = s_duck;
    Racers racers;
    unsigned seq;
    do {
        seq = __atomic_load_n(&s_racers_seq, __ATOMIC_ACQUIRE);
        racers = s_racers;
    } while ((seq & 1u) != 0 || seq != __atomic_load_n(&s_racers_seq, __ATOMIC_ACQUIRE));
    const float w = 6.2831853f / (float)rate;
    const float a_lp = 1.f - expf(-w * 900.f);
    const float f_bp = 2.f * sinf(3.14159265f * 2400.f / (float)rate);
    const float inv_rate = 1.f / (float)rate;
    float mono[160];
    if (frames > 160) {
        frames = 160;
    }
    for (int k = 0; k < RACERS; k++) {
        RacerSource* r = &s_racer_mix.src[k];
        const bool here = racers.presence[k] > 0.f;
        if (here) {
            memcpy(r->pos, racers.pos[k], sizeof(r->pos));
        }
        const float rl0 = r->level;
        r->level += ((here ? racers.presence[k] : 0.f) - r->level) * (1.f - expf(-dt / 0.15f));
        if (rl0 < 1e-3f && r->level < 1e-3f) {
            r->have_dist = false;
            continue;
        }
        /* Doppler: the rate the distance changes, smoothed; a jump (a new
         * car taking the lane) starts it over. */
        float d;
        if (pc_spatial_listener_distance(r->pos, &d)) {
            const float v = r->have_dist ? (d - r->dist) / dt : 0.f;
            if (fabsf(v) > 20000.f) {
                r->vr = 0.f;
            } else {
                r->vr += (v - r->vr) * (1.f - expf(-dt / 0.08f));
            }
            r->dist = d;
            r->have_dist = true;
        }
        gust_step(&r->rpm, dt * 0.7f);
        const float want = fminf(fmaxf(RACER_DOPPLER_C / (RACER_DOPPLER_C + r->vr), 0.7f), 1.4f) *
                           (0.96f + 0.08f * r->rpm.value);
        const float p0 = r->pitch;
        r->pitch += (want - r->pitch) * (1.f - expf(-dt / 0.03f));
        const float hum_f = s_racer_voice[k].hum, whine_f = hum_f * s_racer_voice[k].whine;
        /* The four together about -32 dBFS in a flat desktop mix (the rush
         * -38 there): the racers over the wind. */
        const float amp = 0.017f * volume;
        for (int i = 0; i < frames; i++) {
            const float t = (float)i / (float)frames;
            const float pitch = p0 + (r->pitch - p0) * t;
            /* The hum: a band-limited sawtooth, rolled off. */
            const float dh = hum_f * pitch * inv_rate;
            r->ph_hum += dh;
            r->ph_hum -= floorf(r->ph_hum);
            const float saw = 2.f * r->ph_hum - 1.f - poly_blep(r->ph_hum, dh);
            r->lp1 += a_lp * (saw - r->lp1);
            r->lp2 += a_lp * (r->lp1 - r->lp2);
            r->ph_throb += s_racer_voice[k].throb * inv_rate;
            r->ph_throb -= floorf(r->ph_throb);
            const float throb = 1.f - 0.3f * (0.5f + 0.5f * sinf(6.2831853f * r->ph_throb));
            /* The whine, wavering. */
            r->ph_vib += 5.7f * inv_rate;
            r->ph_vib -= floorf(r->ph_vib);
            const float wf = whine_f * pitch * (1.f + 0.006f * sinf(6.2831853f * r->ph_vib));
            r->ph_whine += wf * inv_rate;
            r->ph_whine -= floorf(r->ph_whine);
            r->ph_whine2 += 2.01f * wf * inv_rate;
            r->ph_whine2 -= floorf(r->ph_whine2);
            const float whine = sinf(6.2831853f * r->ph_whine) + 0.35f * sinf(6.2831853f * r->ph_whine2);
            /* The air off its thrusters. */
            const float n = rnd_unit(&r->seed) * 2.f - 1.f;
            const float high = n - r->bp_low - 0.8f * r->bp_band;
            r->bp_band += f_bp * high;
            r->bp_low += f_bp * r->bp_band;
            const float lv = (level0 + (s_racer_mix.level - level0) * t) * (duck0 + (duck1 - duck0) * t) *
                             (rl0 + (r->level - rl0) * t);
            mono[i] = (1.6f * r->lp2 * throb + 0.22f * whine + 0.12f * r->bp_band) * amp * lv;
        }
        pc_stage_mix_apply(STAGE_MIX_ENGINES, &r->mix, mono, frames, 1);
        if (!pc_spatial_render_placed(k, r->pos, mono)) {
            for (int i = 0; i < frames; i++) { /* not placed (spatial audio bypassed): plain, centred */
                out[i * 2] += mono[i] * 0.7071f;
                out[i * 2 + 1] += mono[i] * 0.7071f;
            }
        }
    }
    /* MELEE_RACERS_LOG=1: the cars' distances and Doppler, twice a second. */
    static int log_on = -1;
    if (log_on < 0) {
        log_on = getenv("MELEE_RACERS_LOG") != NULL;
    }
    if (log_on && ++s_racer_mix.log_frames >= rate / frames / 2) {
        s_racer_mix.log_frames = 0;
        pc_log_line("racers: %.0f/%.0f/%.2f %.0f/%.0f/%.2f %.0f/%.0f/%.2f %.0f/%.0f/%.2f (distance/rate/pitch)",
                    s_racer_mix.src[0].dist, s_racer_mix.src[0].vr, s_racer_mix.src[0].pitch, s_racer_mix.src[1].dist,
                    s_racer_mix.src[1].vr, s_racer_mix.src[1].pitch, s_racer_mix.src[2].dist, s_racer_mix.src[2].vr,
                    s_racer_mix.src[2].pitch, s_racer_mix.src[3].dist, s_racer_mix.src[3].vr, s_racer_mix.src[3].pitch);
    }
}

/* The other craft's engines (craft_frame), slots 4-7, placed:
 *  - the hover platforms and the ball: small jets, a hiss of thrust (noise
 *    around 2.2 kHz, the ball's higher and lighter) over a little low rumble,
 *    a faint whine, fluttering as it burns; the mixer's CRAFT channel;
 *  - the Falcon Flyer: a deep rocket rumble from its rear engines (noise
 *    under 120 Hz, a 46 Hz drone and its octave, a roar around 500 Hz),
 *    surging slowly; its FLYER channel.
 * Each fades in and out as the craft comes and goes. */
typedef struct CraftSource {
    uint32_t seed;
    float level;
    float pos[3];
    Gust surge;
    Flutter flicker;
    float lp1, lp2, bp_low, bp_band, r_low, r_band;
    float ph, ph2;
    StageMixState mix;
} CraftSource;

static struct {
    bool init;
    float level;
    CraftSource src[CRAFT];
} s_craft_mix;

static void craft_mix(float* out, int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_craft_mix.level <= 0.f) {
        s_craft_mix.init = false;
        return;
    }
    if (!s_craft_mix.init) {
        for (int k = 0; k < CRAFT; k++) {
            CraftSource* c = &s_craft_mix.src[k];
            *c = (CraftSource){.seed = 0xB5297A4Du + 0x68E31DA4u * (uint32_t)k};
            c->surge = (Gust){.seed = 0x1B873593u ^ (uint32_t)k, .value = 0.6f, .target = 0.6f};
            c->flicker = (Flutter){.seed = 0xCC9E2D51u ^ (uint32_t)k, .value = 0.5f, .rate = k == 3 ? 7.f : 31.f};
        }
        s_craft_mix.init = true;
    }
    const float level0 = s_craft_mix.level;
    s_craft_mix.level = on ? fminf(1.f, level0 + dt / AMB_FADE_IN_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
    const float volume = pc_get_sfx_volume() * ambience_gain();
    const float duck0 = s_duck0, duck1 = s_duck;
    Craft craft;
    unsigned seq;
    do {
        seq = __atomic_load_n(&s_racers_seq, __ATOMIC_ACQUIRE);
        craft = s_craft;
    } while ((seq & 1u) != 0 || seq != __atomic_load_n(&s_racers_seq, __ATOMIC_ACQUIRE));
    const float w = 6.2831853f / (float)rate;
    const float inv_rate = 1.f / (float)rate;
    float mono[160];
    if (frames > 160) {
        frames = 160;
    }
    for (int k = 0; k < CRAFT; k++) {
        CraftSource* c = &s_craft_mix.src[k];
        const bool flyer = k == 3, ball = k == 2;
        if (craft.here[k]) {
            memcpy(c->pos, craft.pos[k], sizeof(c->pos));
        }
        const float cl0 = c->level;
        c->level += ((craft.here[k] ? 1.f : 0.f) - c->level) * (1.f - expf(-dt / 0.3f));
        if (cl0 < 1e-3f && c->level < 1e-3f) {
            continue;
        }
        gust_step(&c->surge, dt * (flyer ? 0.4f : 1.2f));
        const float flick = flutter_step(&c->flicker, dt);
        const float a_low = 1.f - expf(-w * (flyer ? 120.f : 160.f));
        const float f_bp = 2.f * sinf(3.14159265f * (flyer ? 500.f : ball ? 3000.f : 2200.f) / (float)rate);
        const float f_rb = 2.f * sinf(3.14159265f * (flyer ? 1100.f : 900.f) / (float)rate);
        const float tone = flyer ? 46.f : ball ? 2400.f : 1650.f;
        /* In a flat desktop mix against the stage's bed of about -30 dBFS
         * (the racers and the rush as tuned): the Flyer about -36 while it's
         * there, a platform about -40. */
        float amp;
        if (flyer) {
            amp = (0.7f + 0.3f * c->surge.value) * 0.2f * volume;
        } else {
            amp = (0.75f + 0.25f * flick) * (ball ? 0.042f : 0.055f) * volume;
        }
        for (int i = 0; i < frames; i++) {
            const float n = rnd_unit(&c->seed) * 2.f - 1.f;
            c->lp1 += a_low * (n - c->lp1);
            c->lp2 += a_low * (c->lp1 - c->lp2);
            const float high = n - c->bp_low - 0.9f * c->bp_band;
            c->bp_band += f_bp * high;
            c->bp_low += f_bp * c->bp_band;
            const float rh = n - c->r_low - 1.2f * c->r_band;
            c->r_band += f_rb * rh;
            c->r_low += f_rb * c->r_band;
            c->ph += tone * inv_rate;
            c->ph -= floorf(c->ph);
            c->ph2 += 2.f * tone * inv_rate;
            c->ph2 -= floorf(c->ph2);
            float v;
            if (flyer) {
                const float drone = (2.f * c->ph - 1.f) * 0.5f + 0.3f * sinf(6.2831853f * c->ph2);
                v = 5.f * c->lp2 + 0.25f * drone + 0.9f * c->bp_band + 0.2f * c->r_band;
            } else {
                v = 1.0f * c->bp_band + 2.5f * c->lp2 + 0.06f * sinf(6.2831853f * c->ph) + 0.2f * c->r_band;
            }
            const float t = (float)i / (float)frames;
            const float lv = (level0 + (s_craft_mix.level - level0) * t) * (duck0 + (duck1 - duck0) * t) *
                             (cl0 + (c->level - cl0) * t);
            mono[i] = v * amp * lv;
        }
        pc_stage_mix_apply(flyer ? STAGE_MIX_FLYER : STAGE_MIX_CRAFT, &c->mix, mono, frames, 1);
        if (!pc_spatial_render_placed(4 + k, c->pos, mono)) {
            for (int i = 0; i < frames; i++) {
                out[i * 2] += mono[i] * 0.7071f;
                out[i * 2 + 1] += mono[i] * 0.7071f;
            }
        }
    }
}

/* A layer heard all around (into the ambient bus), through its stage mixer
 * channel: made into a frame of its own, mixed, then added. */
typedef void (*BusLayer)(float* out, int frames, int rate, bool on);

static void bus_layer(BusLayer layer, int channel, StageMixState st[2], float* out, int frames, int rate, bool on) {
    float tmp[2 * 160];
    if (frames > 160) {
        frames = 160;
    }
    memset(tmp, 0, sizeof(float) * 2 * (size_t)frames);
    layer(tmp, frames, rate, on);
    for (int c = 0; c < 2; c++) {
        pc_stage_mix_apply(channel, &st[c], tmp + c, frames, 2);
    }
    for (int i = 0; i < 2 * frames; i++) {
        out[i] += tmp[i];
    }
}

void pc_ambience_mix(float* out, int frames, int rate) {
    static StageMixState wind[2], jungle[2], rush[2];
    if (!enabled()) {
        return;
    }
    const uint64_t stamp = __atomic_load_n(&s_stamp, __ATOMIC_ACQUIRE);
    const int kind = __atomic_load_n(&s_kind, __ATOMIC_RELAXED);
    const bool fresh = stamp != 0 && now_ns() - stamp < AMB_STALE_NS;
    float target = 1.f;
    if (!fresh) {
        __atomic_store(&s_duck_target, &target, __ATOMIC_RELAXED);
    } else if (!pc_stage_mix_listening()) {
        __atomic_load(&s_duck_target, &target, __ATOMIC_RELAXED);
    } /* the dev tools' Stage page open in the pause: heard as in the fight */
    const float step = (float)frames / (float)rate / AMB_DUCK_S;
    s_duck0 = s_duck;
    s_duck = s_duck < target ? fminf(target, s_duck + step) : fmaxf(target, s_duck - step);
    bus_layer(wind_mix, STAGE_MIX_WIND, wind, out, frames, rate, fresh && kind == AMB_WIND);
    falls_mix(out, frames, rate, fresh && kind == AMB_FALLS);
    bus_layer(jungle_mix, STAGE_MIX_JUNGLE, jungle, out, frames, rate, fresh && kind == AMB_FALLS);
    fountain_mix(out, frames, rate, fresh && kind == AMB_FOUNTAIN);
    bus_layer(rush_mix, STAGE_MIX_RUSH, rush, out, frames, rate, fresh && kind == AMB_RUSH);
    racers_mix(out, frames, rate, fresh && kind == AMB_RUSH && pc_spatial_enabled());
    craft_mix(out, frames, rate, fresh && kind == AMB_RUSH);
}
