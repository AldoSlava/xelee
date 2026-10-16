/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Positional sound effects; see spatial_audio.h for the whole path. */
#include "compat.h"
#include "pc/spatial_audio.h"

#ifdef MELEE_SPATIAL_AUDIO

#include "pc/pc.h"
#include "pc/xr_scene.h"
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <melee/ft/forward.h>
#include <melee/ft/ftlib.h>
#include <melee/it/forward.h>
/* it/it_26B1.h, declared here: its header pulls in every fighter type. */
void it_8026B294(HSD_GObj* gobj, Vec3* pos);
#include <sysdolphin/baselib/gobj.h>

#include <phonon.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SP_VOICES 64       /* AX_VOICES */
#define SP_EMITTERS 64     /* fighters and items published per frame */
#define SP_HANDLES 256     /* netplay starts remembered by handle */
#define SP_STALE_NS 250000000ull /* positions older than this: back to plain stereo */

/* A fighter's position is at its feet; its sounds come from about the
 * middle of its body (the same lift pc/xr_scene.c gives the follow focus). */
#define SP_FIGHTER_LIFT 10.0f
/* Desktop: the fight camera is the listener, and its game units are taken
 * at the XR arena's default size (AURORA_XR_ARENA_SCALE), so a fight seen
 * from the usual camera distance sounds about as far as the tabletop stage
 * does from a seated player. */
#define SP_DESKTOP_SCALE 0.0035f
/* Distance gain: full within one stage width (170 game units, Final
 * Destination's main platform, in meters at the arena's current scale),
 * then a gentle 1 / (1 + k (d / ref - 1)) rolloff with a floor, so a
 * fighter at the far edge of a table stage is a little quieter, never lost.
 * Scaling ref with the arena keeps the balance the same at any table size. */
#define SP_REF_UNITS 170.0f
#define SP_ROLLOFF 0.5f
#define SP_MIN_GAIN 0.3f
/* The built-in HRTF (RMS-normalized) comes out about 1.2 dB under the same
 * voice mixed center-panned, measured over fights with
 * MELEE_SPATIAL_AUDIO_STATS; made up so an A/B compares placement, not level. */
#define SP_MAKEUP 1.15f

static int s_on = -1;

static bool enabled(void) {
    if (s_on < 0) {
        const char* v = getenv("MELEE_SPATIAL_AUDIO");
        s_on = v != NULL && v[0] != '\0' && v[0] != '0';
    }
    return s_on != 0;
}

static int stats_on(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("MELEE_SPATIAL_AUDIO_STATS") != NULL;
    }
    return cached;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- game thread ------------------------------------------------------ */

static const HSD_GObj* s_cur;      /* the fighter or item whose proc runs */
static bool s_override_on;         /* a deferred netplay start: its noted emitter */
static const void* s_override_gobj;
static float s_override_pos[3];
static struct {
    int32_t handle;
    const void* gobj;
    float pos[3];
} s_handles[SP_HANDLES];
static unsigned s_handle_next;
static unsigned s_starts_tagged, s_starts_plain, s_late_found, s_late_missed; /* stats */

void pc_spatial_proc(const void* gobj) {
    const HSD_GObj* g = gobj;
    if (!enabled()) {
        return;
    }
    s_cur = g != NULL && (g->classifier == HSD_GOBJ_CLASS_FIGHTER || g->classifier == HSD_GOBJ_CLASS_ITEM) ? g
                                                                                                          : NULL;
}

/* Where an emitter is now; read only. False for anything else. */
static bool emitter_pos(const HSD_GObj* g, float pos[3]) {
    Vec3 p;
    if (g == NULL || g->user_data == NULL) {
        return false;
    }
    if (g->classifier == HSD_GOBJ_CLASS_FIGHTER) {
        ftLib_GetPos((HSD_GObj*)g, &p);
        p.y += SP_FIGHTER_LIFT;
    } else if (g->classifier == HSD_GOBJ_CLASS_ITEM) {
        it_8026B294((HSD_GObj*)g, &p);
    } else {
        return false;
    }
    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
        return false;
    }
    pos[0] = p.x;
    pos[1] = p.y;
    pos[2] = p.z;
    return true;
}

