/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The stage mixer; see stage_mix.h. */
#include "compat.h"
#include "pc/stage_mix.h"

#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <melee/gr/forward.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIX_CHANNELS 4
#define MIX_BANDS 3
#define MIX_OFF (-40.f)

typedef struct MixBand {
    float freq, gain, q;
} MixBand;
typedef struct MixChannel {
    float level;
    MixBand band[MIX_BANDS];
} MixChannel;

/* Each stage's channels, and the mix tuned for them on the headset (in
 * MELEE_STAGE_MIX's form; flat where there's none), which MELEE_STAGE_MIX
 * and the panel start from. */
static const struct {
    int grkind;
    const char* names[MIX_CHANNELS];
    const char* preset;
} s_stages[] = {
    {Gr_Kind_Zebes, {"ACID"}, NULL},
    {Gr_Kind_Corneria, {"WIND", "GREAT FOX", "ARWINGS"}, NULL},
    {Gr_Kind_Kongo, {"FALLS", "JUNGLE"}, NULL},
    {Gr_Kind_Izumi, {"FOUNTAIN", "JETS"}, NULL},
    /* 2026-10-17: loud against the fighters on purpose, the race being the
     * point; the engines' body and the rush's middle brought up. */
    {Gr_Kind_BigBlue,
     {"ENGINES", "RUSH", "CRAFT", "FLYER"},
     "ENGINES=6.5/190,8.0,1.30/715,4.0,1.00/1350,0.0,0.71;RUSH=6.0/205,1.5,0.71/965,6.0,1.00/3800,0.0,0.71"},
};
#define MIX_STAGES ((int)(sizeof s_stages / sizeof s_stages[0]))

static const MixChannel s_flat = {0.f, {{150.f, 0.f, 0.71f}, {1000.f, 0.f, 1.f}, {6000.f, 0.f, 0.71f}}};

static int stage_index(int grkind) {
    for (int i = 0; i < MIX_STAGES; i++) {
        if (s_stages[i].grkind == grkind) {
            return i;
        }
    }
    return -1;
}

static int channel_count(int s) {
    int n = 0;
    while (n < MIX_CHANNELS && s_stages[s].names[n] != NULL) {
        n++;
    }
    return n;
}

/* MELEE_STAGE_MIX, by stage and channel; written once by the game thread
 * before the first stage is published. */
static MixChannel s_env[MIX_STAGES][MIX_CHANNELS];

static bool name_is(const char* name, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && name[i] != '\0'; i++) {
        const char c = s[i] == '_' ? ' ' : (char)toupper((unsigned char)s[i]);
        if (c != name[i]) {
            return false;
        }
    }
    return i == n && name[i] == '\0';
}

/* Entries in MELEE_STAGE_MIX's form into s_env, over what's there. */
static void parse_mix(const char* v) {
    while (v != NULL && *v != '\0') {
        while (*v == ' ' || *v == ';') {
            v++;
        }
        const char* eq = strchr(v, '=');
        if (eq == NULL) {
            break;
        }
        MixChannel* ch = NULL;
        for (int s = 0; s < MIX_STAGES && ch == NULL; s++) {
            for (int c = 0; c < channel_count(s); c++) {
                if (name_is(s_stages[s].names[c], v, (size_t)(eq - v))) {
                    ch = &s_env[s][c];
                    break;
                }
            }
        }
        char* p = (char*)eq + 1;
        MixChannel m = s_flat;
        m.level = strtof(p, &p);
        for (int b = 0; b < MIX_BANDS && *p == '/'; b++) {
            m.band[b].freq = strtof(p + 1, &p);
            if (*p == ',') {
                m.band[b].gain = strtof(p + 1, &p);
            }
            if (*p == ',') {
                m.band[b].q = strtof(p + 1, &p);
            }
        }
        if (ch != NULL) {
            *ch = m;
        }
        while (*p != '\0' && *p != ' ' && *p != ';') {
            p++;
        }
        v = p;
    }
}

static void parse_env(void) {
    for (int s = 0; s < MIX_STAGES; s++) {
        for (int c = 0; c < MIX_CHANNELS; c++) {
            s_env[s][c] = s_flat;
        }
        parse_mix(s_stages[s].preset);
    }
    parse_mix(getenv("MELEE_STAGE_MIX"));
}

static int s_grkind = -1; /* game thread writes, audio thread reads (__atomic) */
static bool s_listening;  /* game thread writes (__atomic) */

bool pc_stage_mix_listening(void) {
    return __atomic_load_n(&s_listening, __ATOMIC_RELAXED);
}

void pc_stage_mix_frame(int grkind) {
    static int published = -2;
#ifdef AURORA_ENABLE_OPENXR
    __atomic_store_n(&s_listening, aurora_xr_dev_mix_listening(), __ATOMIC_RELAXED);
#endif
    if (published == -2) {
        parse_env();
    }
    if (grkind == published) {
        return;
    }
    published = grkind;
    __atomic_store_n(&s_grkind, grkind, __ATOMIC_RELEASE);
#ifdef AURORA_ENABLE_OPENXR
    const int s = stage_index(grkind);
    const int n = s >= 0 ? channel_count(s) : 0;
    AuroraXrMixChannel d[MIX_CHANNELS];
    for (int c = 0; c < n; c++) {
        d[c].level = s_env[s][c].level;
        for (int b = 0; b < MIX_BANDS; b++) {
            d[c].band[b] = (AuroraXrMixBand){s_env[s][c].band[b].freq, s_env[s][c].band[b].gain, s_env[s][c].band[b].q};
        }
    }
    aurora_xr_dev_mix_stage(grkind, n, n > 0 ? s_stages[s].names : NULL, d);
#endif
}

