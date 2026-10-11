# XR frame rate: getting off 60-on-120

Status: plan (2026-10-11). Nothing here is built yet.

## The problem

The Quest build shows the game at 120 Hz with lock-step pacing: each game
frame starts on a tick and its 3D image is shown for exactly two display
frames. Head motion between those two display frames is covered only by
the runtime's rotational timewarp. With the system's experimental
Positional TimeWarp (Horizon OS v83, a setting players have to turn on) it
feels fine; without it, every other display frame is positionally stale,
which reads as judder whenever the head translates. We can't count on
players turning that setting on.

Two things are mixed together in "60 on 120":

1. **Head-motion staleness:** the eyes are drawn from a head pose 8 ms old
   on every second display frame. This is the bad feeling.
2. **Object-motion rate:** fighters move 60 times a second. Even at 2:2
   cadence that is smooth enough; it's what the game does on a TV.

Anything that fixes (1) fixes the complaint. (2) is a bonus.

## Measured (2026-10-11, Quest 3, two-CPU Battlefield, mixed reality)

- **Compositor GPU time per display frame** (VrApi `TW=`): 2.43 ms at
  120 Hz, 2.34 ms at 72 Hz. It's about constant per display frame, so the
  rate decides its share: about 292 ms of every second at 120 Hz against
  about 168 at 72.
- **Extensions offered** (`AURORA_XR_LOG_EXTENSIONS=1`):
  `XR_FB_space_warp` v2, `XR_EXT_frame_synthesis` v1 (the cross-vendor
  successor), `XR_KHR_composition_layer_depth` v6, and
  `XR_META_temporal_pixel_synthesis` v1 (not yet looked into).
- **72 Hz today** (`AURORA_XR_REFRESH=72`, game on its own timer, no
  re-rendering): 4.9 stale frames a second against 0.45 at 120 Hz, as
  expected without lock-step.

## The GPU budget at 72 Hz with every display frame rendered

Per second, with about 2 ms of once-per-tick work (shadows, HUD) that
doesn't re-render:

| | 60 on 120 (today) | 72 on 72 |
|---|---|---|
| Compositor | 120 x 2.4 = ~290 ms | 72 x 2.35 = ~170 ms |
| Left for the app | ~710 ms | ~830 ms (+17%) |
| Once-per-tick work | 60 x 2 = 120 ms | 60 x 2 = 120 ms |
| Eye renders | 60 | 72 (+20%) |
| Eye budget per render | (710 - 120) / 60 = ~9.8 ms | (830 - 120) / 72 = ~9.9 ms |

The compositor time saved pays for the extra eye renders almost exactly.
What's tighter at 72 is the rest: each render must fit between releases
13.9 ms apart (16.7 today), and with interpolation every render is
different, so every one needs its own encode and Dawn translation (about
2.5 ms of render worker, 72 times a second: roughly 40% more render-worker
work, the thread behind most spikes).

## Option A: Application SpaceWarp at 60 -> 120 (recommended first)

Meta's Application SpaceWarp (`XR_FB_space_warp`) is built for exactly our
shape: the app renders at half the display rate and gives the runtime each
frame's depth and per-pixel motion vectors; the runtime synthesizes the
in-between frames. Meta enables positional timewarp automatically for
AppSW apps, without the system setting. So AppSW with *zero* motion
vectors is, in effect, positional timewarp we control.