bool pc_spatial_take_emitter(const void** gobj, float pos[3]) {
    *gobj = NULL;
    if (!enabled()) {
        return false;
    }
    if (s_override_on) {
        *gobj = s_override_gobj;
        memcpy(pos, s_override_pos, sizeof(s_override_pos));
    } else if (s_cur != NULL && emitter_pos(s_cur, pos)) {
        *gobj = s_cur;
    }
    if (*gobj != NULL) {
        s_starts_tagged++;
    } else {
        s_starts_plain++;
    }
    return *gobj != NULL;
}

void pc_spatial_note_handle(int32_t handle) {
    float pos[3];
    if (!enabled() || s_cur == NULL || !emitter_pos(s_cur, pos)) {
        return;
    }
    unsigned i = s_handle_next++ % SP_HANDLES;
    s_handles[i].handle = handle;
    s_handles[i].gobj = s_cur;
    memcpy(s_handles[i].pos, pos, sizeof(pos));
}

void pc_spatial_handle_begin(int32_t handle) {
    if (!enabled()) {
        return;
    }
    /* Newest first: a handle is a frame and a call index, so an old entry
     * for the same value is a frame NET_SFX_FRAMES-plus ago at the least. */
    for (unsigned k = 1; k <= SP_HANDLES; k++) {
        unsigned i = (s_handle_next - k) % SP_HANDLES;
        if (s_handles[i].gobj != NULL && s_handles[i].handle == handle) {
            s_override_on = true;
            s_override_gobj = s_handles[i].gobj;
            memcpy(s_override_pos, s_handles[i].pos, sizeof(s_override_pos));
            s_late_found++;
            return;
        }
    }
    s_late_missed++; /* started with no emitter (or noted too long ago) */
}

void pc_spatial_handle_end(void) {
    s_override_on = false;
}

/* What the game thread hands the audio thread once per drawn fight frame. */
typedef struct Snapshot {
    uint64_t stamp; /* now_ns() when published; 0: never */
    bool xr3d;      /* the fight is in 3D in a headset: the head is the listener */
    float view[3][4];
    int n;
    struct {
        const void* gobj;
        float pos[3];
    } e[SP_EMITTERS];
    int fighters, items; /* stats */
} Snapshot;

static pthread_mutex_t s_snap_mutex = PTHREAD_MUTEX_INITIALIZER;
static Snapshot s_snap;

static void collect(Snapshot* s, int plink, int* count) {
    for (HSD_GObj* g = HSD_GObjPLinkHead[plink]; g != NULL && s->n < SP_EMITTERS; g = g->next) {
        if (emitter_pos(g, s->e[s->n].pos)) {
            s->e[s->n].gobj = g;
            s->n++;
            (*count)++;
        }
    }
}

void pc_spatial_frame(const float view[3][4]) {
    static Snapshot s; /* game thread only; static to keep it off the stack */
    static uint64_t last_log;
    if (!enabled() || HSD_GObjPLinkHead == NULL) {
        return;
    }
    s.n = s.fighters = s.items = 0;
    collect(&s, HSD_GOBJ_PLINK_FIGHTER, &s.fighters);
    collect(&s, HSD_GOBJ_PLINK_ITEM, &s.items);
    memcpy(s.view, view, sizeof(s.view));
    s.xr3d = pc_xr_world_3d();
    s.stamp = now_ns();
    pthread_mutex_lock(&s_snap_mutex);
    s_snap = s;
    pthread_mutex_unlock(&s_snap_mutex);

    if (stats_on() && s.stamp - last_log >= 2000000000ull) {
        last_log = s.stamp;
        pc_log_line("spatial: game: starts tagged=%u plain=%u, rollback-late starts with emitter %u without %u;"
                    " publishing %d fighters %d items, listener %s",
                    s_starts_tagged, s_starts_plain, s_late_found, s_late_missed, s.fighters, s.items,
                    s.xr3d ? "head (XR)" : "camera");
        s_starts_tagged = s_starts_plain = s_late_found = s_late_missed = 0;
    }
}

/* ---- audio thread ----------------------------------------------------- */

