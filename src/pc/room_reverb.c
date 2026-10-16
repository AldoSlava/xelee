/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A room's reverb; see room_reverb.h. */
#include "pc/room_reverb.h"

#include <math.h>
#include <string.h>

const RoomShape room_reverb_default_room = {20.f, 18.f, 2.5f};

#define SPEED_OF_SOUND 343.f
#define HIGH_HZ 6000.f /* where the high decay time is met */

/* A furnished living room's average absorption (sofa, rug, curtains,
 * shelves; bare walls and ceiling), low and high frequencies. */
static const float s_absorption[2] = {0.15f, 0.30f};

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void room_reverb_times(const RoomShape* room, float rt60[2]) {
    const float volume = room->floor_area * room->height;
    const float surface = 2.f * room->floor_area + room->perimeter * room->height;
    for (int b = 0; b < 2; b++) {
        /* Eyring: RT60 = 0.161 V / (-S ln(1 - a)). */
        rt60[b] = clampf(0.161f * volume / (-surface * logf(1.f - s_absorption[b])), 0.12f, 1.5f);
    }
}

/* In place: an 8-point Walsh-Hadamard transform, scaled to stay orthogonal. */
static void hadamard8(float* x) {
    for (int h = 1; h < 8; h *= 2) {
        for (int i = 0; i < 8; i += 2 * h) {
            for (int j = i; j < i + h; j++) {
                const float a = x[j], b = x[j + h];
                x[j] = a + b;
                x[j + h] = a - b;
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        x[i] *= 0.35355339f; /* 1 / sqrt(8) */
    }
}

/* Which lines feed each side, and in which sign: two rows of the Hadamard
 * matrix, so the sides are uncorrelated. The input goes in on a third. */
static const float s_out[2][ROOM_REVERB_LINES] = {
    {1, -1, 1, -1, 1, -1, 1, -1},
    {1, 1, -1, -1, 1, 1, -1, -1},
};
static const float s_in[ROOM_REVERB_LINES] = {1, 1, 1, 1, -1, -1, -1, -1};

/* The late part's line lengths for a room whose mean free path is 2.35 m
 * (the default room's), milliseconds; mutually prime once in samples. */
static const float s_delay_ms[ROOM_REVERB_LINES] = {11.3f, 13.7f, 16.1f, 18.9f, 21.4f, 24.2f, 27.5f, 30.8f};

static bool coprime(int a, int b) {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a == 1;
}

void room_reverb_clear(RoomReverb* r) {
    memset(r->line, 0, sizeof(r->line));
    memset(r->damp, 0, sizeof(r->damp));
    memset(r->in_hist, 0, sizeof(r->in_hist));
    r->pos = r->in_pos = 0;
    r->energy = 0.f;
}

void room_reverb_init(RoomReverb* r, const RoomShape* room, int rate, float wet) {
    room_reverb_clear(r);
    room_reverb_times(room, r->rt60);
    const float fs = (float)rate;
    const float volume = room->floor_area * room->height;
    const float surface = 2.f * room->floor_area + room->perimeter * room->height;
    const float mfp = 4.f * volume / surface; /* mean free path, m */

    /* Lines scaled with the room (gently: short lines ring metallic). */
    const float k = clampf(sqrtf(mfp / 2.35f), 0.6f, 1.8f);
    for (int i = 0; i < ROOM_REVERB_LINES; i++) {
        int d = (int)lrintf(s_delay_ms[i] * k * fs / 1000.f) | 1;
        if (d >= ROOM_REVERB_MAX_DELAY) {
            d = (ROOM_REVERB_MAX_DELAY - 1) | 1;
        }
        for (bool ok = false; !ok;) {
            ok = true;
            for (int j = 0; j < i && ok; j++) {
                ok = coprime(d, r->delay[j]);
            }
            if (!ok) {
                d += 2;
            }
        }
        r->delay[i] = d;
        /* Each pass through a line loses its share of 60 dB per decay
         * time (Jot): at DC by the low decay time, at 6 kHz by the high
         * one, through a one-pole lowpass b0 / (1 - a1 z^-1). With
         * b0 = g0 (1 - a1), its gain at w is g0 (1 - a) / |1 - a e^-jw|;
         * for that to be gh, with r = (gh / g0)^2 and c = cos w:
         * (1 - r) a^2 - 2 (1 - r c) a + (1 - r) = 0, the root under 1. */
        const float g0 = powf(10.f, -3.f * (float)d / (r->rt60[0] * fs));
        const float gh = powf(10.f, -3.f * (float)d / (r->rt60[1] * fs));
        const float ratio = (gh / g0) * (gh / g0);
        const float c = cosf(6.2831853f * fminf(HIGH_HZ, 0.45f * fs) / fs);
        float a = 0.f;
        if (ratio < 1.f) {
            const float p = 1.f - ratio * c, q = 1.f - ratio;
            a = (p - sqrtf(fmaxf(p * p - q * q, 0.f))) / q;
        }
        r->a1[i] = a;
        r->b0[i] = g0 * (1.f - a);
    }

    /* The late part starts about one mean free path after the sound. */
    r->pre = (int)lrintf(clampf(mfp / SPEED_OF_SOUND, 0.003f, 0.025f) * fs);
    if (r->pre >= ROOM_REVERB_MAX_PRE) {
        r->pre = ROOM_REVERB_MAX_PRE - 1;
    }
    r->pre_ms = 1000.f * (float)r->pre / fs;

    /* Early reflections, first order, off a box of the room's area, a
     * quarter longer than wide, and height, for a sound on a table about
     * 0.7 m ahead of a seated listener: the extra path each takes over the
     * direct sound, and the side it mostly arrives from. Approximate: where
     * the player sits in the room isn't known. */
    const float width = sqrtf(room->floor_area / 1.25f);
    const float depth = room->floor_area / width;
    const float h = room->height;
    const struct {
        float extra; /* m */
        float side;  /* -1 left, 1 right, 0 both */
    } refl[ROOM_REVERB_TAPS] = {
        {1.4f, 0.f},                        /* the table and floor below */
        {2.f * (h - 1.1f), 0.f},            /* the ceiling */
        {2.f * (0.5f * width - 0.3f), -1.f}, /* the wall to the left */
        {2.f * (0.5f * width + 0.3f), 1.f},  /* the wall to the right, a little farther */
        {2.f * (0.5f * depth - 0.4f), 0.f}, /* the wall ahead */
        {2.f * (0.5f * depth + 0.4f), 0.f}, /* the wall behind */
    };
    const float reflect = sqrtf(1.f - 0.5f * (s_absorption[0] + s_absorption[1]));
    for (int t = 0; t < ROOM_REVERB_TAPS; t++) {
        const float extra = clampf(refl[t].extra, 0.3f, 12.f);
        int d = (int)lrintf(extra / SPEED_OF_SOUND * fs);
        r->tap_delay[t] = d < ROOM_REVERB_MAX_PRE ? d : ROOM_REVERB_MAX_PRE - 1;
        /* Spherical spreading against the 0.7 m direct path. */
        const float g = reflect * 0.7f / (0.7f + extra);
        r->tap_gain[t][0] = g * (refl[t].side > 0.f ? 0.45f : 1.f);
        r->tap_gain[t][1] = g * (refl[t].side < 0.f ? 0.45f : 1.f);
    }

    /* Measure: the late part's energy for a unit impulse, for late_gain;
     * then the whole output's, early reflections and all, for `gain`. */
    r->late_gain = 1.f;
    r->gain = 1.f;
    float early_energy = 0.f;
    for (int t = 0; t < ROOM_REVERB_TAPS; t++) {
        early_energy += 0.5f * (r->tap_gain[t][0] * r->tap_gain[t][0] + r->tap_gain[t][1] * r->tap_gain[t][1]);
    }
    for (int t = 0; t < ROOM_REVERB_TAPS; t++) {
        r->tap_gain[t][0] = 0.f; /* the late part alone, for now */
        r->tap_gain[t][1] = 0.f;
    }
    double late = 0.0;
    float in[256], left[256], right[256];
    for (int done = 0; done < rate; done += 256) {
        memset(in, 0, sizeof(in));
        in[0] = done == 0 ? 1.f : 0.f;
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        room_reverb_process(r, in, left, right, 256);
        for (int i = 0; i < 256; i++) {
            late += 0.5 * ((double)left[i] * left[i] + (double)right[i] * right[i]);
        }
    }
    r->late_gain = late > 0.0 ? (float)(1.0 / sqrt(late)) : 1.f;
    /* The early reflections back, at their energy against a late part of
     * one, and the whole scaled to `wet`. */
    for (int t = 0; t < ROOM_REVERB_TAPS; t++) {
        const float extra = clampf(refl[t].extra, 0.3f, 12.f);
        const float g = reflect * 0.7f / (0.7f + extra);
        r->tap_gain[t][0] = g * (refl[t].side > 0.f ? 0.45f : 1.f);
        r->tap_gain[t][1] = g * (refl[t].side < 0.f ? 0.45f : 1.f);
    }
    r->gain = sqrtf(wet / (1.f + early_energy));
    room_reverb_clear(r);
}

void room_reverb_process(RoomReverb* r, const float* in, float* left, float* right, int n) {
    float energy = r->energy;
    for (int i = 0; i < n; i++) {
        r->in_hist[r->in_pos] = in[i];
        /* Early reflections: taps on the input's history. */
        float el = 0.f, er = 0.f;
        for (int t = 0; t < ROOM_REVERB_TAPS; t++) {
            const float x = r->in_hist[(r->in_pos - r->tap_delay[t] + ROOM_REVERB_MAX_PRE) % ROOM_REVERB_MAX_PRE];
            el += x * r->tap_gain[t][0];
            er += x * r->tap_gain[t][1];
        }
        const float x = r->in_hist[(r->in_pos - r->pre + ROOM_REVERB_MAX_PRE) % ROOM_REVERB_MAX_PRE];
        r->in_pos = (r->in_pos + 1) % ROOM_REVERB_MAX_PRE;

        /* The late part: each line's output, damped, mixed and fed back. */
        float o[ROOM_REVERB_LINES];
        float ll = 0.f, lr = 0.f;
        for (int k = 0; k < ROOM_REVERB_LINES; k++) {
            const float y = r->line[k][(r->pos - r->delay[k] + ROOM_REVERB_MAX_DELAY) % ROOM_REVERB_MAX_DELAY];
            r->damp[k] = r->b0[k] * y + r->a1[k] * r->damp[k];
            o[k] = r->damp[k];
            ll += s_out[0][k] * o[k];
            lr += s_out[1][k] * o[k];
        }
        hadamard8(o);
        for (int k = 0; k < ROOM_REVERB_LINES; k++) {
            r->line[k][r->pos] = o[k] + s_in[k] * x;
        }
        r->pos = (r->pos + 1) % ROOM_REVERB_MAX_DELAY;

        const float sl = (el + ll * r->late_gain * 0.35355339f) * r->gain;
        const float sr = (er + lr * r->late_gain * 0.35355339f) * r->gain;
        left[i] += sl;
        right[i] += sr;
        /* About a 20 ms window: rings while anything is in or out. */
        energy += 0.001f * (sl * sl + sr * sr + x * x - energy);
    }
    r->energy = energy;
}

bool room_reverb_ringing(const RoomReverb* r) {
    return r->energy > 1e-10f; /* -100 dB */
}