- **Meta's guidance** ([Application SpaceWarp, Unity docs](https://developers.meta.com/vr/documentation/unity/os-app-spacewarp/)):
  motion vectors *and the depth AppSW uses* come from a separate low-res
  pass, not the eye buffer: "We also collect the depth buffer of the
  motion vector pass, which is the same resolution as the motion vectors"
  (368x400 by default against Quest 2's 1440x1584 eyes; "a full-res MV
  buffer would cause an order of magnitude more GPU"). Opaque draws only:
  AppSW "only supports each pixel moving in one direction", so translucent
  objects stay out (distant particles are fine). About 18 MB on Quest 3.
  Synthesis is extrapolation, then positional timewarp from that depth;
  Phase Sync turns on automatically. Logcat shows `ASW=..., Type=App`
  while it's active; `debug.oculus.spaceWarpDebug` and
  `debug.oculus.MVOverlay` show draws with wrong motion vectors. Controller
  input latency can be higher. Mixed reality isn't covered.
- **A1: a low-res depth and motion-vector pass, zero motion vectors (days).**
  - Check `XR_FB_space_warp` is offered on Quest 3; read
    `XrSystemSpaceWarpPropertiesFB` for the recommended motion-vector size.
  - Depth and motion-vector swapchains at the runtime's recommended
    motion-vector size (multiview, two layers), imported into Dawn like
    the color images.
  - A low-res pass after the eyes: the frame's opaque world draws (depth
    writes on) replayed once more with the eye views into that depth and
    motion-vector pair, writing zero motion. Same replay machinery as the
    eyes; the pixels are trivial at that size, the vertex work about the
    eye pass's vertex share. The eye pass's own depth stays discarded.
  - `XrCompositionLayerSpaceWarpInfoFB` chained to each projection view:
    the motion-vector and depth sub-images, `appSpaceDeltaPose` (identity,
    or the arena's movement while the player drags it), depth range and
    near/far. Our depth is reversed Z with an infinite far plane. The
    sibling depth specs (`XR_KHR_composition_layer_depth`,
    `XR_EXT_frame_synthesis`) allow `nearZ > farZ` for reversed depth and
    either one infinite; confirm the FB struct takes the same.
  - Also check whether the Quest runtime offers `XR_EXT_frame_synthesis`,
    the cross-vendor extension for the same thing; prefer it if so.
  - Pacing: with AppSW the runtime runs the app at half rate (60), so the
    XR loop's `xrWaitFrame` returns at 60 and every layer (HUD, screen,
    lasers) updates at 60. Lock-step becomes one display frame per game
    frame from the app's side; fixed-latency presentation, the latency
    picker and the release-order logic all simplify (the image is released
    in the same loop it's due).
  - **Risks to check on device first:** AppSW over passthrough with our
    premultiplied-alpha projection layer (synthesized frames must warp
    alpha too, or edges will halo); translucent effects with no depth
    (holograms over the room) warp by the depth behind them; disocclusion
    smear at the arena's edges. If AppSW misbehaves in mixed reality,
    stop here and go to option B.
  - Cost: the depth store (bandwidth, about 0.3-0.5 ms on the GPU at
    scale 1.0), the motion-vector clear, the runtime's synthesis (its own
    GPU time, which dynamic resolution's compositor allowance must take
    in).
- **Costs A1 adds** (beyond the above):
  - The runtime's synthesis on every synthesized display frame (half of
    them). It's more than a timewarp; Meta presents it as cheaper than
    rendering a frame, not free. VrApi's compositor time will show it, and
    dynamic resolution's compositor allowance has to take it in.
  - The low-res pass: one more replay of the opaque world draws, on the
    GPU mostly vertex work, and on the render worker another encode and
    Dawn translation (roughly 1-1.5 ms a game frame), the thread behind
    most spikes. Recording it in the same encoder as the eyes, right
    after them, keeps it to one submit.
  - The XR loop at 60: HUD, laser pointers, hand ghosts and the screen
    update at 60 instead of 120 (world-locked quads are reprojected, but
    pointer responsiveness changes).
  - Content that writes no depth (translucent effects over the room) is
    reprojected at the depth behind it: a slight wobble in head motion.
  - Dynamic resolution's partial render area: depth and motion-vector
    sub-images follow `imageRect`.
- **With zero motion vectors** the runtime treats everything as static and
  corrects only head motion (with parallax from depth): positional
  timewarp. Moving things (fighters, scrolling stages, follow windows) hold
  where they were for the synthesized frame, the same 2:2 cadence as now.
  Nothing gets worse; whether it behaves over passthrough is the thing to
  test.
