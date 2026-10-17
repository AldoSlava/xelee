/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The stage mixer: a level and a three-band parametric EQ (a low shelf, a
 * peak, a high shelf) for each of a stage's own sounds, the ones placed or
 * made here (pc/spatial_audio.c's stage sounds, pc/stage_ambience.c), to
 * tune them by ear. Set on the XR dev tools' Stage page
 * (aurora_xr_dev_mix), or, without it, by MELEE_STAGE_MIX: entries
 * separated by spaces or ';', each NAME=LEVEL[/F,G,Q/F,G,Q/F,G,Q] (dB; the
 * low shelf, the peak, the high shelf: Hz, dB, Q), NAME a channel below with
 * '_' for a space, e.g.
 *   MELEE_STAGE_MIX="ENGINES=+6/120,0,0.7/1500,4,1.2/6000,0,0.7 RUSH=-2"
 * The panel logs each change in that form. Flat (0 dB everywhere) without
 * either. Output only, like the sounds it mixes. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The channels, by stage (pc/stage_mix.c's table names them). */
enum {
    STAGE_MIX_ACID = 0, /* Brinstar */
    STAGE_MIX_WIND = 0, /* Corneria */
    STAGE_MIX_GREAT_FOX = 1,
    STAGE_MIX_ARWINGS = 2,
    STAGE_MIX_FALLS = 0, /* Kongo Jungle */
    STAGE_MIX_JUNGLE = 1,
    STAGE_MIX_FOUNTAIN = 0, /* Fountain of Dreams */
    STAGE_MIX_JETS = 1,
    STAGE_MIX_ENGINES = 0, /* Big Blue */
    STAGE_MIX_RUSH = 1,
};

/* One stream's filter state (a voice, a placed source, a bus's channel);
 * zeroed: a fresh one. */
typedef struct StageMixState {
    float z[3][2];
} StageMixState;

/* Game thread, each drawn fight frame: the stage playing (its Gr_Kind). */
void pc_stage_mix_frame(int grkind);

/* Audio thread, once each AX frame before anything mixes: takes the
 * newest settings. */
void pc_stage_mix_begin(int rate);

/* Audio thread: `frames` samples of channel `channel`'s sound, `stride`
 * apart, through its EQ and level, in place. */
void pc_stage_mix_apply(int channel, StageMixState* st, float* buf, int frames, int stride);

#ifdef __cplusplus
}
#endif
