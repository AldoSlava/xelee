/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stage ambience; see stage_ambience.h. */
#include "compat.h"
#include "pc/stage_ambience.h"

#include "pc/pc.h"
#include "pc/spatial_audio.h"
#include <melee/gr/ground.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Without a drawn fight frame for this long, the ambience fades out. */
#define AMB_STALE_NS 250000000ull
#define AMB_FADE_S 1.0f /* in or out, full scale */
#define AMB_DUCK_S 0.15f /* the pause duck's glide, full scale */

enum { AMB_NONE, AMB_WIND, AMB_FALLS };

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

void pc_ambience_frame(void) {
    if (!enabled()) {
        return;
    }
    const int kind = stage_info.grkind == Gr_Kind_Corneria ? AMB_WIND
                     : stage_info.grkind == Gr_Kind_Kongo  ? AMB_FALLS
                                                           : AMB_NONE;
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
    s_wind.level = on ? fminf(1.f, level0 + step) : fmaxf(0.f, level0 - step);

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

static void falls_mix(int frames, int rate, bool on) {
    const float dt = (float)frames / (float)rate;
    if (!on && s_falls.level <= 0.f) {
        s_falls.init = false;
        return;
    }
    if (!s_falls.init) {
        falls_init();
    }
    const float level0 = s_falls.level;
    s_falls.level = on ? fminf(1.f, level0 + dt / AMB_FADE_S) : fmaxf(0.f, level0 - dt / AMB_FADE_S);
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
         * little over the wind, the waterfall being right there. */
        const float amp = (0.75f + 0.25f * f->swell.value) * 0.066f * volume;
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
        pc_spatial_render_placed(c, s_falls_pos[c], mono);
    }
}

void pc_ambience_mix(float* out, int frames, int rate) {
    if (!enabled()) {
        return;
    }
    const uint64_t stamp = __atomic_load_n(&s_stamp, __ATOMIC_ACQUIRE);
    const int kind = __atomic_load_n(&s_kind, __ATOMIC_RELAXED);
    const bool fresh = stamp != 0 && now_ns() - stamp < AMB_STALE_NS;
    float target = 1.f;
    if (!fresh) {
        __atomic_store(&s_duck_target, &target, __ATOMIC_RELAXED);
    } else {
        __atomic_load(&s_duck_target, &target, __ATOMIC_RELAXED);
    }
    const float step = (float)frames / (float)rate / AMB_DUCK_S;
    s_duck0 = s_duck;
    s_duck = s_duck < target ? fminf(target, s_duck + step) : fmaxf(target, s_duck - step);
    wind_mix(out, frames, rate, fresh && kind == AMB_WIND);
    falls_mix(frames, rate, fresh && kind == AMB_FALLS);
}