- **A1 spike, step by step:**
  1. Enable `XR_EXT_frame_synthesis` (else `XR_FB_space_warp`) behind
     `AURORA_XR_APPSW`; read its system properties for the recommended
     motion-vector image size.
  2. Swapchains at the recommended motion-vector size, two layers each:
     depth (one of the runtime's depth formats from
     `xrEnumerateSwapchainFormats`,
     `XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`) and motion vectors
     (RGBA16F). Imported into Dawn as the color images are
     (`CreateSharedTextureMemoryFromVkImage`); they follow the color image
     through acquire, wait and release.
  3. The low-res pass, recorded right after the eyes: the opaque world
     draws (depth writes on) replayed with the eye views, writing depth and
     zero motion. Multiview pipeline variants for this target layout (one
     more set to build and warm).
  4. Chain the synthesis info on each projection view: the motion-vector
     and depth sub-images (their rects scaled with dynamic resolution's),
     `appSpaceDeltaPose` identity, `minDepth` 0, `maxDepth` 1, and for our
     reversed infinite depth `nearZ = +inf`, `farZ` = our near
     plane in meters), nearZ > farZ meaning reversed.
  5. Pacing: check that the runtime halves the app's rate
     (`predictedDisplayPeriod` doubling in `xrWaitFrame`). Lock-step then
     runs at one display frame per game frame; fixed-latency presentation
     and the latency picker follow `g_displayPerGameFrame`.
  6. Measure and judge (logcat's `ASW=120, Type=App` confirms it's on):
     VrApi stale and compositor time against today,
     app GPU time, the feel of head translation (with the system PTW
     setting off), the arena's edges over passthrough, translucent effects.
     A/B with `AURORA_XR_APPSW=0`.
- **A2: real motion vectors.** Fighters, items, moving stage pieces and
  scrolling stages then move at 120 in the synthesized frames too. Design
  below ("Motion vectors").

## Motion vectors (A2 design)

A motion vector is a pixel's movement in normalized device coordinates
from the previous app frame to this one. Meta's implementations compute it
per vertex: the vertex shader transforms each vertex with this frame's
matrices and with last frame's, and the fragment shader writes the
difference. We need, for every opaque world draw, last frame's version of
every matrix in its chain.

### The chain

A world vertex reaches an eye as

    clip = P_eye · V_eye · A · X · V_game^-1 · M_pos · v

- `M_pos`: the GX position matrix the vertex uses (per draw, or per vertex
  through `PNMTXIDX` for skinned fighters, up to ten slots), model to game
  view. It includes the game camera.
- `V_game^-1`: undoes the game camera.
- `X`: a moving piece's extra placement (`aurora_xr_world_transform`;
  identity for most draws).
- `A`: the arena (where the player put it, and the stage center that
  follow windows slide).
- `P_eye · V_eye`: the eye.

`clip_prev` is the same chain with last frame's `M_pos`, `V_game`, `X`,
`A` and eye. Head motion, arena grabs, follow-window scrolls, game camera
moves and object motion all fall out of the one difference. (Whether
AppSW wants head motion left in the vectors, as Meta's engines do, or
taken out through `appSpaceDeltaPose`: to confirm against the extension's
spec before building.)

### Draw identity

The hard part, as in melee-unlocked: which of last frame's draws is this
one? Ours can be exact.

- Every mesh the game draws goes through `pc_xr_dobj_begin(jobj,
  dobj_index)` (`displayfunc.c`). It sends an identity through the FIFO, a
  new `GX_AURORA_XR_DRAW_ID` command like the world/HUD category ones:
  the joint's address and the mesh index.
- One mesh can be several GX draws (its primitives): the command
  processor numbers them per identity per frame, so each draw's id is
  (joint, mesh, n).
- Address reuse (a joint freed and another allocated there): pairs must
  also agree on vertex count and primitive, as melee-unlocked requires.
- `DrawData` gains the id (8 bytes; the record has about that much spare
  at 192).
- Draws without one (particles, effects, anything not drawn through
  `HSD_JObjDisp`): zero motion. Most are translucent and stay out of the
  pass anyway.

### Last frame's matrices

- The FIFO thread keeps, per id, last frame's matrices: the position
  matrix slots the draw read (a mask of used slots), plus `V_game` and the
  moving-piece transform index. About 600 world draws a frame; the lookup
  can use the same next-in-order prediction as the decoded-vertex cache.
- At each paired draw it pushes last frame's matrices into this frame's
  uniform buffer in the same layout as the current ones, and the draw
  records that range (`DrawData::prevUniformRange`, 8 bytes: the inline
  payload goes from 192 to 208). No pair: the current range, which means
  zero motion.
- The XR side keeps last frame's arena, moving-piece transforms and eye
  matrices for the pass's second eye uniform.
