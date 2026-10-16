# Grounding Melee's stages in the room: a research report

Branch `quest-xr` at `bc2bb00`, written 2026-10-16. This is research only: no repo files were changed and the headset was not touched.

## TL;DR

- **Resolution Games' grounding comes down to three things:** darken the real surface under virtual objects, make the virtual content's light and colour agree with the camera feed, and hide virtual content where real surfaces should cover it. Most of their individual tricks are cheap. The expensive ones (environment-texture lighting, refraction) are the ones they call hard to control.
- **The best item for us costs no eye-pass time.** It is a soft contact/AO shadow drawn as a compositor **quad layer**, placed between the passthrough layer and our projection layer. The compositor reprojects it every display frame, so it can't swim the way an eye-pass shadow could under SpaceWarp. Our quad-layer and static-swapchain machinery already exists (pointer and placing layers in `xr.cpp`). The one hard requirement is **knowing where the real surface is.**
- **Colour and exposure matching is nearly free with what we already enable.** `XR_KHR_composition_layer_color_scale_bias` is already on, but we only use it on pointer quads. Chained onto the 3D projection layer, it gives a global colour scale with no eye-pass cost. `XR_FB_passthrough` styles (brightness, contrast, saturation) and `XR_META_passthrough_color_lut` can grade the *room* toward each stage's palette. That is the Isle of Pigs "colour tint on video feed" trick, and both are offered by our runtime.
- **Surprise 1: the scene and depth extensions aren't in our runtime's offered list.** The headset log `build/quest-perf/ext-warmup.log` (runtime "Oculus 209.113.0") lists 80+ extensions, but not `XR_FB_scene`, `XR_FB_spatial_entity*`, `XR_META_environment_depth`, `XR_META_spatial_entity_mesh` or `XR_META_environment_raycast`. Our manifest declares neither `com.oculus.permission.USE_SCENE` nor `USE_ANCHOR_API`. The most likely reason is that the runtime hides these extensions from apps that don't declare the permissions, the way it hides passthrough without `com.oculus.feature.PASSTHROUGH`. I couldn't confirm this from Meta's docs; it needs one manifest change and a re-read of the offered list. Either way, the planned tabletop mode can't start until this is sorted.
- **Surprise 2: Meta has no native light-estimation API.** Meta's own passthrough design page says to "consider ambient light estimation", but no API provides it. The only route is the Passthrough Camera API (Camera2, Quest 3/3S, Horizon OS v74+, a camera permission prompt). Meta's own brightness sample just averages the Y plane.
- **Surprise 3: our coverage-to-alpha step zeroes alpha wherever no depth was written.** Any darkening or feathering drawn *inside* the replays is erased. In-pass shadows or occluders must run in the `finish` hook after coverage, or be compositor layers. Environment-depth occlusion also needs a small change to the coverage logic (see T8).
- **Everything below is render-only.** At most it *reads* fighter positions, as `xr_scene.c` already does for the follow window. Nothing touches sim state, the wire or tick timing.

---

## 1. What Resolution Games did, and why it works

Sources:
- Part 1: https://www.resolutiongames.com/blog/how-we-grounded-angry-birds-ar-isle-of-pigs-in-your-living-room-part-1
- Part 2: https://www.resolutiongames.com/blog/how-we-grounded-angry-birds-ar-isle-of-pigs-in-your-living-room-part-2

Both are by Magnus, Technical Director of their AR/MR team. The game is ARKit-first, made in Unity.

Their own list of effects, quoted from Part 1:
- directional shadows
- ambient occlusion on physics objects
- ambient occlusion on props
- light intensity estimation
- light temperature estimation
- environment texture as light information
- colour tint on video feed
- particle effects in the air
- refraction in virtual objects
- occluder surfaces on peripheral planes

Part 2 adds performance and heat management.

| # | Technique | What they did | Why it works perceptually |
|---|---|---|---|
| R1 | **Blob / AO shadow under objects** | A subtle AO blob under each pig. They note that a drop shadow that moved vertically would make the pig look like it hovers. | Contact shadows are the strongest cue that an object rests *on* a surface rather than floating in front of it. The shadow's position relative to the object sets perceived height and depth; this is the "ball-in-a-box" result (Kersten, Mamassian & Knill 1997, https://journals.sagepub.com/doi/10.1068/p260171; demo at https://kerstenlab.psych.umn.edu/demos/shadows). Follow-up work found the effect survives even an unrealistic (contrast-reversed) shadow (https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5122971/), so a *cheap, approximate* shadow still buys most of it. |
| R2 | **Directional shadows on a shadow-catcher** | Unity shadow casters onto a ground material that is "completely transparent except for where there is a shadow received". It darkens the camera image "a little bit". Birds and the in-AR UI cast shadows too. | A shadow on the *real* surface ties the two worlds into one lighting model. It only darkens slightly because real shadows in a lit room are partial (ambient fill); a black shadow reads as a sticker. |
| R3 | **AO skirts on static props** | Hand-placed AO geometry around trees, signs and snow heaps, tunable per situation and faded in and out. | Contact darkening where two surfaces meet (crevice/contact AO) is what makes an object look *seated*. Hand-authoring it lets them tune it rather than accept baked lighting. |
| R4 | **Dynamic AO from below** | An orthographic camera renders the blocks from below into a 32×32 depth map ("darker = closer to the ground"), blurred up to 128×128, on a transparent ground quad. | A soft, direction-free footprint shadow that updates as the structure collapses. It is cheap because it is tiny and blurred. The author calls it "a really powerful way to place objects in the real world". |
| R5 | **Light intensity estimation** | ARKit's 0–1 estimate, run through *separate tuning curves* for the directional light and the ambient light. | Virtual objects that stay bright when the room dims look self-lit, like a screen, rather than part of the room. Their key line: "very subtle … when it works, but very jarring when it doesn't." |
| R6 | **Light temperature estimation** | Colour-temperature float → colour ramp → tints both lights. | White-balance mismatch is one of the first tells of a composite. Warm tungsten rooms make neutral CG look blue. |
| R7 | **Environment texture as lighting** | ARKit's stitched environment map fed to the skybox for lighting only (not drawn). | Reflections and ambient colour pick up the room's palette. They call it hard to control because brightness swings a lot. |
| R8 | **Colour tint on the video feed** | Per-theme colour (beach, desert, disco, snow) applied to the camera feed by exposing the YCbCr→RGB 4×4 matrix. They say it costs "practically nothing". | Instead of pulling the CG toward reality, it pulls reality toward the CG. Shared colour grading is the classic film compositing trick for making plates and CG feel shot together. |
| R9 | **Particles in the air** | Snowflakes and similar drifting through the space. | Particles at many depths around the viewer give motion parallax and stereo cues that fill the volume. That says "this space is shared" and invites the player to move their head. |
| R10 | **Refraction of the real world** | Ice blocks refract the camera image: background, opaque pass, grab the buffer, refraction shader. | Real background *through* virtual material proves the object is physically in the room. |
| R11 | **Occluder surfaces** | Every detected plane gets a depth-writing transparent occluder (the shadow plane too), which hides virtual things below ground. They note players rarely notice, and that depth data on newer devices does it better. | Interposition, meaning near things hide far things, is a dominant depth cue. Violating it ("the pig is visible *through* the table") breaks the illusion instantly. |
| R12 | **Heat-aware feature shedding** | Monitor frame rate and heat. When the OS signals heat, disable the most expensive features, then more as it builds. Profile under comparable thermal conditions. | Frame drops break grounding more than missing polish does. A stable frame keeps world-locked content stuck to the world. |