/* ---- audio thread ----------------------------------------------------- */

typedef struct Biquad {
    float b0, b1, b2, a1, a2;
} Biquad;

/* RBJ's cookbook: band 0 a low shelf, 1 a peak, 2 a high shelf. */
static Biquad biquad(int kind, const MixBand* m, float rate) {
    const float f = fminf(fmaxf(m->freq, 10.f), rate * 0.45f);
    const float q = fmaxf(m->q, 0.1f);
    const float A = powf(10.f, m->gain / 40.f);
    const float w0 = 6.2831853f * f / rate;
    const float cw = cosf(w0), alpha = sinf(w0) / (2.f * q);
    float b0, b1, b2, a0, a1, a2;
    if (kind == 1) {
        b0 = 1.f + alpha * A;
        b1 = -2.f * cw;
        b2 = 1.f - alpha * A;
        a0 = 1.f + alpha / A;
        a1 = -2.f * cw;
        a2 = 1.f - alpha / A;
    } else {
        const float sa = 2.f * sqrtf(A) * alpha;
        const float s = kind == 0 ? 1.f : -1.f; /* the high shelf: the low's, mirrored */
        b0 = A * ((A + 1.f) - s * (A - 1.f) * cw + sa);
        b1 = s * 2.f * A * ((A - 1.f) - s * (A + 1.f) * cw);
        b2 = A * ((A + 1.f) - s * (A - 1.f) * cw - sa);
        a0 = (A + 1.f) + s * (A - 1.f) * cw + sa;
        a1 = -s * 2.f * ((A - 1.f) + s * (A + 1.f) * cw);
        a2 = (A + 1.f) + s * (A - 1.f) * cw - sa;
    }
    return (Biquad){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

static struct {
    int grkind;
    int rate;
    MixChannel set[MIX_CHANNELS];
    Biquad bq[MIX_CHANNELS][MIX_BANDS];
    bool flat[MIX_CHANNELS][MIX_BANDS];
    float gain[MIX_CHANNELS], gain0[MIX_CHANNELS]; /* this frame's level, and the last frame's */
} s_mix = {.grkind = -2};

static float level_gain(float db) {
    return db <= MIX_OFF ? 0.f : powf(10.f, db / 20.f);
}

void pc_stage_mix_begin(int rate) {
    const int grkind = __atomic_load_n(&s_grkind, __ATOMIC_ACQUIRE);
    const int s = stage_index(grkind);
    const bool fresh = grkind != s_mix.grkind || rate != s_mix.rate;
    s_mix.grkind = grkind;
    s_mix.rate = rate;
    for (int c = 0; c < MIX_CHANNELS; c++) {
        MixChannel m = s >= 0 ? s_env[s][c] : s_flat;
#ifdef AURORA_ENABLE_OPENXR
        AuroraXrMixChannel x;
        if (s >= 0 && c < channel_count(s) && aurora_xr_dev_mix(grkind, c, &x)) {
            m.level = x.level;
            for (int b = 0; b < MIX_BANDS; b++) {
                m.band[b] = (MixBand){x.band[b].freq, x.band[b].gain, x.band[b].q};
            }
        }
#endif
        if (fresh || memcmp(&m, &s_mix.set[c], sizeof(m)) != 0) {
            for (int b = 0; b < MIX_BANDS; b++) {
                s_mix.bq[c][b] = biquad(b, &m.band[b], (float)rate);
                s_mix.flat[c][b] = m.band[b].gain == 0.f;
            }
            s_mix.set[c] = m;
        }
        s_mix.gain0[c] = fresh ? level_gain(m.level) : s_mix.gain[c];
        s_mix.gain[c] = level_gain(m.level);
    }
}

void pc_stage_mix_apply(int channel, StageMixState* st, float* buf, int frames, int stride) {
    if (channel < 0 || channel >= MIX_CHANNELS || frames <= 0) {
        return;
    }
    for (int b = 0; b < MIX_BANDS; b++) {
        float* z = st->z[b];
        if (s_mix.flat[channel][b]) {
            z[0] = z[1] = 0.f; /* a flat band passes as is, and starts clean when it's turned */
            continue;
        }
        const Biquad q = s_mix.bq[channel][b];
        for (int i = 0; i < frames; i++) { /* transposed direct form II */
            const float x = buf[i * stride];
            const float y = q.b0 * x + z[0];
            z[0] = q.b1 * x - q.a1 * y + z[1];
            z[1] = q.b2 * x - q.a2 * y;
            buf[i * stride] = y;
        }
        for (int k = 0; k < 2; k++) {
            if (fabsf(z[k]) < 1e-20f) {
                z[k] = 0.f; /* no denormals in a fade's tail */
            }
        }
    }
    const float g0 = s_mix.gain0[channel], g1 = s_mix.gain[channel];
    if (g0 == 1.f && g1 == 1.f) {
        return;
    }
    for (int i = 0; i < frames; i++) {
        buf[i * stride] *= g0 + (g1 - g0) * ((float)i / (float)frames);
    }
}
