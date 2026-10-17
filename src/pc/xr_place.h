/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Placing the stage before every fight (XR build).
 *
 * As a fight starts, the game is held on its first frame, still drawing,
 * while the player moves, turns and scales the stage with their hands; A or
 * Start begins the fight, B puts the stage back at its default placement, Y
 * shows or hides the how-to cards (open at the start of every hold).
 *
 * The hold runs no simulation ticks at all: no game state changes, nothing
 * is consumed and no frame passes, so a replay or a recording can't tell.
 * It happens only when an XR session is already presenting as the fight
 * opens, so builds without XR, and an XR build playing flat, are never
 * held. It never happens in netplay, where the peer (VR or not, this build
 * or not) expects the match to start on the agreed frame. MELEE_XR_PLACE=0
 * turns it off. */
#ifndef PC_XR_PLACE_H
#define PC_XR_PLACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AURORA_ENABLE_OPENXR
/* Once per scene-loop pass, before the pass's ticks (gm/gmscene.c). True:
 * run no ticks this pass, only draw; the pads that came in are dropped. */
bool pc_xr_place_hold(void);
/* Game thread: the stage is held for placing, or about to be (no ticks, no
 * fight yet). Only reads. */
bool pc_xr_place_holding(void);
/* A stage's fight geometry is ready in the 3D view (xr_scene.c): offer the
 * hold for it next pass. */
void pc_xr_place_stage_ready(int grkind);
/* The placement dummy scene (see xr_place.c): started when it opens; its
 * result is polled each frame (0 still placing, 1 adopted, and once
 * non-zero it is reset). Placed once this launch: decided. */
void pc_xr_place_dummy_start(void);
int pc_xr_place_dummy_result(void);
bool pc_xr_place_decided(void);
/* A headset is presenting and placing is not turned off. */
bool pc_xr_place_available(void);
/* The first fight mode of a launch should place the stage first: a headset
 * is presenting and there's no stage spot yet (none placed this launch,
 * none found in the room). */
bool pc_xr_place_needed(void);
/* The launcher's "Before each fight": true holds every fight for placing;
 * false starts it right away once a stage spot is set (this launch, or
 * found in the room), holding only while there's none. */
void pc_xr_place_set_hold(bool every_fight);
/* gm/gmscene.c: the scene now running (GameSceneKind). Sudden Death is
 * the fight just played, on its stage: no hold. */
void pc_xr_place_set_scene(int kind);
#else
static inline void pc_xr_place_set_hold(bool every_fight) { (void)every_fight; }
static inline void pc_xr_place_set_scene(int kind) { (void)kind; }
static inline void pc_xr_place_dummy_start(void) {}
static inline int pc_xr_place_dummy_result(void) { return 0; }
static inline bool pc_xr_place_decided(void) { return true; }
static inline bool pc_xr_place_available(void) { return false; }
static inline bool pc_xr_place_needed(void) { return false; }
static inline bool pc_xr_place_hold(void) { return false; }
static inline bool pc_xr_place_holding(void) { return false; }
static inline void pc_xr_place_stage_ready(int grkind) { (void)grkind; }
#endif

#ifdef __cplusplus
}
#endif

#endif