/* Steam Audio's built-in HRTF cannot be made for AX's 32 kHz (iplHRTFCreate
 * fails there, checked against 4.8.1; 44.1 and 48 kHz work), so the
 * positional path runs at 48 kHz: each voice's mono frame is resampled 2:3
 * up to 240 samples, spatialized into a 48 kHz stereo bus, and the bus is
 * resampled 3:2 back down into the 32 kHz mix once per frame. Both are
 * fixed-ratio windowed-sinc polyphase filters (a quarter and three eighths
 * of a millisecond of delay). */
#define SP_RATE 48000
#define SP_FRAME 240               /* SP_RATE / 200: one 5 ms AX frame */
#define SP_AX_RATE 32000
#define SP_AX_FRAME 160
#define SP_UP_HALF 8               /* upsampler half-width, input samples */
#define SP_UP_TAPS (2 * SP_UP_HALF)
#define SP_DOWN_HALF 12            /* downsampler half-width, 48 kHz samples */
#define SP_DOWN_TAPS (2 * SP_DOWN_HALF)

typedef struct SpVoice {
    const void* gobj;
    float pos[3]; /* game units, the newest known */
    bool tagged;
    uint64_t last_frame; /* the AX frame it last rendered in; state resets after a gap */
    float hist[SP_UP_TAPS]; /* the upsampler's last input samples */
    IPLBinauralEffect bin;
    IPLDirectEffect dir;
} SpVoice;

static SpVoice s_v[SP_VOICES];
static IPLContext s_ctx;
static IPLHRTF s_hrtf;
static IPLAudioSettings s_settings;
static bool s_ready;

static float s_up[3][SP_UP_TAPS];     /* upsampler phases: offsets 0, 2/3, 1/3 */
static float s_down[2][SP_DOWN_TAPS]; /* downsampler phases: offsets 0, 1/2 */
static float s_bus[2][SP_FRAME];      /* this frame's spatialized voices, 48 kHz */
static float s_bus_hist[2][SP_DOWN_TAPS];
static bool s_bus_used;               /* anything in the bus or its history */
static int s_bus_quiet;               /* frames since the bus last had a voice */

static Snapshot s_audio_snap;   /* the audio thread's copy */
static float s_listener[3][4];  /* game units -> listener space, meters */
static float s_scale;           /* meters per game unit */
static bool s_active;           /* spatializing this AX frame */
static uint64_t s_frame;        /* AX frames since start */

/* Stats, MELEE_SPATIAL_AUDIO_STATS=1. */
static struct {
    unsigned tags, voice_frames, fallback_frames, frames;
    double energy_in, energy_out;
    uint64_t ns;
    float sample_pos[3];
    float sample_gain;
    bool sampled;
} st;

static double blackman_sinc(double x, double cutoff, double half) {
    /* cutoff: of the input's Nyquist (1: none). */
    const double pi = 3.14159265358979323846;
    if (fabs(x) >= half) {
        return 0.0;
    }
    const double t = pi * x * cutoff;
    const double s = fabs(t) < 1e-9 ? 1.0 : sin(t) / t;
    const double w = 0.42 + 0.5 * cos(pi * x / half) + 0.08 * cos(2.0 * pi * x / half);
    return s * w;
}

/* One phase: taps at distances (frac + half - 1 - t) from the output point,
 * normalized to unity gain. */
static void make_phase(float* taps, int ntaps, double frac, double cutoff) {
    const double half = ntaps / 2;
    double sum = 0.0;
    double v[64];
    for (int t = 0; t < ntaps; t++) {
        v[t] = blackman_sinc(frac + half - 1 - t, cutoff, half + 1.0);
        sum += v[t];
    }
    for (int t = 0; t < ntaps; t++) {
        taps[t] = (float)(v[t] / sum);
    }
}

static void make_filters(void) {
    /* Up: output j sits at input position j * 2/3; its fraction cycles
     * 0, 2/3, 1/3. The band is the input's own, so no cutoff below it. */
    make_phase(s_up[0], SP_UP_TAPS, 0.0, 0.95);
    make_phase(s_up[1], SP_UP_TAPS, 2.0 / 3.0, 0.95);
    make_phase(s_up[2], SP_UP_TAPS, 1.0 / 3.0, 0.95);
    /* Down: output i sits at bus position i * 3/2; fractions 0, 1/2. The
     * band is cut at 32 kHz's Nyquist, 2/3 of 48 kHz's. */
    make_phase(s_down[0], SP_DOWN_TAPS, 0.0, 2.0 / 3.0 * 0.95);
    make_phase(s_down[1], SP_DOWN_TAPS, 0.5, 2.0 / 3.0 * 0.95);
}

