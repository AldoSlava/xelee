/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stage ambience; see stage_ambience.h. */
#include "compat.h"
#include "pc/stage_ambience.h"

#include "pc/pc.h"
#include <melee/gr/ground.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Without a drawn fight frame for this long, the ambience fades out. */
#define AMB_STALE_NS 250000000ull
#define AMB_FADE_S 1.0f /* in or out, full scale */

enum { AMB_NONE, AMB_WIND };

static uint64_t s_stamp; /* game thread writes, audio thread reads (__atomic) */
static int s_kind;

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

void pc_ambience_frame(void) {
    if (!enabled()) {
        return;
    }
    const int kind = stage_info.grkind == Gr_Kind_Corneria ? AMB_WIND : AMB_NONE;
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

void pc_ambience_mix(float* out, int frames, int rate) {
    if (!enabled()) {
        return;
    }
    const uint64_t stamp = __atomic_load_n(&s_stamp, __ATOMIC_ACQUIRE);
    const int kind = __atomic_load_n(&s_kind, __ATOMIC_RELAXED);
    const bool on = kind == AMB_WIND && stamp != 0 && now_ns() - stamp < AMB_STALE_NS;
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
    s_wind.level = on ? fminf(1.f, level0 + step) : fmaxf(0.f, level0 - step);

    gust_step(&s_wind.shared, dt);
    const float volume = pc_get_sfx_volume() * ambience_gain();
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
        /* About -33 dBFS on average and -28 in gusts, a fight's mix running
         * about -19: under the fight, there between its sounds. */
        const float amp = (0.25f + 0.75f * g * g) * 0.20f * volume;
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
            const float lv = level0 + (s_wind.level - level0) * ((float)i / (float)frames);
            out[i * 2 + c] += (rush + whistle) * amp * lv;
        }
    }
}
