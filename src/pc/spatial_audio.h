/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Positional sound effects (the launcher's Audio setting, on by default, or
 * MELEE_SPATIAL_AUDIO=0/1 at run time; built with the MELEE_SPATIAL_AUDIO
 * CMake option, which needs the Steam Audio SDK).
 *
 * A sound a fighter or an item starts is heard from where that fighter or
 * item is: in the XR build relative to the head, with the stage placed on
 * the table; elsewhere relative to the fight camera. Every other sound,
 * music and the reverb busses are mixed as before.
 *
 * The path, in four steps:
 *   1. Emitter. The GObj proc loop (gobj.c) names the fighter or item whose
 *      proc is running; a sound start (axdriver.c) takes that emitter and
 *      its position and keeps them beside its sound machine record.
 *   2. Voice. When the audio thread gives the record AX voices
 *      (AXDriverExec), the voices are tagged with the emitter; acquiring or
 *      freeing a voice clears its tag.
 *   3. Positions. Once per drawn fight frame (cm/camera.c) the game thread
 *      publishes every fighter's and item's position, and the camera, into
 *      a snapshot the audio thread copies once per 5 ms AX frame.
 *   4. Render. A tagged voice is mixed mono (its L/R pan dropped, its level
 *      kept) and spatialized with an HRTF and a distance gain (Steam Audio's
 *      binaural and direct effects, at 48 kHz: resampled from and back to
 *      AX's 32 kHz) into the dry mix. Its aux sends (reverb) are unchanged.
 *
 * Output only. Nothing here is read by the simulation, nothing is in a
 * rollback snapshot (this file is not in libmelee_game.a, and axdriver.c's
 * statics are excluded from it), and none of it changes which sounds start
 * or when. Netplay: an emitter rides beside the sound start, never through
 * pc/net_sfx.c's handles; a start that net_sfx.c defers to the end of a
 * rollback finds its emitter by handle in a side table here. Resimulated
 * frames start no sounds and publish no positions, so a rollback leaves
 * playing voices with their tags and the latest published positions.
 *
 * Without the CMake option every call below is an empty inline. */
#ifndef PC_SPATIAL_AUDIO_H
#define PC_SPATIAL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef MELEE_SPATIAL_AUDIO

/* Before the game starts (the launcher's Audio setting, on by default);
 * MELEE_SPATIAL_AUDIO=0/1 overrides it. */
void pc_spatial_set_enabled(bool on);

/* ---- game thread ---- */

/* Around one GObj proc (HSD_GObj_RunProcs): a fighter or item becomes the
 * current emitter, anything else (or NULL) clears it. */
void pc_spatial_proc(const void* gobj);
/* For a sound start: the current emitter and where it is now (game units).
 * False, with *gobj NULL, when there is none or the feature is off. */
bool pc_spatial_take_emitter(const void** gobj, float pos[3]);
/* Netplay: the sound start that pc/net_sfx.c handed `handle` had the current
 * emitter; remembered for a start it plays later (pc_spatial_handle_begin). */
void pc_spatial_note_handle(int32_t handle);
/* Around a start pc/net_sfx.c plays at the end of a rollback: its emitter is
 * the one noted for `handle`. */
void pc_spatial_handle_begin(int32_t handle);
void pc_spatial_handle_end(void);
/* Once per drawn fight frame, with the fight camera's view matrix (world ->
 * camera, GX Mtx layout): publishes positions and the listener. */
void pc_spatial_frame(const float view[3][4]);

/* ---- audio thread (pc/audio.c and AXDriverExec, under the audio lock) ---- */

/* AX voice `voice` now plays a sound from `gobj`, last seen at `pos`. */
void pc_spatial_voice_tag(int voice, const void* gobj, const float pos[3]);
void pc_spatial_voice_clear(int voice);
/* Once per AX frame, before the voices mix: takes the newest positions and
 * the listener. */
void pc_spatial_frame_begin(void);
/* Whether `voice` is rendered by pc_spatial_render this frame. */
bool pc_spatial_voice_active(int voice);
/* Spatializes the voice's dry mono frame (`mono`, 160 samples) into the
 * positional bus. */
void pc_spatial_render(int voice, const float* mono);
/* After the voices: adds the positional bus to the interleaved stereo mix
 * `out` (160 frames). */
void pc_spatial_frame_end(float* out);
/* Main thread, from AXInit: creates the Steam Audio objects when
 * spatial audio is on. */
void pc_spatial_init(int rate, int frame);

#else

static inline void pc_spatial_set_enabled(bool on) { (void)on; }
static inline void pc_spatial_proc(const void* gobj) { (void)gobj; }
static inline bool pc_spatial_take_emitter(const void** gobj, float pos[3]) {
    (void)pos;
    *gobj = 0;
    return false;
}
static inline void pc_spatial_note_handle(int32_t handle) { (void)handle; }
static inline void pc_spatial_handle_begin(int32_t handle) { (void)handle; }
static inline void pc_spatial_handle_end(void) {}
static inline void pc_spatial_frame(const float view[3][4]) { (void)view; }
static inline void pc_spatial_voice_tag(int voice, const void* gobj, const float pos[3]) {
    (void)voice;
    (void)gobj;
    (void)pos;
}
static inline void pc_spatial_voice_clear(int voice) { (void)voice; }
static inline void pc_spatial_frame_begin(void) {}
static inline bool pc_spatial_voice_active(int voice) {
    (void)voice;
    return false;
}
static inline void pc_spatial_render(int voice, const float* mono) {
    (void)voice;
    (void)mono;
}
static inline void pc_spatial_frame_end(float* out) { (void)out; }
static inline void pc_spatial_init(int rate, int frame) {
    (void)rate;
    (void)frame;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