/* 160 samples at 32 kHz -> 240 at 48 kHz, with the voice's own history. */
static void upsample(SpVoice* v, const float* in, float* out) {
    float b[SP_UP_TAPS + SP_AX_FRAME];
    memcpy(b, v->hist, sizeof(v->hist));
    memcpy(b + SP_UP_TAPS, in, sizeof(float) * SP_AX_FRAME);
    for (int j = 0; j < SP_FRAME; j++) {
        /* Position SP_UP_HALF + j * 2/3 in b: floor and phase. */
        const int n = SP_UP_HALF + (j * 2) / 3;
        const float* taps = s_up[j % 3];
        const float* x = b + n - SP_UP_HALF + 1;
        float acc = 0.f;
        for (int t = 0; t < SP_UP_TAPS; t++) {
            acc += x[t] * taps[t];
        }
        out[j] = acc;
    }
    memcpy(v->hist, b + SP_AX_FRAME, sizeof(v->hist));
}

/* The bus, 240 samples at 48 kHz -> 160 at 32 kHz, added to `out`. */
static void downsample_bus(float* out) {
    for (int c = 0; c < 2; c++) {
        float b[SP_DOWN_TAPS + SP_FRAME];
        memcpy(b, s_bus_hist[c], sizeof(s_bus_hist[c]));
        memcpy(b + SP_DOWN_TAPS, s_bus[c], sizeof(s_bus[c]));
        for (int i = 0; i < SP_AX_FRAME; i++) {
            const int n = SP_DOWN_HALF + (i * 3) / 2;
            const float* taps = s_down[i & 1];
            const float* x = b + n - SP_DOWN_HALF + 1;
            float acc = 0.f;
            for (int t = 0; t < SP_DOWN_TAPS; t++) {
                acc += x[t] * taps[t];
            }
            out[i * 2 + c] += acc;
        }
        memcpy(s_bus_hist[c], b + SP_FRAME, sizeof(s_bus_hist[c]));
    }
}

static void log_cb(IPLLogLevel level, const char* message) {
    (void)level;
    pc_log_line("spatial: steam audio: %s", message);
}

void pc_spatial_init(int rate, int frame) {
    static bool tried;
    if (tried || !enabled()) {
        return;
    }
    tried = true;
    if (rate != SP_AX_RATE || frame != SP_AX_FRAME) {
        pc_log_line("spatial: the mixer runs at %d Hz in %d-sample frames, not %d/%d; off", rate, frame,
                    SP_AX_RATE, SP_AX_FRAME);
        return;
    }
    const uint64_t t0 = now_ns();
    IPLContextSettings cs;
    memset(&cs, 0, sizeof(cs));
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = log_cb;
#if defined(__aarch64__) || defined(__ARM_NEON)
    cs.simdLevel = IPL_SIMDLEVEL_NEON;
#else
    cs.simdLevel = IPL_SIMDLEVEL_AVX2; /* AVX-512 can throttle the clock */
#endif
    if (iplContextCreate(&cs, &s_ctx) != IPL_STATUS_SUCCESS) {
        pc_log_line("spatial: Steam Audio context failed; off");
        return;
    }
    s_settings.samplingRate = SP_RATE;
    s_settings.frameSize = SP_FRAME;
    IPLHRTFSettings hs;
    memset(&hs, 0, sizeof(hs));
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_RMS;
    IPLerror err = iplHRTFCreate(s_ctx, &s_settings, &hs, &s_hrtf);
    if (err != IPL_STATUS_SUCCESS) {
        pc_log_line("spatial: Steam Audio HRTF failed (%d); off", (int)err);
        return;
    }
    for (int i = 0; i < SP_VOICES; i++) {
        IPLBinauralEffectSettings bs = {s_hrtf};
        IPLDirectEffectSettings ds = {1};
        if (iplBinauralEffectCreate(s_ctx, &s_settings, &bs, &s_v[i].bin) != IPL_STATUS_SUCCESS ||
            iplDirectEffectCreate(s_ctx, &s_settings, &ds, &s_v[i].dir) != IPL_STATUS_SUCCESS)
        {
            pc_log_line("spatial: Steam Audio effect %d failed; off", i);
            return;
        }
    }
    make_filters();
    s_ready = true;
    pc_log_line("spatial: on, Steam Audio %d.%d.%d, HRTF binaural at %d Hz (resampled from %d) in %d-sample frames"
                " (set up in %.1f ms)",
                STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, SP_RATE, rate,
                SP_FRAME, (now_ns() - t0) / 1e6);
}

