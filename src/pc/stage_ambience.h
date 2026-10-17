/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stage ambience: sounds the stages lack, made here (docs: the expanded
 * stage sounds idea). Corneria's wind on the Great Fox's deck, heard all
 * around, and Kongo Jungle's waterfall, heard from where it falls.
 * Output only: nothing here is read back by the game, so netplay and
 * rollback never see it. MELEE_AMBIENCE_GAIN (dB) sets its level. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The launcher's Audio setting (Expanded stage sounds); MELEE_AMBIENCE
 * overrides it. They play only with spatial audio on: they are made to be
 * heard placed (pc_spatial_ambient_bus). */
void pc_ambience_set_enabled(bool on);

/* Game thread, once per drawn fight frame (cm/camera.c): which stage's
 * ambience plays. Read only. */
void pc_ambience_frame(void);

/* Any thread: the pause menu's duck (lbAudioAx_80024E84), which takes the
 * music down to 0.2; the ambience follows it, gliding. It lifts by itself
 * once no fight draws, so a match left paused can't leave the next one
 * ducked. */
void pc_ambience_set_duck(float gain);

/* Audio thread: the ambience for one AX frame of `frames` stereo samples:
 * what surrounds the listener added to `out` (interleaved, at the AX rate),
 * what stands somewhere placed there (pc_spatial_render_placed). */
void pc_ambience_mix(float* out, int frames, int rate);

#ifdef __cplusplus
}
#endif