The articles don't cover scale cues, placement UX, edge/base treatments or sound. Where I discuss those below, the reasoning is mine plus Meta's design guidance, not Resolution's.

Meta's own MR design pages agree on the core points:
- "Place a halo or a drop shadow on the floor to 'pin' an object to a particular space in passthrough."
- "Without occlusion, virtual objects just appear to be floating inside passthrough."
- "When a virtual object is not properly aligned with a surface, it becomes difficult to understand its location and distance."

Source: https://developers.meta.com/horizon/design/mr-design-scene/

The Horizon OS passthrough page also says "Virtual objects look more believable when their lighting matches the real environment. Consider ambient light estimation." (https://developers.meta.com/horizon/essentials/horizon-os-passthrough/)

---

## 2. What we have today (code audit)

### Passthrough and compositing (`extern/aurora/lib/xr/xr.cpp`)
- **Passthrough setup:** one `XR_FB_passthrough` feature and one layer, `XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB`, running at creation (around line 1147).
  - No style is ever set: no `xrPassthroughLayerSetStyleFB`, edge colour, colour map or LUT.
  - `sync_passthrough()` starts and pauses it for MR or VR (line ~3661).
- **Layer order in a fight** (around line 3962):
  1. Passthrough, with `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`.
  2. HUD quad, *under* the 3D layer in MR.
  3. The 3D projection layer, premultiplied, with the SpaceWarp info chained per view.
  4. The placing quads and pointer quads.
- **The alpha rule** (`kCoverageShader`, line ~5161, run through `XrReplayTarget::finish = draw_coverage`):
  - After the replays, two full-screen triangles write *alpha only*.
  - Pixels whose depth is still cleared get **alpha 0**. Pixels with geometry depth get **alpha 1**.
  - Translucent draws that write no depth therefore *add light* over passthrough, like holograms.
  - Consequence: anything that should *darken* the room, such as a shadow, must write alpha **after** coverage, or live in its own compositor layer.
  - It also explains why the `AURORA_XR_FADE_BLEND` prototype (true alpha fades at soft clips, `gx.cpp:462`) can't survive: coverage zeroes the alpha it wrote.
- **`environmentBlendMode` is `OPAQUE`,** as Meta requires even with passthrough.
- **Colour scale/bias:** `XR_KHR_composition_layer_color_scale_bias` is enabled, but only used to tint pointer and bar quads (`add_pointer_quad`, line ~3434).
- **Reference space:** `XR_REFERENCE_SPACE_TYPE_LOCAL` (line 1091). We know nothing about the floor or table height. The calibration plan deliberately avoids seeding from `LOCAL_FLOOR`.
- **No real-world understanding of any kind:** no depth, scene, anchors, planes or occlusion, and no ground shadow, AO, base or plinth in the XR code.

### Extensions
**Enabled when offered:**
- `XR_KHR_vulkan_enable2`
- `XR_FB_passthrough`
- `XR_FB_display_refresh_rate`
- `XR_KHR_composition_layer_color_scale_bias`
- `XR_EXT_performance_settings`
- `XR_META_performance_metrics`
- `XR_FB_space_warp`
- `XR_EXT_hand_tracking`
- `XR_META_boundary_visibility`
- `XR_KHR_android_create_instance`

**Offered by the headset** (Horizon runtime 209.113.0, `build/quest-perf/ext-warmup.log`, 2026-10-11) and relevant here but not enabled:
- `XR_META_passthrough_color_lut`
- `XR_META_passthrough_layer_resumed_event`
- `XR_META_passthrough_preferences`
- `XR_FB_triangle_mesh` (for surface-projected passthrough)
- `XR_FB_hand_tracking_mesh`
- `XR_FB_composition_layer_alpha_blend`
- `XR_KHR_composition_layer_depth`
- `XR_FB_composition_layer_depth_test`
- `XR_EXT_local_floor`
- `XR_FB_spatial_entity_user`
- `XR_EXT_spatial_container` (an unfamiliar name; not in our 1.1.63 headers, so I couldn't look it up)

**Not offered:**
- `XR_FB_scene`, `XR_FB_scene_capture`
- `XR_FB_spatial_entity`, plus its `_query`, `_storage` and `_container` variants
- `XR_META_spatial_entity_mesh`, `XR_META_spatial_entity_discovery`
- `XR_META_environment_depth`, `XR_META_environment_raycast`
- `XR_EXT_spatial_entity`, `XR_EXT_plane_detection`

The OpenXR headers we build against (`openxr_loader_android-1.1.63`, and the SDL copy) do define all of these, so the headers are not the blocker.

### Manifest (`platforms/android/app/src/xr/AndroidManifest.xml`)
- **Declares:** `com.oculus.feature.PASSTHROUGH`, hand tracking, `BOUNDARY_VISIBILITY`.
- **Missing:** `com.oculus.permission.USE_SCENE` and `com.oculus.permission.USE_ANCHOR_API`.
- **What Meta's samples declare:**
  - XrSceneModel declares both.
  - XrPassthroughOcclusion declares `USE_SCENE`, and warns in code that "com.oculus.USE_SCENE permission should be requested before creation of OpenXR instance".
  - Source: https://github.com/meta-quest/Meta-OpenXR-SDK, `Samples/XrSamples/XrPassthroughOcclusion` and `XrSceneModel`.

### Stage staging (`src/pc/xr_scene.c`)
- **Per-stage rules:**
  - Part, joint and mesh hides (`s_builtin_rules`).
  - Moves.
  - Clip planes (`CLIP_BELOW/BEHIND/FRONT/LEFT/RIGHT`) in game units, with optional fade bands.
  - Soft clips are a Bayer dither. Straddling draws get split index lists (`AURORA_XR_CLIP_SPLIT`), so fully inside/outside draws stay on normal pipelines.
- **Fighters and items are never clipped.**
- **Placement:**
  - `s_placements`: game point and scale per grkind, on top of the global `AURORA_XR_ARENA_POS/SCALE/YAW` (desk default 0.0035 m/unit at (0, -0.25, -0.7)).
  - Head-relative for stages not placed this session.
  - Grabs are per stage, per session.
  - Every fight opens the placement hold (sim-free, `pc_net_active()` excludes netplay).
- **Floor knowledge:** `mpLib_FloorExtent` already gives each stage's floor span and top-platform height (logged at stage load). That is enough to synthesize a stage footprint.

### Melee's own shadows
`lbShadow` projects fighter silhouettes through an orthographic shadow camera onto *stage* geometry. Those survive the eye replay unchanged (`docs/xr-3d-plan.md` §4), and the four ~584×480 shadow passes are submitted early (`AURORA_XR_PRESUBMIT`). Nothing ever darkens the *real* surface.

### Planned but not built (from memory)
- **MR tabletop:** Scene API table outline, cut at the table plane, a neutral surface and border, bespoke per-stage table treatments.
- **Calibration ghost:** a floor plus Battlefield silhouette placed once, persisted with `XR_FB_spatial_entity` anchors.
- **Per-stage anchoring:** floor vs platform height.

### Budget (from `quest-benchmark.md`, `next-session-perf.md`, `ko-explosion-cost.md`)
- **Eye pass:** about 10.5–11 ms at GPU level 2 (456 MHz), 4× MSAA. Of that, about 2.85 ms is compositor preemption that our timestamps include.
- **Breakdown (where measured):**
  - MSAA 4×→1× saves 3.7 ms.
  - GX lighting is about 1.9 ms.
  - Binning is about 1.4 ms for about 600 draws.
  - The per-bin floor is about 0.02 ms × 66 bins.
- **Dynamic resolution:** aims at 13 ms, floor 1.0.
- **Headroom:** heavy 4P stages run with little or no margin. 2P Battlefield has about 5 ms.
- **Policy:** hide effects rather than drop resolution. Benchmark at CPU 3 / GPU 2.

---

## 3. Meta systems: availability, permissions, cost

| System | Native OpenXR on Quest 3/3S? | Offered to us today? | Permission | Likely cost | Notes |
|---|---|---|---|---|---|
| **`XR_FB_passthrough` styles** (`XrPassthroughStyleFB`: `textureOpacityFactor`, `edgeColor`; colour maps MonoToMono, MonoToRgba, `XrPassthroughBrightnessContrastSaturationFB`) | Yes | Yes (v5, already enabled) | `com.oculus.feature.PASSTHROUGH` (have it) | Compositor-side. Meta gives no numbers for styles. Changes "may only take effect in the next frame". | Mono maps force greyscale, which Meta calls inappropriate on colour-passthrough devices. Use brightness/contrast/saturation (BCS) or a LUT. Only one colour map can be chained at a time. https://developers.meta.com/horizon/documentation/native/android/mobile-passthrough-customization/ |
| **`XR_META_passthrough_color_lut`** (`XrPassthroughColorMapLutMETA`, `…InterpolatedLutMETA`) | Yes | Yes (v1, not enabled) | Same | "LUT resolution impacts memory usage and GPU performance". Start at 16. Max 64 on current devices. Creating a 64³ LUT "can take a few milliseconds", so build it at load. Updating every frame at 64 has a "notable performance impact"; animate `weight` instead. | Edges are not affected by LUTs. https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrPassthroughColorMapLutMETA.html |
| **`XR_KHR_composition_layer_color_scale_bias`** on any layer, passthrough included | Yes | Yes (enabled) | None | "Changes take effect immediately". Applies to the whole layer, edges included (Meta). Expect it to be essentially free, since it is a multiply-add in the compositor; I couldn't find a published cost. | For our premultiplied 3D layer: keep bias at 0 (bias would lift alpha-0 pixels and glow over the room) and alpha scale at 1. |
| **Surface-projected passthrough** (`XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB` + `XR_FB_triangle_mesh`) | Yes | Yes (triangle mesh offered) | Same | A second passthrough layer (max 3). No published cost. | "More stable rendition" on known surfaces, but "no depth testing between the passthrough projection surface and the objects rendered", and a mismatched surface gives "conflicting depth cues". Speculative for us. |
| **`XR_META_environment_depth`** | Yes on Quest 3. Meta's Unity page says "only supported on Quest 3 devices". Check `supportsEnvironmentDepth` at runtime for 3S. | **No**: missing from our list (see §2) | `com.oculus.permission.USE_SCENE` (runtime grant). Meta's sample checks it before `xrCreateInstance`. | Depth is generated by the runtime (system cost unpublished). Ours: one depth-swapchain acquire per frame, plus whatever passes sample it. Meta gives no ms figures: hard occlusion is "cheaper", soft "requires slightly more GPU". One forum report saw 72 → 30–50 fps after enabling Unity soft occlusion (2024, anecdotal). | Read-only depth swapchain; reproject using the returned per-eye pose and FOV. Unreliable under 0.2 m and degrading past about 4 m. Hand removal is optional. https://developers.meta.com/horizon/documentation/native/android/mobile-depth/ and https://communityforums.atmeta.com/quest-development-128/quest-3-pow-level-stuck-at-3-passthrough-depth-api-151541 |
| **`XR_FB_scene` + `XR_FB_spatial_entity` (+ `_query`, `_container`, `XR_FB_scene_capture`)**: planes with semantic labels (TABLE, FLOOR, WALL_FACE…), room layout, bounded 2D/3D | Yes | **No** (see §2) | The scene overview shows only `USE_ANCHOR_API` ("Scene and Spatial Anchors capabilities"). The spatial data permission page says `USE_SCENE` is needed at runtime, or `xrQuerySpacesFB` returns no bounded, semantic or room-layout spaces. Declare both. | One-time async queries: negligible per-frame cost. | Apps can request Space Setup. https://developers.meta.com/horizon/documentation/native/android/openxr-scene-overview/ and https://developers.meta.com/horizon/documentation/native/native-spatial-data-perm/ |
| **`XR_META_spatial_entity_mesh` / `XR_META_spatial_entity_room_mesh`** (room triangle mesh) | Defined in the registry; Meta's native docs barely cover it | **No** | `USE_SCENE` (presumably; not confirmed) | One-time | Overkill for a tabletop. Useful only for room-wide occluders or shadow receivers. https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetSpaceRoomMeshMETA.html |
| **`XR_META_environment_raycast`** | In the registry (needs `XR_EXT_future`, which is offered) | **No** | Unknown; probably `USE_SCENE` | Per-ray, async creation | Would allow "drop the stage onto whatever surface is under it" without Space Setup. I couldn't confirm Quest support or OS version. https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrPerformEnvironmentRaycastMETA.html |
| **`XR_EXT_spatial_entity` / `XR_EXT_spatial_plane_tracking`** (Khronos, 2025) | Meta engineers co-authored them. I found no confirmation that Horizon OS ships them. | No | n/a | n/a | https://www.khronos.org/blog/openxr-spatial-entities-extensions-released-for-developer-feedback |
| **Light / ambient estimation** | **None.** Meta recommends it but offers no API. `XR_ANDROID_light_estimation` exists in the headers but is Android XR's, and isn't offered on Quest. | n/a | n/a | n/a | https://developers.meta.com/horizon/essentials/horizon-os-passthrough/ |
| **Passthrough Camera API** (Camera2 / NDK camera) | Yes: Quest 3/3S, Horizon OS v74+. Not OpenXR, plain Android Camera2. | n/a (not an XR extension) | `horizonos.permission.HEADSET_CAMERA` (passthrough cameras only) or `android.permission.CAMERA`. Passthrough must be on. | Meta: about **1–2 % GPU per streamed camera**, about **45 MB** memory; 60 Hz; 20–40 ms capture latency; YUV420; 1280×960 or 1280×1280. | Camera data is "Device User Data" under the developer data policy. Meta's samples estimate brightness from the Y plane (BT.709, rolling average, hysteresis). Exposure and white-balance metadata aren't documented. https://developers.meta.com/horizon/documentation/native/android/pca-native-overview and https://developers.meta.com/horizon/documentation/unity/unity-sample-camera-brightness |
| **`XR_FB_hand_tracking_mesh`** (hand occluder) | Yes | Yes (offered) | `HAND_TRACKING` (have it) | Skinned mesh, depth-only; small | Meta's guidance: draw occluders "to the depth buffer only, with color writes disabled". |
| **`XR_EXT_performance_settings` notifications** (`XrEventDataPerfSettingsEXT`, thermal/compute levels) | Yes | Yes (enabled) | None | None | **We enable it but don't handle its events** (no `PERF_SETTINGS` event handling in `xr.cpp`). This is the direct analogue of Resolution's "OS signals heat". |

The CPU/GPU caps for passthrough apps are documented at https://developers.meta.com/horizon/essentials/cpu-gpu-levels/: passthrough locks out the higher GPU levels on Quest 3/3S. That is why every benchmark runs at CPU 3 / GPU 2.

---

## 4. Technique-by-technique mapping

Ratings run Low / Med / High. **Perf** is cost against the eye pass at GPU 2, plus any compositor or system cost. Estimates are marked "est." and are not measured.

### T1. Contact/AO shadow as a compositor quad layer *(R1, R3, R4)*
- **Approach:**
  - Add one `XrCompositionLayerQuad` lying flat on the real surface under the stage. Submit it **after passthrough and before the HUD and projection layers**.
  - Its texture is a premultiplied black blob/AO footprint: RGB 0, alpha around 0.25–0.45 at the core, falling off softly.
  - Generate the footprint on the CPU at stage load from `mpLib_FloorExtent` (floor span) plus a per-stage depth (z extent from the clip boxes and a small table), blurred to about 128×128 like Resolution's 32→128 trick.
  - Upload it once through `create_static_swapchain`, which the pointer textures already use.
  - The stage's own pixels (alpha 1) cover the shadow where the stage sits on it. The shadow shows only around and beneath the stage, which is exactly where it should.
- **Fighter blobs (the R1 cue):**
  - Up to four small quads sharing one static blob texture. Only their poses change per frame: the fighter's x and z projected straight down onto the surface plane.
  - Opacity and size vary with height above the surface: fainter and wider as the fighter rises. Per the Kersten result, this is what makes off-stage fighters read as "over the table".
  - Positions come from a render-side read in `xr_scene.c`, where `Player_GetEntity` and `ftLib` are already used.
- **Why a quad layer and not the eye pass:**
  - Zero eye-pass cost.
  - The compositor reprojects quads at display rate with their true pose, so the shadow stays locked to the table even on SpaceWarp-synthesized frames. An eye-pass shadow with no depth in the SpaceWarp depth image would be treated as infinitely far and could slide on synthesized frames.
- **The hard requirement: a surface height.**
  - *Tabletop:* the table plane from the Scene API (T10).
  - *Floating:* the calibration ghost's "floor" (`placement-calibration-plan.md`), or nothing at all.
  - A shadow at the wrong height is worse than none: it says "the stage is floating *here*" with false confidence.
  - Rule: show it only when a surface is known.
- **Layer budget:** the hold can already submit 3 + 5 placing + 6 pointer layers. Read `XrSystemGraphicsProperties::maxLayerCount` and drop fighter blobs while pointing.
- **Ratings:**
  - Payoff: **High** in tabletop. **Med** in floating, and only with a calibrated surface.
  - Effort: **Low–Med.**
  - Perf: eye pass **0**; compositor small (est. well under 0.2 ms per display frame for a few small quads; unmeasured).
  - Risk: **Low** with the known-surface rule.

### T2. Same shadow, drawn in the eye pass (alternative to T1)
- **Approach:** draw the shadow in `draw_coverage`'s pass *after* `coverSet`, in tile memory, with premultiplied-over blending of (0, 0, 0, a). Alpha must go up (`a + dst.a·(1−a)`) so the room darkens.
- **Pros:** can be depth-tested against stage geometry, and has no layer-count limits.
- **Cons:**
  - It costs eye-pass time (est. 0.1–0.3 ms for a partial-screen quad at 4× MSAA across 66 bins; unmeasured).
  - It needs a multiview twin pipeline.
  - It isn't in the SpaceWarp depth, so it may swim on synthesized frames.
- **Ratings:** Payoff **High** (same as T1). Effort **Med.** Perf **Low–Med.** Risk **Med** (SpaceWarp).
- **Use only if** T1's layering proves wrong somewhere.

### T3. AO band on the stage where it meets the cut *(R3, the "skirt" on the virtual side)*
- **Approach:**
  - Today a `CLIP_BELOW` cut (Great Bay at y 0, Fountain's pole, the planned table-plane cut) leaves geometry that just stops.
  - Darken stage geometry within N game units above a "ground" clip plane: multiply by `mix(k, 1, smoothstep(0, N, dist))`.
  - The XR clip variant in `gx/shader.cpp` already computes per-plane distances for fade bands, so this is a few ALU ops on a flagged plane.
  - **Catch:** draws classified "fully inside" use the normal pipeline. Classification would have to treat "inside but within N of a ground plane" as needing the variant. No discard is needed, so early depth stays.
- **Ratings:**
  - Payoff: **Med.** It makes the stage look seated *in* the table rather than sliced.
  - Effort: **Med** (shader-variant and classifier change; pipeline cache churn on first run).
  - Perf: **Low** (ALU on a few more draws).
  - Risk: **Low–Med.**

### T4. Exposure/colour match by scaling our 3D layer *(R5, R6, lightweight)*
- **Approach:**
  - Chain an `XrCompositionLayerColorScaleBiasKHR` on the projection layer (`proj.next`, which is free; SpaceWarp info hangs off the views, not the layer).
  - Use colour scale (r, g, b) with alpha 1 and bias 0.
  - Start with **per-stage** constants, since Melee's TV-lit stages differ a lot in brightness (Brinstar Depths vs Fountain), plus a **room-light preset** (Bright / Normal / Dim) in the launcher.
  - Treat it as a look test (the user's blind A/B method) before building any sensing.
- **Perceptual rationale:** in a dim room, a full-brightness stage reads as an emissive screen, not an object. A 10–25 % pull-down plus a slight warm or cool shift aligns it with the auto-exposed passthrough.
- **Caveats:**
  - It dims additive effects too (fine).
  - The HUD is a separate layer; leave it alone for legibility.
- **Ratings:** Payoff **Med** (subtle when right). Effort **Low** (a struct and a launcher row). Perf **≈0** (compositor). Risk **Low.**

### T5. Grade the room toward the stage *(R8: Resolution's cheapest big win)*
- **Approach:**
  - During fights only, set a passthrough style on our existing layer with `XrPassthroughBrightnessContrastSaturationFB`, or enable `XR_META_passthrough_color_lut` and use a 16³–32³ LUT per stage family. For example:
    - Brinstar: warm red, slightly darker.
    - Icicle Mountain / Big Blue: cool.
    - Fountain of Dreams: dusk violet.
    - Battlefield / FD: slight dim plus desaturation ("house lights down").
  - Fade in with `XrPassthroughColorMapInterpolatedLutMETA` (identity → stage LUT, `weight` animated) when the hold releases. Fade out at results.
  - Build LUTs at stage load, never in a fight frame (64³ creation takes milliseconds).
- **Perceptual rationale:** shared grading makes the plate and the CG feel lit by one light. A slightly dimmed, desaturated room also raises the stage's relative salience without touching our render.
- **Ratings:**
  - Payoff: **Med–High.**
  - Effort: **Low** for BCS; **Low–Med** for LUTs (author 20-odd LUTs; a script can derive them from stage palette screenshots).
  - Perf: eye pass **0**; compositor small (Meta: LUT resolution matters, so stay ≤32; unmeasured).
  - Risk: **Med.** Taste: some players won't want their room tinted, so ship it with a strength setting and keep it mild. Comfort and safety: never dim the room hard, since the user is holding a gamepad in a real room.

### T6. Room light sensing via the Passthrough Camera API *(R5, R6 for real)*
- **Approach:**
  - A Java/NDK Camera2 stream at the smallest resolution and lowest rate allowed.
  - Average Y and Cb/Cr over a downsampled frame about once a second, smoothed with hysteresis like Meta's brightness sample.
  - Map through *separate tuning curves* (Resolution's lesson) into T4's colour scale, and optionally T5's grade.
  - All on a side thread; nothing in the render chain waits on it.
- **Big uncertainty:**
  - Passthrough is auto-exposed and auto-white-balanced. Camera frames may sit near mid-grey and neutral in most rooms, which would mostly measure the AE/AWB.
  - What we actually want to match is the *displayed* passthrough, and Meta doesn't say whether camera-API frames get the same processing as the displayed feed.
- **Ratings:**
  - Payoff: **Low–Med** (uncertain, as above).
  - Effort: **Med–High** (Camera2 plumbing, permission prompt and fallback, Quest 3/3S and v74+ gating).
  - Perf: about **1–2 % GPU per camera** (Meta) and about 45 MB; negligible CPU at 1 Hz.
  - Risk: **Med.** It adds a camera permission prompt (a privacy and store-review surface), and like any new permission dialog it can block unattended `quest_perf` launches.
- **Do T4 first.** If manual presets are judged enough, skip T6.

### T7. Table occluder plus generic table cut *(R11)*
- **Approach:**
  - In tabletop mode, the planned cut at the table plane hides the *stage* below the table. *Fighters and items are never clipped*, so a fighter falling off-stage drops *through* the real table.
  - Fix: a depth-only quad over the table polygon drawn first in the eye pass (writes depth, no colour), so anything below the table fails the depth test.
  - **Coverage would then mark the occluder alpha 1** (a black table). Fix it after `coverSet`: re-draw the same occluder with depth compare **Equal**, writing alpha 0. That clears exactly the pixels where nothing virtual drew in front of it.
  - Give the occluder's depth output `@invariant` so both draws produce bit-identical depth.
  - This mirrors Meta's own guidance: "Draw the occluder to the depth buffer only, with color writes disabled."
- **Ratings:**
  - Payoff: **High** in tabletop (interposition violations are the most jarring failure). **N/A** floating.
  - Effort: **Med.**
  - Perf: **Low** (two small quads per view; est. under 0.2 ms).
  - Risk: **Low–Med** (Equal-compare precision; test it).

### T8. Environment-depth occlusion (dynamic: hands, controllers, people)
- **Approach:**
  - Enable `XR_META_environment_depth` (after T10's permission work) and acquire the depth image per frame.
  - Import the runtime's `VkImage`s into Dawn the same way `xr.cpp` already wraps swapchain images (`SharedTextureMemory`). **Depth-format import through Dawn's shared texture memory is unverified.**
  - In the eye pass:
    - A full-screen depth-only pass, *before* the replays, that writes reprojected environment depth into our reversed-Z buffer, so real surfaces nearer than the stage early-Z-reject stage fragments.
    - After coverage, the same pass again with compare Equal, writing alpha 0 (T7's trick, full-screen).
  - Hard edges only; soft occlusion would need per-fragment sampling in every GX pipeline.
- **When it matters:** in fights, hands are usually on a gamepad below the line of sight, so payoff is mostly during the placement hold and when someone walks between the player and the stage.
- **Ratings:**
  - Payoff: **Med.**
  - Effort: **High.**
  - Perf: **Med** (est. 0.4–1 ms: two full-screen passes per view at 4× MSAA over 66 bins, plus a texture fetch; plus the runtime's depth generation, which is unknown and shares the GPU).
  - Risk: **High** (jaggies and lag from low-res depth; 0.2 m near limit; Quest 3S support; Dawn import).
- **If built:** it is the first thing to hide under dynamic-resolution pressure.

### T9. Hand-mesh occluder in the placement hold only
- **Approach:** `XR_FB_hand_tracking_mesh` (offered; hands are already tracked) skinned with the joint poses we already locate. Draw it depth-only with T7's Equal re-clear, only while holding or pointing.
- **Why:** hands reaching into the stage to grab it are the most common occlusion failure, and the hold renders no fight, so there is no perf concern.
- **Ratings:** Payoff **Med** (placement feel). Effort **Med.** Perf **0** in fights. Risk **Low.**

### T10. Scene model for table and floor planes (the enabler)
- **Approach:**
  1. Add `USE_SCENE` and `USE_ANCHOR_API` to the XR manifest.
  2. Request `USE_SCENE` from `MeleeXrActivity` before instance creation, and only when tabletop is chosen (Meta recommends requesting on use, with a fallback).
  3. Re-read the offered extension list.
  4. Then `XR_FB_spatial_entity` + `_query` + `_container` + `XR_FB_scene` (+ `XR_FB_scene_capture` to launch Space Setup if no model exists).
  5. Query TABLE planes and take the bounded-2D polygon.
  - This feeds T1 (shadow plane), T3 (ground clip plane) and T7 (occluder), and is the tabletop mode itself.
- **Ratings:**
  - Payoff: **High** (enabler).
  - Effort: **Med.**
  - Perf: **≈0** per frame.
  - Risk: **Med.** The extensions aren't offered today, and the reason is unconfirmed. Permission UX adds a dialog. The user's Space Setup may lack a table, so fall back to the calibration ghost.

### T11. Placement UX grounding *(R1 applied to placing; Meta's "alignment" guidance)*
- **During the hold:**
  - Show T1's footprint shadow and a thin **drop line** from the stage's main floor to the surface.
  - **Snap height** to the known surface (table plane, calibration floor or, if it turns out to be available, `XR_META_environment_raycast`), with a magnet zone.
  - Show the floor-span outline on the surface.
  - All as quads through the existing placing layers.
- **Scale cues:** the fighters are the viewer's familiar-size anchor. Tabletop "scale = fit table" means fighter size varies with table size. That's acceptable, but a **minimum fighter height** (e.g. a clamp on the arena scale) avoids "ant" fighters on large tables. Today's per-stage scales (0.55–1.15×) already vary fighter size across stages by about 2×; consider whether tabletop should keep fighter scale constant and crop instead.
- **Ratings:** Payoff **Med–High.** Effort **Low–Med.** Perf **0** in fights. Risk **Low.**

### T12. Base and edge treatments (floating mode)
- **Plinth / diorama base:** a generic shaded slab or disc under floating stages, generated from the floor polygon. It makes the stage read as one *object*, a "toy on a shelf", which the eye accepts more readily than a fragment hanging in space.
  - This is the floating-mode counterpart of the planned tabletop "neutral surface and border".
  - Ratings: Payoff **Med** (taste-dependent). Effort **Med.** Perf **Low–Med** (eye-pass fill, est. 0.1–0.4 ms). Risk **Med** (taste).
- **True alpha feathering at soft clips** instead of Bayer dither (`AURORA_XR_FADE_BLEND`):
  - It needs the coverage step to preserve the alpha that blended draws wrote: for example, a separate "fade" draw class that writes alpha *after* coverage, or a stencil-tagged keep.
  - Ratings: Payoff **Low–Med** (the dither already reads fine at distance). Effort **Med.** Perf **Med** (blending loses early-Z on those draws; Brinstar acid showed how costly discard paths are). Risk **Med.**
- **Passthrough edge rendering** (`edgeColor`):
  - Not a grounding tool: it outlines the *room*. It might help as a temporary aid during calibration ("here are your surfaces").
  - Ratings: Payoff **Low.** Effort **Low.** Perf small. Risk **Low.**

### T13. Ambient particles in the room *(R9)*
- **Approach:** extend selected stages' existing ambience slightly past the stage box, such as Icicle Mountain snow or Fountain's kept sparkles, so it drifts over the table and in front of the player. Additive particles add light over passthrough already.
- **Ratings:**
  - Payoff: **Low–Med.**
  - Effort: **Low–Med** (per-stage clip/particle rule changes).
  - Perf: **Med.** Particles are about 10 % of the eye pass, and the user already hides KO particles to protect frame time.
  - Risk: **Low.**
- **Verdict:** a per-stage treat, not a system.

### T14. Real-world refraction *(R10)*
- **Not recommended.** Melee's refraction (`lbRefract`) copies the *mono* game frame. Refracting the room would mean compositing camera frames (T6's stream at 20–40 ms latency) into the eye pass.
- **Ratings:** Payoff **Low.** Effort **High.** Perf **Med–High.** Risk **High.**

### T15. Heat-aware shedding *(R12)*
- **Approach:**
  - Handle `XrEventDataPerfSettingsEXT` (CPU and GPU domains, thermal and compute sub-domains; the extension is already enabled).
  - Keep a shed list ordered by cost: T8 → T13 → T12 plinth → T2/T3. Never shed T1, T4 or T5, which cost nothing in the eye pass.
  - This matches the user's "hide effects, not resolution" rule, and adds a thermal input that dynamic resolution doesn't have today.
- **Ratings:** Payoff **Med** (robustness in long sessions). Effort **Low.** Perf **0.** Risk **Low.**

### T16. Already done and worth noting: spatial audio
Steam Audio with stage-placed sources (Brinstar's lava, the crowd) is an auditory grounding cue that Resolution didn't cover. Its sound sources share the arena transform, so they stay consistent with any placement or snapping change (T10/T11).

### Prerequisite: registration stability
Contact shadows and AO bands *expose* any swim between the stage and the table, because the eye compares edges at the contact. The SpaceWarp vector fixes (`096d7da`, `fcbca0c`) matter more here than anywhere else. Keeping shadows as compositor quads (T1) sidesteps the problem for the shadow itself.

---

## 5. Summary table

| # | Technique | Payoff | Effort | Quest perf (eye pass / other) | Risk |
|---|---|---|---|---|---|
| T4 | Colour scale on 3D layer (per stage, room preset) | Med | Low | 0 / ≈0 | Low |
| T5 | Passthrough BCS/LUT grade per stage, fade in | Med–High | Low–Med | 0 / small compositor | Med (taste) |
| T1 | Contact/AO + fighter-blob shadow quads | High (tabletop) | Low–Med | 0 / small compositor | Low (if surface known) |
| T15 | Thermal events → shed list | Med | Low | 0 | Low |
| T10 | Manifest permissions + Scene API tables | High (enabler) | Med | ≈0 | Med |
| T11 | Placement: drop line, snap, footprint | Med–High | Low–Med | 0 in fight | Low |
| T7 | Table occluder + Equal re-clear | High (tabletop) | Med | <0.2 ms (est.) | Low–Med |
| T3 | AO band at ground cut | Med | Med | Low | Low–Med |
| T9 | Hand-mesh occluder in the hold | Med | Med | 0 in fight | Low |
| T12a | Plinth/base for floating stages | Med | Med | 0.1–0.4 ms (est.) | Med |
| T6 | Camera-API light sensing | Low–Med | Med–High | 1–2 % GPU/camera, 45 MB | Med |
| T2 | In-pass shadow catcher | High | Med | 0.1–0.3 ms (est.) | Med |
| T8 | Environment-depth occlusion | Med | High | 0.4–1 ms (est.) + system | High |
| T12b | True alpha feathering | Low–Med | Med | Med | Med |
| T13 | Room-filling particles | Low–Med | Low–Med | Med | Low |
| T14 | Real-world refraction | Low | High | Med–High | High |

---

## 6. Prioritised shortlist (cheapest high-payoff first)

1. **T4: colour-scale the 3D layer.** One struct on `proj.next` in `xr.cpp`, per-stage constants next to `s_placements`, and a room-light preset in the launcher. Blind A/B it. Zero eye-pass cost.
2. **T5: grade the passthrough during fights.** Start with `XrPassthroughBrightnessContrastSaturationFB`, since it needs no new extension: a gentle dim and desaturate as "house lights down", faded in at hold release. Move to `XR_META_passthrough_color_lut` (16³–32³, built at stage load, `weight` animated) for per-stage palettes if BCS proves the idea.
3. **T15: handle perf-settings thermal events.** A small event-loop addition that gives future effects a shed path consistent with the "hide, don't drop resolution" rule.
4. **T10: unblock the scene stack.**
   - Add `USE_SCENE` and `USE_ANCHOR_API` to `platforms/android/app/src/xr/AndroidManifest.xml`, grant `USE_SCENE`, and re-log "OpenXR extensions offered".
   - That one experiment settles whether `XR_FB_scene`, `XR_META_environment_depth` and `XR_META_environment_raycast` become available.
   - Then build table-plane queries. This is the tabletop mode's first step anyway.
5. **T1: contact/AO footprint plus fighter blobs as compositor quads,** shown only when a surface is known (tabletop table, or the calibration floor). This is the single biggest grounding win, and it costs nothing in the eye pass.
6. **T11: placement grounding:** footprint, drop line and surface snap in the hold. It reuses T1 and the placing-layer code.
7. **T7: table occluder with the Equal re-clear,** so fighters stop falling through the real table in tabletop mode.
8. **T3: AO band at the ground cut** (clip-shader change), so stages sit *in* the table.
9. *Later, measured first:* T9 (hand occluder in the hold), T12a (plinth for floating mode), then T6 and T8 only if earlier steps show the light-matching or occlusion gap is still the weakest link.

T13 and T14 stay on the shelf.

---

## 7. Things to verify (I couldn't confirm these)

- **Why the scene and depth extensions are absent:** whether the Horizon runtime hides `XR_FB_scene`, `XR_FB_spatial_entity*` and `XR_META_environment_depth` from apps lacking `USE_SCENE` or `USE_ANCHOR_API`. Meta's docs show the permissions but don't say that enumeration is filtered. One manifest change plus a log read settles it.
- **Depth API on Quest 3S:** Meta's Unity docs say "Quest 3 only"; check `supportsEnvironmentDepth`.
- **`XR_META_environment_raycast`:** whether it ships on Quest at all, at which OS version, and under which permission.
- **Compositor costs:** for quad layers, BCS and LUT styles, and colour scale/bias. Meta publishes no numbers, so benchmark with `quest_perf.py` at CPU 3 / GPU 2 using the compositor GPU counter (`/perfmetrics_meta/compositor/gpu_frametime`).
- **Camera-API frames vs the displayed passthrough:** whether they share exposure and white balance. This decides whether T6 measures anything useful.
- **Dawn depth-image import:** whether Dawn's `SharedTextureMemory` path can wrap the environment-depth swapchain's depth-format `VkImage`s.
- **`XR_EXT_spatial_container`:** what this offered extension is. It isn't in our 1.1.63 headers.
- **Layer count:** `XrSystemGraphicsProperties::maxLayerCount` on Quest 3, against the hold's worst case plus the T1 shadow quads.

---

## 8. Sources

- Resolution Games, Part 1: https://www.resolutiongames.com/blog/how-we-grounded-angry-birds-ar-isle-of-pigs-in-your-living-room-part-1
- Resolution Games, Part 2: https://www.resolutiongames.com/blog/how-we-grounded-angry-birds-ar-isle-of-pigs-in-your-living-room-part-2
- Meta, MR design (scene): https://developers.meta.com/horizon/design/mr-design-scene/
- Meta, Horizon OS passthrough essentials: https://developers.meta.com/horizon/essentials/horizon-os-passthrough/
- Meta, native passthrough: https://developers.meta.com/horizon/documentation/native/android/mobile-passthrough/
- Meta, native passthrough customization (styles, LUTs, colour scale, projected passthrough): https://developers.meta.com/horizon/documentation/native/android/mobile-passthrough-customization/
- Khronos, `XrPassthroughColorMapLutMETA`: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrPassthroughColorMapLutMETA.html
- Khronos, `xrCreatePassthroughColorLutMETA`: https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreatePassthroughColorLutMETA.html
- Meta, native Depth API: https://developers.meta.com/horizon/documentation/native/android/mobile-depth/
- Meta, Unity Depth API (hard vs soft occlusion, device support): https://developers.meta.com/horizon/documentation/unity/unity-depthapi-occlusions/ and https://developers.meta.com/horizon/documentation/unity/unity-depthapi-xr-oculus/
- Meta forum, passthrough plus Depth API levels and frame rate (anecdotal): https://communityforums.atmeta.com/quest-development-128/quest-3-pow-level-stuck-at-3-passthrough-depth-api-151541
- Meta, OpenXR Scene overview: https://developers.meta.com/horizon/documentation/native/android/openxr-scene-overview/
- Meta, native spatial data permission: https://developers.meta.com/horizon/documentation/native/native-spatial-data-perm/
- Meta OpenXR SDK samples (XrPassthroughOcclusion, XrSceneModel manifests and code): https://github.com/meta-quest/Meta-OpenXR-SDK
- Khronos, room mesh: https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetSpaceRoomMeshMETA.html
- Khronos, environment raycast: https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrPerformEnvironmentRaycastMETA.html
- Khronos, spatial entities blog: https://www.khronos.org/blog/openxr-spatial-entities-extensions-released-for-developer-feedback
- Meta, Passthrough Camera API (native): https://developers.meta.com/horizon/documentation/native/android/pca-native-overview
- Meta, Passthrough Camera (Android apps): https://developers.meta.com/horizon/documentation/android-apps/passthrough-camera
- Meta, camera brightness sample: https://developers.meta.com/horizon/documentation/unity/unity-sample-camera-brightness
- Meta, Android passthrough camera samples: https://developers.meta.com/horizon/documentation/android-apps/passthrough-camera-samples
- Meta, CPU/GPU levels (passthrough caps): https://developers.meta.com/horizon/essentials/cpu-gpu-levels/
- Kersten, Mamassian & Knill 1997, "Moving cast shadows induce apparent motion in depth": https://journals.sagepub.com/doi/10.1068/p260171 (demos: https://kerstenlab.psych.umn.edu/demos/shadows)
- Ouhnana & Kingdom 2016, "Objects versus shadows as influences on perceived object motion": https://www.ncbi.nlm.nih.gov/pmc/articles/PMC5122971/
- Repo: `extern/aurora/lib/xr/xr.cpp`, `extern/aurora/lib/gfx/xr_replay.hpp`, `extern/aurora/lib/gx/gx.cpp`, `src/pc/xr_scene.c`, `src/pc/xr_scene.h`, `platforms/android/app/src/xr/AndroidManifest.xml`, `docs/quest-xr.md`, `docs/xr-3d-plan.md`, `build/quest-perf/ext-warmup.log`, and the project memories listed in the brief.