void pc_spatial_voice_tag(int voice, const void* gobj, const float pos[3]) {
    if (!s_ready || voice < 0 || voice >= SP_VOICES || gobj == NULL) {
        return;
    }
    SpVoice* v = &s_v[voice];
    v->gobj = gobj;
    memcpy(v->pos, pos, sizeof(v->pos));
    v->tagged = true;
    v->last_frame = 0; /* a new sound: state resets at its first frame */
    st.tags++;
}

void pc_spatial_voice_clear(int voice) {
    if (voice >= 0 && voice < SP_VOICES) {
        s_v[voice].tagged = false;
        s_v[voice].gobj = NULL;
    }
}

void pc_spatial_frame_begin(void) {
    s_frame++;
    s_active = false;
    if (!s_ready) {
        return;
    }
    pthread_mutex_lock(&s_snap_mutex);
    s_audio_snap = s_snap;
    pthread_mutex_unlock(&s_snap_mutex);
    const Snapshot* s = &s_audio_snap;
    if (s->stamp == 0 || now_ns() - s->stamp > SP_STALE_NS) {
        return; /* no fight drawing (menus, scene changes) */
    }
    if (s->xr3d) {
#ifdef AURORA_ENABLE_OPENXR
        if (!aurora_xr_audio_listener(s_listener)) {
            return;
        }
        s_scale = sqrtf(s_listener[0][0] * s_listener[0][0] + s_listener[1][0] * s_listener[1][0] +
                        s_listener[2][0] * s_listener[2][0]);
#else
        return;
#endif
    } else {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                s_listener[r][c] = s->view[r][c] * SP_DESKTOP_SCALE;
            }
        }
        s_scale = SP_DESKTOP_SCALE;
    }
    if (!(s_scale > 0.f) || !isfinite(s_scale)) {
        return;
    }
    s_active = true;
    /* Follow each tagged voice's emitter. One no longer published (an item
     * gone, a fighter KO'd off the lists) keeps where it was last seen. */
    for (int i = 0; i < SP_VOICES; i++) {
        SpVoice* v = &s_v[i];
        if (!v->tagged) {
            continue;
        }
        for (int k = 0; k < s->n; k++) {
            if (s->e[k].gobj == v->gobj) {
                memcpy(v->pos, s->e[k].pos, sizeof(v->pos));
                break;
            }
        }
    }
}

bool pc_spatial_voice_active(int voice) {
    if (voice < 0 || voice >= SP_VOICES || !s_v[voice].tagged) {
        return false;
    }
    if (!s_active) {
        st.fallback_frames++;
        return false;
    }
    return true;
}