- Cuts: a pair whose delta is implausible holds at zero motion.
  melee-unlocked's thresholds (40 units or 1.2 rad) are a start; a fighter
  turning around (a mirrored scale) is one too.

### The pass and its shaders

- Low-res multiview pass after the eyes, at the runtime's recommended
  motion-vector size: the opaque world draws (depth writes on, no
  blending), into the depth and motion-vector swapchains.
- A position-only shader variant: the vertex stage is the GX position path
  (vertex fetch, position matrices, the eye chain) done twice; no
  lighting, no texture coordinates, no TEV. The fragment stage writes
  `ndc - ndc_prev` (and the depth difference) to RGBA16F. Generated from
  the same code as the GX shaders, so it's keyed only by vertex format and
  matrix use: far fewer pipelines than the eye pass, quick to warm.
- Cost: on the FIFO thread, the lookups and about 50 bytes of extra
  uniforms per paired draw; on the render worker, the pass's encode and
  translation (roughly 1-1.5 ms a game frame); on the GPU, vertex work at
  a few pixels per triangle.

### What it won't catch

- Vertices the game rewrites on the CPU (rebuilt streams, shape
  animation) without changing matrices: those parts move at 60.
- Translucent effects: left out of the pass (AppSW moves each pixel one
  way), so they move at 60 too, positioned by depth behind them.
- Extrapolation overshoot: a hit, a direction change or a landing shows
  a frame's worth of the old motion in the synthesized frame. That's
  AppSW's nature; it's why Meta says to switch it off where it falls
  short.

### Order of work

1. A1: the low-res pass and swapchains with zero motion, position-only
   pipelines included. Feel-test head motion over passthrough.
2. Identity: the FIFO command, per-draw ids, a desktop log of pairing
   rates (paired, unpaired, cut) per stage.
3. Last frame's matrices and the two-chain shader: real vectors.
4. Debugging: a desktop dump of the motion-vector image as color, and
   Meta's overlays (`debug.oculus.spaceWarpDebug`,
   `debug.oculus.MVOverlay`) on the headset.
5. Tuning: cut thresholds, which draws go in the pass, per-stage checks
   (scrolling stages, follow windows, Pokémon Stadium's transformations).

## Option B: 72 Hz, eyes re-rendered every display frame

What was asked for: display at 72 Hz and render 72 distinct frames a
second. The game still simulates at 60 (it has to: game speed, netplay).

- **Pacing.** 72/60 = 1.2 display frames per game frame. Two choices:
  - The game on its own 60 Hz timer, as today for rates that aren't a
    multiple of 60 (`update_pacing`, `per = 0`). Simple, but tick phase
    drifts against the display.
  - Ticks driven from the display in a 5-of-6 pattern (`aurora_xr_pace`
    ticks on five display frames out of every six): exactly 60 a second,
    locked to the display, so every tick's phase is known. Preferred.
- **Eyes every display frame.** Each display frame renders the eyes from
  the newest game frame with that display frame's head pose. One display
  frame in six has no new game frame, so its eyes re-render the previous
  one. That removes head-motion staleness entirely (every display frame is
  fresh), with no runtime reprojection needed.
- **Decoupling (the big change).** Today a frame packet's GPU data (the
  vertex, index, uniform and storage pools, the staging buffers) is
  recycled once the frame is submitted. Re-rendering a game frame's eyes
  means keeping the newest packet's world passes and pools alive until the
  next game frame's eyes are done: double-buffered pools, and a
  "re-render the eyes" work item the XR thread queues on display frames
  with no new game frame. The deferred-eyes path (record into an image
  before it's free, flush when it is) carries over as is.
- **The release-order rule still holds.** Quest's runtime marks each
  release on the GPU, and work queued ahead of it delays it (docs/quest-xr.md,
  "Release order"). At 72 Hz there's a release every display frame, so
  each eye render must fit the 13.9 ms between two releases.
- **GPU budget: the hard part.** 13.9 ms a display frame, less the
  compositor's ~2 ms, leaves about 11.5 ms for the eyes, 72 times a second.
  Today's heavy stages take 10-12 ms of eye pass at scale 1.0 once per game
  frame (four-CPU Stadium rock, Mute City, Brinstar Depths). Eyes 72 times
  a second instead of 60 is 20% more GPU, so those stages would not hold
  1.0 at 72 Hz. Either the floor drops below 1.0 at 72 Hz, or the eye pass
  gets about 20% cheaper first (fewer draws: merging, instancing;
  cheaper shaders). That's the real cost of this option.
- **CPU:** each re-render is another eye encode and Dawn translation on
  the render worker, about 2.5 ms, on one display frame in six.
- **Everything keyed to "display frames per game frame"** (fixed-latency
  presentation, the latency picker, dynamic resolution's margins, the pace
  probe) moves to a per-display-frame model. Simpler in the end: every
  display frame renders and releases once.
- **What it looks like:** head motion and passthrough at 72 (vs 120
  today), objects at 60 with one repeat in six, a 12 Hz beat in fighter
  motion. Worth judging on the headset before building it (below).

## Option C: B plus sub-frame interpolation

melee-unlocked keeps the simulation at 60 and fills in display frames
between ticks from the game's own data (`port/runtime/gx/subframe.{h,cpp}`,
`authored_pose.{h,cpp}`). Its solver pairs each draw with the previous
tick's by identity (guarded against address reuse, same vertex count and
primitive), takes each position matrix's delta `D = M_cur * M_prev^-1`,
and applies a fraction of it: interpolating (exact, one tick of latency)
or extrapolating (no latency, overshoots on direction changes). Cuts and
implausible deltas (past 40 units or 1.2 rad) hold the latest pose;
rebuilt particle streams blend vertices; an "authored" mode samples
animation tracks forward instead. A lot of its code is flicker edge cases.

