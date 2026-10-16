/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A room's reverb, for sounds placed in the player's own room (mixed
 * reality; pc/spatial_audio.c): a feedback delay network sized from the
 * room, decaying faster in the highs than the lows as a furnished room does,
 * after a few early reflections off its floor, ceiling and walls, with
 * decorrelated left and right outputs so it surrounds the listener instead
 * of sitting in the head. Mono in, stereo out, any rate; plain C, no state
 * outside the struct.
 *
 * Steam Audio's parametric reverb was the first choice and is not used: in
 * 4.8.1 it is mono out (only the omni channel carries anything) and starts
 * 57.6 ms after its input whatever the decay times, as the late tail of its
 * hybrid mode (checked with an impulse). A living room's reverb starts
 * within ~10 ms. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A room: its floor's area (m²) and perimeter (m), and its height (m). */
typedef struct RoomShape {
    float floor_area, perimeter, height;
} RoomShape;

/* A furnished living room of 5 x 4 x 2.5 m, for when the headset knows
 * none (no Space Setup, or not a headset). */
extern const RoomShape room_reverb_default_room;

#define ROOM_REVERB_LINES 8
#define ROOM_REVERB_MAX_DELAY 4096 /* samples per line: 85 ms at 48 kHz */
#define ROOM_REVERB_TAPS 6         /* early reflections */
#define ROOM_REVERB_MAX_PRE 2048   /* the input's longest delay, samples */

typedef struct RoomReverb {
    /* Set up by room_reverb_init. */
    float rt60[2];      /* decay times, seconds: at low and at high frequencies */
    float pre_ms;       /* the late part's start after the input */
    int delay[ROOM_REVERB_LINES];
    float b0[ROOM_REVERB_LINES], a1[ROOM_REVERB_LINES]; /* each line's damping */
    int tap_delay[ROOM_REVERB_TAPS];
    float tap_gain[ROOM_REVERB_TAPS][2];
    int pre;            /* the late part's input delay, samples */
    float late_gain;    /* scales the late part to unit impulse energy */
    float gain;         /* the whole output, set by room_reverb_init */
    /* Running state. */
    float line[ROOM_REVERB_LINES][ROOM_REVERB_MAX_DELAY];
    float damp[ROOM_REVERB_LINES];
    float in_hist[ROOM_REVERB_MAX_PRE];
    int pos, in_pos;
    float energy;       /* a running measure of what is still ringing */
} RoomReverb;

/* Decay times (seconds) at low and high frequencies for a furnished room of
 * that shape (Eyring, with a furnished room's absorption). */
void room_reverb_times(const RoomShape* room, float rt60[2]);

/* Sets the reverb up for `room` at `rate` Hz, its output's energy for a unit
 * impulse being `wet` (power ratio; 1: as loud as what went in). Clears its
 * state. Costs one second of the reverb's own processing (it measures
 * itself): call it off the audio thread. */
void room_reverb_init(RoomReverb* r, const RoomShape* room, int rate, float wet);

/* Adds the reverb of `in` (n samples) into `left` and `right`. */
void room_reverb_process(RoomReverb* r, const float* in, float* left, float* right, int n);

/* Whether anything still rings (after input stops: about its decay time). */
bool room_reverb_ringing(const RoomReverb* r);

void room_reverb_clear(RoomReverb* r);

#ifdef __cplusplus
}
#endif