void pc_spatial_render(int voice, const float* mono) {
    SpVoice* v = &s_v[voice];
    const uint64_t t0 = now_ns();
    float in[SP_FRAME], left[SP_FRAME], right[SP_FRAME];
    float* in_ch[1] = {in};
    float* out_ch[2] = {left, right};
    IPLAudioBuffer inb = {1, SP_FRAME, in_ch};
    IPLAudioBuffer outb = {2, SP_FRAME, out_ch};

    if (v->last_frame + 1 != s_frame) {
        iplBinauralEffectReset(v->bin);
        iplDirectEffectReset(v->dir);
        memset(v->hist, 0, sizeof(v->hist));
    }
    v->last_frame = s_frame;
    upsample(v, mono, in);

    /* Listener space: x right, y up, -z ahead, meters. */
    const float* p = v->pos;
    float h[3];
    for (int r = 0; r < 3; r++) {
        h[r] = s_listener[r][0] * p[0] + s_listener[r][1] * p[1] + s_listener[r][2] * p[2] + s_listener[r][3];
    }
    const float d = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    IPLVector3 dir = {0.f, 0.f, -1.f};
    if (d > 1e-4f && isfinite(d)) {
        dir.x = h[0] / d;
        dir.y = h[1] / d;
        dir.z = h[2] / d;
    }
    const float ref = SP_REF_UNITS * s_scale;
    float gain = 1.f;
    if (d > ref && isfinite(d)) {
        gain = 1.f / (1.f + SP_ROLLOFF * (d / ref - 1.f));
        if (gain < SP_MIN_GAIN) {
            gain = SP_MIN_GAIN;
        }
    }

    IPLDirectEffectParams dp;
    memset(&dp, 0, sizeof(dp));
    dp.flags = IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
    dp.distanceAttenuation = gain;
    iplDirectEffectApply(v->dir, &dp, &inb, &inb);

    IPLBinauralEffectParams bp;
    memset(&bp, 0, sizeof(bp));
    bp.direction = dir;
    bp.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
    bp.spatialBlend = 1.f;
    bp.hrtf = s_hrtf;
    iplBinauralEffectApply(v->bin, &bp, &inb, &outb);

    for (int i = 0; i < SP_FRAME; i++) {
        left[i] *= SP_MAKEUP; /* past the direct effect, whose gain tops out at 1 */
        right[i] *= SP_MAKEUP;
        s_bus[0][i] += left[i];
        s_bus[1][i] += right[i];
    }
    s_bus_used = true;
    s_bus_quiet = 0;

    if (stats_on()) {
        for (int i = 0; i < SP_AX_FRAME; i++) {
            /* The dry signal as plain stereo would have carried it: the
             * mono level on both sides (the equal-power center). */
            st.energy_in += 2.0 * (double)mono[i] * mono[i];
        }
        for (int i = 0; i < SP_FRAME; i++) {
            /* At 48 kHz: 1.5 samples per input sample. */
            st.energy_out += ((double)left[i] * left[i] + (double)right[i] * right[i]) * (2.0 / 3.0);
        }
        st.voice_frames++;
        st.ns += now_ns() - t0;
        if (!st.sampled) {
            st.sampled = true;
            memcpy(st.sample_pos, h, sizeof(h));
            st.sample_gain = gain;
        }
    }
}

void pc_spatial_frame_end(float* out) {
    if (!s_ready) {
        return;
    }
    /* The bus drains through the downsampler for its length after the last
     * voice, then stops costing anything. */
    if (s_bus_used) {
        const uint64_t t0 = now_ns();
        downsample_bus(out);
        memset(s_bus, 0, sizeof(s_bus));
        if (++s_bus_quiet > 2) {
            s_bus_used = false;
            memset(s_bus_hist, 0, sizeof(s_bus_hist));
        }
        st.ns += now_ns() - t0;
    }
    if (!stats_on()) {
        return;
    }
    st.frames++;
    if (st.frames % 400 != 0) { /* 2 s of 5 ms frames */
        return;
    }
    unsigned active = 0;
    for (int i = 0; i < SP_VOICES; i++) {
        active += s_v[i].tagged;
    }
    const double ratio = st.energy_in > 0.0 ? 10.0 * log10(st.energy_out / st.energy_in) : 0.0;
    pc_log_line("spatial: audio: %u voices tagged, %u voice-frames spatialized, %u fell back to stereo, %u tagged now;"
                " %.1f us per AX frame (%.2f%% of its 5 ms); out/in %+.1f dB; e.g. at %.2f,%.2f,%.2f m gain %.2f",
                st.tags, st.voice_frames, st.fallback_frames, active, st.ns / 1e3 / 400.0,
                st.ns / 1e9 / 400.0 / 0.005 * 100.0, ratio, st.sample_pos[0], st.sample_pos[1],
                st.sample_pos[2], st.sample_gain);
    st.tags = st.voice_frames = st.fallback_frames = 0;
    st.energy_in = st.energy_out = 0.0;
    st.ns = 0;
    st.sampled = false;
}

#endif