For us:

- **Hook point:** the eye replay already re-encodes recorded draws, so
  per-draw matrix overrides fit there.
- **Draw identity is easier for us:** `pc_xr_jobj_begin` /
  `pc_xr_dobj_begin` already see every joint and mesh drawn; a tag through
  the FIFO (like the world/HUD/hidden categories) gives each draw an exact
  identity (joint, mesh index) instead of heuristic pairing.
- **Latency:** interpolation adds a tick (16.7 ms) on top of fixed-latency
  presentation, which already holds frames; extrapolation costs nothing
  but overshoots.
- **Scope:** weeks, mostly edge cases (particles, rebuilt streams, cuts,
  hit-lag freezes).

It's also what A2's motion vectors need: the same pairing gives each
draw's previous matrices.

## Recommendation

1. **Feel-test 72 Hz now, for free:** `AURORA_XR_REFRESH=72` in
   `melee-env.txt` runs the display at 72 with the game on its own timer
   (no re-rendering: one frame in six repeats, rotationally reprojected).
   It shows what 72 Hz passthrough and the 5:6 object cadence feel like
   before anything is built. Option B would fix only the head staleness of
   the repeated frame on top of that.
2. **Prototype A1** (AppSW, zero motion vectors): it targets the actual
   complaint, keeps 120 Hz passthrough and every bit of tonight's tuned
   pacing, and costs little GPU. The open question is mixed reality; a
   day's spike answers it.
3. **If AppSW is unusable over passthrough, build B**, and plan on the eye
   pass getting about 20% cheaper (or a floor below 1.0 at 72 Hz) for the
   heavy stages.
4. **C (or A2) later,** as the object-motion upgrade on whichever path
   wins; the draw-identity work is shared.

## Sources

- [melee-unlocked](https://github.com/Hero88go/melee-unlocked):
  `port/runtime/gx/subframe.h`, `authored_pose.h`.
- [Introducing Application SpaceWarp](https://developers.meta.com/vr/blog/introducing-application-spacewarp/)
  (Meta): positional timewarp is enabled automatically for AppSW apps.
- [Unity: Understand Application SpaceWarp](https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.15//manual/features/spacewarp/spacewarp-overview.html).
- [Quest v83 brings System Positional TimeWarp](https://www.uploadvr.com/quest-horizon-os-v83-system-positional-timewarp-temporal-dimming/)
  (UploadVR): the system setting is experimental and opt-in.
- [OpenXR specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html):
  `XR_KHR_composition_layer_depth`, `XR_FB_space_warp`;
  [XrCompositionLayerSpaceWarpInfoFB](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSpaceWarpInfoFB.html),
  [XrCompositionLayerDepthInfoKHR](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerDepthInfoKHR.html),
  [XrFrameSynthesisInfoEXT](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameSynthesisInfoEXT.html).
