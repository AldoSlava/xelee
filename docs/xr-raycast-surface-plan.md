# Finding the real surface under the stage: environment raycast for T1

Branch `quest-xr` at `6417425`, written 2026-10-16. This is research and design only: no code was changed, nothing was built, and the headset was not used.

This follows `docs/xr-stage-grounding-report.md` (T1 contact shadow, T10 scene planes, T11 placement aids, §7 open items) and `docs/xr-stage-placement-ux.md` (the stage spot, MR floating, the Place Stage scene).

## TL;DR

- **Feasible, with one permission.** `XR_META_environment_raycast` casts a ray against **live depth** and returns the first hit, with a surface normal. It needs no Space Setup and no scanned room. Our runtime (Horizon 209.113.0) has it but hides it because the manifest lacks `horizonos.permission.USE_SCENE`. Its one dependency, `XR_EXT_future`, is already offered. It works on **Quest 3 and 3S only**. Our manifest also lists Quest 2 and Pro, so the feature must be optional.
- **The result answers T1's gating rule directly.** T1 says "show the shadow only when the real surface height is known". A ray straight down from the stage measures that height. The ray only works while the spot is in the depth cameras' view, and the result can be noisy. So we filter it: several rays across the footprint, a consensus on height, a flatness check, and agreement over time before anything changes.
- **Recommended cadence: measure while placing, then freeze.**
  - Cast continuously while the stage is being placed (the Place Stage scene, the hold, a pause grab).
  - Confirm with a short burst when the player lets go, then **freeze** the result for the fight.
  - Cast a 1 s burst when a stage appears without a hold. That covers netplay and the proposed "Start right away" default.
  - Don't cast periodically during fights unless the probe shows the raycaster costs nothing.
- **Fade with height, from one formula.** The shadow's strength is `R²/(R²+h²)`, where `R` is the footprint's effective radius and `h` is the gap between the stage's underside and the surface. That is the ambient occlusion of a disc. It is strong on a desk 10–25 cm below, faint on a floor a metre below, and zero when nothing is known. No separate "table vs floor" classifier is needed.
- **Fighter blobs:** go on the **real surface only**. Melee's own `lbShadow` already darkens the stage's floors under fighters. Real-surface blobs are compositor quads *under* the projection layer, so the stage's own pixels hide them wherever the stage is in the way. They show only where a fighter is off-stage, which is exactly where the height cue helps.
- **The same raycaster drives T11.** It gives the drop line, a footprint outline on the surface and a magnet snap when the player lets go. The stage spot gains a small "surface" record. It is never trusted on a later launch without a fresh, confirming hit.
- **The code fits one pattern.** A worker thread owns the raycaster and publishes a filtered `SurfaceEstimate`. The XR thread only reads that snapshot when it builds quad layers. Nothing touches sim state, the wire, tick timing or the input path.
- **The unknowns are the cost of keeping the depth service running, and the consent prompt.** Meta publishes no figures for latency, accuracy or cost. The **probe build** (step P0) measures all three, plus how the dialog behaves, before anything else is built.

---

## 1. The API

### 1.1 Lifecycle

Source: the spec chapter (`specification/sources/chapters/extensions/meta/meta_environment_raycast.adoc` in KhronosGroup/OpenXR-Docs, last modified 2025-03-01) and our own 1.1.63 headers (`build/android-arm64-xr/_deps/openxr_loader_android-1.1.63/.../openxr.h`).

1. **Enable `XR_EXT_future` and `XR_META_environment_raycast` at instance creation.** Without the future extension, `xrCreateInstance` fails with `XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED`.
2. **Check support.** Chain `XrSystemEnvironmentRaycastPropertiesMETA` onto `xrGetSystemProperties` and read `supportsEnvironmentRaycast`. If it is false, creating a raycaster must fail with `XR_ERROR_FEATURE_UNSUPPORTED`.
3. **Create the raycaster asynchronously.** `xrCreateEnvironmentRaycasterAsyncMETA(session, &info, &future)` returns an `XrFutureEXT`; its return value "only indicates whether the parameters were acceptable". Poll with `xrPollFutureEXT` until the future is READY, then call `xrCreateEnvironmentRaycasterCompleteMETA`, which fills `XrEnvironmentRaycasterCreateCompletionMETA { futureResult, environmentRaycaster }`.
   - "Runtimes may initialize needed resources to perform raycast operations during this asynchronous call". This is probably where the depth service starts.
   - The create call can also return **`XR_ERROR_PERMISSION_INSUFFICIENT`**. That is most likely where a missing `USE_SCENE` grant shows up (inferred; see §1.4).
   - Creating a raycaster takes no options: `XrEnvironmentRaycasterCreateInfoMETA` has only `type`/`next`.
4. **Cast.** `xrPerformEnvironmentRaycastMETA(raycaster, &getInfo, &hit)` takes:
   - `XrEnvironmentRaycastHitGetInfoMETA { baseSpace, time, origin, direction, filterCount, filters }`;
   - optionally `XrEnvironmentRaycastFilterDistanceMETA { maxDistance }`. The runtime "must not return any hitpoint that is more than maxDistance away from origin". This is the only filter defined.
   - It returns `XrEnvironmentRaycastHitMETA { status, pose }`, with `pose` in `baseSpace`.
   - It is **synchronous**: the result comes back from the call. The spec doesn't say how long it takes.
   - Possible errors include `XR_ERROR_TIME_INVALID`, so `time` must be a sensible recent `XrTime`.
5. **Destroy** with `xrDestroyEnvironmentRaycasterMETA`.

### 1.2 Hit pose and normal

From the spec:
- **Position** is "the first point of intersection when traveling from origin in the direction".
- **Orientation** is "the best estimate of the normal of the surface hit by the ray". **The normal is +Z rotated by the orientation**, and rotating +Y by the orientation "roughly points in the direction of the ray origin".

For a ray cast straight down onto a table, the normal `qrot(o, {0,0,1})` should be close to world up `(0,1,0)`.

### 1.3 Hit statuses

From the spec, with our handling:

| Status | Meaning (spec) | Pose usable? | Our handling |
|---|---|---|---|
| `HIT` | Valid hit | Yes | A candidate |
| `NO_HIT` | No intersection | No | Counts as "nothing below", but only if it persists |
| `HIT_POINT_OCCLUDED` | "the hit point is occluded. For example, if you are looking at a screen, the hit point may be on the wall behind it." The pose "may not be accurate". | Weakly | Weak candidate. Never starts an estimate; can only keep one alive. |
| `HIT_POINT_OUTSIDE_OF_FOV` | "outside camera's field of view" | No | **No information.** Keep the last estimate; don't decay it. |
| `RAY_OCCLUDED` | "the input ray is occluded" | No | No information (for example, a hand or gamepad between the headset and the table) |
| `HIT_INVALID_ORIENTATION` | Position known, normal undefined | Position only | Candidate for height only; the flatness check is skipped |

The spec adds: "the application can instruct users to move to a different viewing point to achieve more accurate raycast results."

Meta's MRUK wrapper adds a `NotReady` status that the native enum lacks. Their wording is "It takes several frames after the EnvironmentRaycastManager is enabled before the raycast is ready" ([MRUK enum, v207](https://developers.meta.com/vr/reference/mruk/v207/enum_meta_x_r_environment_raycast_hit_status)). Natively, expect a short warm-up after creation in which casts return `NO_HIT` or out-of-FOV. The filter in §2.4 already absorbs that.

### 1.4 What it measures, and what it needs

- **Live depth, no Space Setup.**
  - MRUK: `Raycast` "Shoots a virtual ray into the live depth buffer and reports the first intersection point", and "Only rays within the device's depth camera frustum will return hits" ([MRUK environment raycast](https://developers.meta.com/horizon/documentation/unity/unity-mr-utility-kit-environment-raycast/)).
  - Meta's WebXR guide: it "does not require room scanning — it works immediately using real-time surface detection" ([IWSDK guide](https://developers.meta.com/horizon/documentation/web/iwsdk-guide-environment-raycast/)).
  - The camera-FOV and occlusion statuses in the spec agree: the answer depends on what the headset can see *now*.
  - **I couldn't rule out** that the runtime also uses a stored scene mesh when one exists. Nothing says it does.
- **The Depth API is no longer a prerequisite.** MRUK says "As of v81 of MRUK, the Depth API is no longer needed for this feature". Earlier MRUK versions raycast against the Depth API texture themselves. The native extension appears to run its own depth internally. (Meta's v81 API reference still describes the old dependency, so the docs disagree with each other.)
- **Devices.**
  - The Khronos OpenXR-Inventory lists the extension for **Quest 3 and Quest 3S** (and Meta's XR Simulator), not for Quest 2 or Pro.
  - A third-party Quest log shows the runtime skipping it "as device requirements … are not met" (StereoKit-rust `examples/example_of_quest_log.txt`; the device isn't named, presumably a Quest 2 or Pro).
  - Meta's Depth API page: "requires Meta Quest 3, Meta Quest 3S" ([Depth API overview](https://developers.meta.com/vr/documentation/unity/unity-depthapi-overview/)).
  - Our manifest's `com.oculus.supportedDevices` is `quest2|questpro|quest3|quest3s`, so on two of the four listed devices the shadow has to be simply absent.
- **Passthrough.** The Depth API requires passthrough ("Passthrough is required for receiving environment depth"). Raycasting probably does too. MR floating always has passthrough on, so that's fine.
- **Range and near limit.**
  - The depth near limit is "approximately 0.2 meters"; closer than that is "unreliable" ([native Depth API](https://developers.meta.com/horizon/documentation/native/android/mobile-depth/)). At the desk default the stage sits 0.7 m away, so we're well clear.
  - No far limit is published. MRUK's `maxDistance` default is 100 m. A floor 1–2 m below a desk-height stage is well within what the depth cameras see.
- **Latency and accuracy:** **not published anywhere I could find.** The spec, MRUK, the Unity package and the motif docs give no millimetre or millisecond figures. Meta warns in general that depth "might be too expensive for your app in terms of CPU or GPU usage" and "you might not be satisfied with the latency of the real-time depth information" ([passthrough occlusions](https://developers.meta.com/horizon/documentation/unity/unity-customize-passthrough-passthrough-occlusions/)). For a *static* table, latency hardly matters; for the drop line during a drag it might. The probe measures both (§7, P0).
- **Hands.** The Depth API has optional hand removal; the raycast extension has no such option. A hand or controller under the stage will be hit like any surface. That matters most while grabbing the stage (§2.5).
- **Prior art.** Meta's "Instant Content Placement" motif does what T1 and T11 want: "casts a ray from the object downwards towards the environment", places "a grounding shadow below the object, which sits tightly on the detected surface", and on release moves the object "onto the surface with a little bit of offset" so the shadow stays visible ([motif blog](https://developers.meta.com/vr/blog/mixed-reality-motif-instant-content-placement-virtual-objects-project-setup/), [motif docs](https://developers.meta.com/horizon/documentation/unity/unity-mrmotifs-instant-content-placement/)). It uses **one** downward ray, and a shader that cuts the shadow off at the surface edge.

### 1.5 The permission and its prompt

- **Two names.** Our runtime's filter wants **`horizonos.permission.USE_SCENE`** (from `build/quest-perf/ext-warmup.log`). Meta's docs and samples all still use `com.oculus.permission.USE_SCENE` ([native spatial data permission](https://developers.meta.com/horizon/documentation/native/native-spatial-data-perm/); Unity-PassthroughCameraApiSamples `EnvironmentRayCastSampleManager.cs`). I found no document of the rename; `horizonos.permission.USE_ANCHOR_API` shows the same pattern. **Declare both** in the manifest. The probe logs `checkSelfPermission` for each, to see which one the package manager knows and whether a grant on one covers the other.
- **Declaration vs grant.** The runtime hides the extension when the manifest lacks the *declaration* ("missing uses-permission string … from AndroidManifest"). So the extension can be enabled before the user has *granted* anything. The grant is presumably checked when the raycaster is created (`XR_ERROR_PERMISSION_INSUFFICIENT`).
  - Meta's XrPassthroughOcclusion sample warns that USE_SCENE "should be requested before creation of OpenXR instance" (for depth).
  - **Unverified for raycast:** whether a grant that arrives *after* `xrCreateInstance` lets the raycaster be created without restarting the instance. The probe tests exactly this.
- **What the user sees.** Meta: "The request will display a one-time permission explainer dialog, followed by a permission consent confirmation dialog." **I couldn't find the dialog's wording.** In Settings it is filed under the user-facing name "spatial data" (Meta's docs call it the "spatial data permission").
- **Automatic prompting (a risk).** The same page says: "If the app is compiled with the current versions of the SDK, there is no requirement to request the permission in the app. The system will automatically show a permission dialog when the app is launched." We don't use Meta's SDK. We use the Khronos loader, with `metavr:targetSdkVersion="207"` in the manifest. So **it's unknown whether simply declaring the permission makes the OS prompt at launch.** If it does, the prompt appears before the player ever chose the feature. Like the Guardian dialog (`guardian-pause-gamepad` memory), it would also block unattended `quest_perf` launches until granted once. P0 checks this first, with a manifest-only build.
- **If declined.**
  - Scene queries "won't return" data, silently.
  - Raycaster creation should fail with `XR_ERROR_PERMISSION_INSUFFICIENT` (likely, not confirmed).
  - Meta's guidance is to request "only when using the functionality" and "to provide a fallback if the user decides not to grant the permission".
  - Android stops showing the dialog after repeated denials. After that, only the system Settings page can grant it.

---

## 2. Using it for T1

### 2.1 What we're measuring

The stage's room pose is `A = T(pos)·R_y(yaw)·S(scale)` about the stage's centre point `c` (game units). It is yaw-only, so a horizontal real surface is horizontal in game space too. Per stage, T1 needs three things:

- **`y_s`:** the real surface height in LOCAL, under the stage.
- **`h`:** the gap between the stage's **underside** (its lowest visible point after clips) and `y_s`, in metres.
- **`coverage`:** how much of the footprint is over that surface. This catches a stage hanging over a table edge.

The stage data comes from the game side. That is new, but cheap:
- the footprint x span (`mpLib_FloorExtent`, already measured in `stage_top()`);
- a z depth: from the clip box where `s_clips` boxes the stage, otherwise a default of ±30 units, tuned per stage;
- an underside y: the `CLIP_BELOW` plane where one exists (Great Bay 0, Yoshi's Story −40, FoD −80…), otherwise a small per-stage table next to `s_placements`.

These would reach aurora through a new `aurora_xr_set_stage_footprint(xmin, xmax, zmin, zmax, ybottom)`, called beside `aurora_xr_set_stage_top`.

### 2.2 Where the rays go

```
   top view of the footprint (stage space, yaw applied)      side view
   ┌───────────────────────────────┐                     origin row ─ ─ ─ ─ ─ ─  underside + 0.10 m
   │  o           o           o    │                         │    │    │
   │                               │                         │    │    │   rays straight down,
   │  o           O           o    │   O centre ray          │    │    │   maxDistance 2.5 m
   │                               │                         ▼    ▼    ▼
   │  o           o           o    │                     ════════════════  real surface y_s
   └───────────────────────────────┘
   3×3 grid at ±0.8 of the half-extents
```

- **Direction:** world down `(0, −1, 0)`, not stage-down. The stage has no pitch or roll anyway.
- **Origin:** each grid point at the stage's underside **plus 0.10 m**.
  - Starting *above* the underside means a stage resting on (or slightly sunk into) the table still sees the table. A ray that starts below the table top would come back `RAY_OCCLUDED`, or would hit the floor.
  - 10 cm is less than the gap to anything that isn't the support surface. A desk lamp *beside* the stage isn't under it.
- **Distance filter:** `maxDistance` 2.5 m. That reaches the floor from a "Room" preset stage, and stops rays wandering into a lower storey through a stair gap.
- **Nine rays, not one.**
  - One ray (Meta's motif) can't tell a stage half off the table, and one hand under the centre fools it.
  - Nine give a consensus height and a coverage figure.
  - If the probe shows a ray costs well under 0.1 ms, a 5×3 grid gives finer coverage for the masked shadow (§2.6).
- **`time`:** the latest `predictedDisplayTime` that the XR thread published. LOCAL is static, so the exact time hardly matters, but it has to be a valid recent `XrTime` (`XR_ERROR_TIME_INVALID`).

### 2.3 When to cast

| Moment | Cadence | Why |
|---|---|---|
| **Place Stage scene, the hold, a pause grab** (`g_placing > 0`, or `pointing` with a grab active) | Continuous, ~15 Hz, 9 rays | The stage moves. The drop line and snap (§4) need a live answer. The player is looking at the stage, so the spot is in the depth cameras' view. |
| **Release** (hold ends, Place Stage A, grab let go) | A 0.5 s confirmation burst at 15 Hz, then **freeze** | The pose is final. The estimate is confirmed with the hands gone (they made the grab). |
| **Stage appears without a hold** (netplay; the proposed "Start right away" default; a stage change) | A 1 s burst from the first 3D frame, then freeze | The stage is in view from frame 0. The burst runs during "Ready… GO!", purely on the render side. |
| **Events:** a recenter (`REFERENCE_SPACE_CHANGE_PENDING`, still unhandled in `poll_events`), the session returning to FOCUSED after the headset was off, a re-adopted stage spot | A 1 s burst; the old estimate is dropped first after a recenter | LOCAL has moved, or the room may have changed |
| **During fights** | **None by default.** Optional validation at 0.2 Hz (one centre ray), *only* if P0 shows a live raycaster costs ≈0. | The stage doesn't move in a fight. Validation would catch a table that was carried away, and would undo a wrong freeze. |

"Once when the hold releases" is not enough alone. The proposed C.4 default ("Start right away", `docs/xr-stage-placement-ux.md`) and netplay both mean many fights have no hold, so the appearance burst is the main path. Periodic casting is the weakest option: it costs whatever the depth service costs for the whole fight, and the scene under a stage almost never changes mid-fight.

### 2.4 From hits to an estimate: filtering and hysteresis

**For each cast (one grid of rays):**
1. **Keep only usable hits.** Keep `HIT`, plus `HIT_INVALID_ORIENTATION` for height. Keep `HIT_POINT_OCCLUDED` only as a weak vote.
2. **Flatness.** Drop hits whose normal `n = qrot(o, +Z)` is more than 15° off world up (`n.y < 0.966`). That throws away cushions, laps, the side of a monitor and the edges of objects.
3. **The hand column.** Drop hits where a tracked hand or controller (`G.hands[*].touch`, the grip pose) is within 0.12 m horizontally of the ray *and* between the hit and the origin in height. That's the user's own hand under the stage while grabbing.
4. **Cluster by height.** Sort the remaining heights and take the largest cluster within ±1.5 cm.
   - The candidate is `y = median(cluster)`, and `coverage = |cluster| / rays cast`.
   - It must have at least 4 of 9 rays, including at least one of the centre row. Otherwise this cast has **no candidate**.
   - **Ties:** choose the *lower* cluster only if it covers more rays. A box on the table under one corner shouldn't lift the whole shadow.
5. **Count no-information rays.** Rays that came back `OUTSIDE_OF_FOV` or `RAY_OCCLUDED` are counted separately. A cast that is ≥50 % no-information is "blind" and changes nothing.

**Over time, as a small state machine:**

| State | Enters when | Leaves when |
|---|---|---|
| **Unknown** (start; after a recenter) | — | Candidates from casts spanning ≥ 0.4 s (placing) or ≥ 0.6 s (burst) agree within ±1 cm, and their spread is σ ≤ 5 mm → **Known** |
| **Known (y, coverage)** | as above | **Moved:** a *different* candidate (|Δy| > 3 cm) is consistent for ≥ 0.5 s while placing, or ≥ 2 s from validation casts, with σ ≤ 5 mm → Known(new). **Gone:** casts that are not blind find no candidate for ≥ 1 s → Unknown. **Blind casts do nothing.** |
| **Frozen** (after release or burst) | Known at release | A new burst or placement → Known (live). Validation can move it only by the "Moved" rule. |

- **Smoothing.** While Known, follow the candidate with an EMA (τ ≈ 0.25 s). Ignore changes smaller than 5 mm, so the shadow doesn't shimmer.
- **Why the σ gate catches dynamic intruders.** A table is still to the millimetre; a hand, a pet or a person walking by isn't.
  - A hand held under the stage for half a second will mostly fail the σ gate (hands jitter by more than 5 mm), the hand column (3), or both.
  - A sleeping cat is the one case that passes. It is also, arguably, now the surface under the stage.
- **Fading.** Shadow opacity eases in and out over 250 ms, through the quad's colour-scale alpha (§5.4). A change of state never pops.

### 2.5 Close below or floating high: one formula, not a classifier

The question "is a surface close below the stage?" doesn't need thresholds. Under diffuse room light, the occlusion at the centre of a disc's shadow is the fraction of the cosine-weighted sky the disc blocks: **`R²/(R²+h²)`** for a disc of radius `R` at height `h`.

So the shadow's core alpha is:

```
R_eff  = sqrt(footprint_width · footprint_depth / π) · scale     (metres)
h      = max(0, y_underside − y_s)                                (metres)
alpha  = a_max · coverage_factor · R_eff² / (R_eff² + h²)        a_max ≈ 0.40 (grounding report: 0.25–0.45)
blur   = σ0 + k·h          (penumbra grows with the gap)
show   iff state ∈ {Known, Frozen} and alpha ≥ 0.04
```

At desk defaults on Battlefield (factor 1.1 × 0.0035 m/unit; a footprint of about 0.53 × 0.19 m, so `R_eff` ≈ 0.18 m):

| Situation | `h` | `R²/(R²+h²)` | Core alpha (a_max 0.4) | Reads as |
|---|---|---|---|---|
| Snapped onto the desk | 0–2 cm | ≈1.0 | 0.40 | Sitting on the desk: a tight contact shadow |
| Desk default, desk below | ~0.2 m | 0.45 | 0.18 | Hovering just over the desk: a soft, wide shadow |
| Couch preset over a coffee table | ~0.4 m (R ≈ 0.36 m) | 0.45 | 0.18 | Same reading, at a larger scale |
| Floating over the floor | ~1.0 m | 0.03 | 0.013 | Hidden (below 0.04) |
| Unknown, blind, declined, Quest 2/Pro | — | — | — | **No shadow** (T1's rule) |

This replaces the three-way "show at surface / fade with height / hide" choice with one continuous rule: the AO law fades the shadow as the stage rises. A cut-off below 4 % alpha hides it once it would be imperceptible anyway.

Also:
- **A stage sunk into the table** (underside below `y_s`): the gap is clamped to 0, giving full contact strength. The part of the stage under the table top is T7's occlusion problem, not T1's.
- **Follow-window stages** (Race to the Finish, the Adventure courses) never show a footprint shadow: their visible geometry slides through the window.
- **Diorama stages** (Big Blue, Mute City) use their clip box as the footprint.

### 2.6 Edges: a stage hanging off a table

A quad can't be cut off at the table edge the way Meta's motif shader cuts its shadow. Without a cut, the half of the shadow past the edge floats in mid-air. Two answers, in order of cost:

1. **Coverage fade (minimum viable).** `coverage_factor = smoothstep(0.55, 0.9, coverage)`. A stage mostly on the table keeps its shadow; one hanging well off it loses it.
2. **Coverage-masked texture (later).** With a 5×3 or 7×3 ray grid, mark which samples landed on the surface cluster. Blur that mask into the footprint texture (a 128×64 premultiplied black image, built on the worker in well under a millisecond). Upload it to a 2-image swapchain only when the frozen estimate changes. The shadow then stops at the table edge, to within one grid cell. That's coarse, but it hides the worst case.

### 2.7 Fallbacks when raycast is unavailable

In order:
1. **The calibration floor** (`placement-calibration-plan.md`), once it is built. The user put that ghost's floor on a surface on purpose, which is a declared surface, so the shadow may use it. When both exist, the raycast can check it: a disagreement of more than 3 cm means the room changed, so the raycast wins and the calibration is flagged.
2. **`XR_EXT_local_floor`**, which is offered and needs no permission. It gives the real floor height.
   - Under the AO law, a floor a metre down gives no visible shadow at desk scale. It matters only for a "Room" preset stage placed low.
   - The user ruled out *seeding placement* from LOCAL_FLOOR (2026-10-03). Using it only as a shadow receiver is a different use, but it's flagged as decision D5 anyway.
3. **The stage-space floor:** not a fallback for T1. A shadow drawn at some guessed height below the stage claims a surface that may not exist, which is exactly what the gating rule forbids. If floating stages need grounding with no known surface, that's T12a's plinth: an obviously virtual base, which claims nothing about the room.
4. **No shadow.** This is the default whenever 1–2 don't apply.

---

## 3. Fighter blob shadows

**Recommendation: blobs on the real surface only. No new on-stage blobs.**

- **On the stage's own floor, Melee already draws them.** `lbShadow` projects fighter silhouettes onto stage geometry, and it survives the eye replay (`docs/xr-3d-plan.md` §4). Extra blobs there would double the shadow.
- **Placement in the layer stack does the selection for free.** Real-surface blobs are compositor quads between passthrough and the projection layer. Stage pixels are alpha 1 in the 3D layer, so **wherever the stage is between the eye and the blob, the stage hides it.** A fighter standing on Battlefield has its blob under the stage, out of sight. A fighter recovering off the ledge, or knocked out over the table, has a visible blob on the table under them. That is the Kersten "ball in a box" height cue (grounding report R1), exactly where Melee has no shadow at all.
- **The size, opacity and gating rule is a stylised drop shadow, not the AO law.** A fighter is only about 3–4 cm wide at desk scale, so the AO law would make it invisible a few centimetres up. Use instead:
  - `alpha = 0.35 · clamp(1 − h_f / 0.5 m, 0, 1)`;
  - `size = r0 · (1 + h_f / 0.25 m)`, where `h_f` is the fighter's height above `y_s`;
  - hidden when `h_f < 0`. Below the blast-zone line a fighter can pass *under* the real desk surface, and a shadow on the desk then would be a lie.
  - Blobs exist only when the stage's surface estimate is Known or Frozen.
- **Timing: match the shown frame.** The projection layer shows an image from a few loops earlier (fixed-latency presentation; `stereo.shownTick`). Quads take their pose at `xrEndFrame`.
  - Fighter positions have to be **published with their game tick**, through a ring of the last ~8 ticks.
  - The XR thread then picks the entry for `stereo.shownTick`. Otherwise the blobs lead the fighters by 2–4 frames.
  - The positions come from a render-side read in `xr_scene.c`, as the follow window already does with `Player_GetEntity` (read only).
- **Layer budget.** Show blobs only in unheld fights (`placing == 0`). During the hold the fighters stand on the stage, so the stage hides their blobs anyway.

---

## 4. Snap and placement aids (T11)

The same raycaster and estimate drive all of T11, at the 15 Hz placing cadence:

- **Drop line.**
  - A thin vertical quad from the stage's main floor (the arena position) down to `y_s`, turned about the vertical to face the head. It reuses the pointer beam's static texture, tinted with colour scale.
  - It shows only while placing, and only in the Known state. When Unknown or blind, it fades out rather than guessing a length.
  - It also answers "how high am I?" while dragging: the length is the gap.
- **Footprint on the surface.** While placing, the T1 shadow itself is the footprint. Optionally add an outline ring (a second static texture) so a high stage, whose shadow has faded, still shows where it is over the surface.
- **Magnet snap.**
  - **Never during a drag.** The hand owns the pose, and a pull would fight it.
  - **On release:** if `|h| ≤ snap_zone`, ease the stage vertically over 150 ms so that its underside sits at `y_s + 3 mm`. `snap_zone` is 4 cm, or 10 % of `R_eff`, whichever is larger. The 3 mm is Meta's motif's "little bit of offset", so the contact shadow stays visible.
  - The snap goes through `set_arena_pose`, so it's logged like any grab ("Arena for stage N placed at …").
  - A haptic tick and the menu "confirm" sound mark it.
- **B (reset)** goes back to the spot or default, as today. The estimate is then re-cast (a burst), never carried over.

### 4.1 How it feeds the stage spot

`aurora_xr_adopt_stage()` (the Place Stage scene's A) captures the arena pose into `g_adoptedArena`. Add a small record next to it:

```
struct SpotSurface {
  bool  valid;        // a Known/Frozen estimate existed when adopted
  bool  snapped;      // the snap fired: the spot "rests on" the surface
  float gapM;         // underside-to-surface gap at adoption (metres, >= 0)
  float yLocal;       // y_s in LOCAL at adoption (this launch only)
};
```

How the record is used:
- **This launch:** the spot's semantics don't change. Origin, facing and scale stay what the player placed. The record only saves a re-measure: a stage at the spot whose fresh burst agrees within ±1 cm skips the Unknown phase.
- **Other stages.** Each stage is placed as spot × factor about its centre point, so its underside sits at a different height. Two options (decision D3):
  - **(a) As today.** Each stage gets its own burst at appearance. Some stages' undersides end up inside the table, which is acceptable for floating mode.
  - **(b) "Never sink below a known surface".** If the stage spot is snapped, a stage whose underside would go below `y_s` is lifted to `y_s + 3 mm`, render-side, per stage.
  - Recommendation: (a) now, and (b) as part of tabletop, where the calibration plan's per-stage floor/platform anchoring belongs. The placement UX report deliberately keeps a surface out of the floating spot (Axis 1), and (b) would quietly add one.
- **Persistence (phase 2, anchors).**
  - When the spot is saved with `horizonos.permission.USE_ANCHOR_API`, save `{snapped, gapM}` with it, **relative to the anchor**, never `yLocal`.
  - On a later launch the record is a **hint only**. The shadow appears only after a fresh burst confirms a surface within ±2 cm of `anchor.y − gapM`. Otherwise the stage stays at the anchored pose, the hint is dropped, and the shadow follows whatever the fresh burst says. A table moved since the last session must never get a shadow at its old height.
- **Recenter.** On `REFERENCE_SPACE_CHANGE_PENDING` (handler not built yet; placement UX doc C.5): drop the estimate, set Unknown and burst.

---

## 5. Fit with our code and constraints

### 5.1 Where it lives (`extern/aurora/lib/xr/xr.cpp`)

| Piece | Where | Notes |
|---|---|---|
| Offer detection | The extension scan (~l.671–682) | `B.hasFutureExt`, `B.hasEnvRaycastExt`; an `AURORA_XR_RAYCAST=0/1/probe` env gate like `AURORA_XR_HANDS` |
| Enable | The `exts` list (~l.689–720) | Both names pushed together, only when both are offered |
| Function pointers | After `xrCreateInstance`, via `xr_proc<>` (l.604) | `xrCreateEnvironmentRaycasterAsyncMETA`, `…CompleteMETA`, `xrDestroyEnvironmentRaycasterMETA`, `xrPerformEnvironmentRaycastMETA`, `xrPollFutureEXT`, `xrCancelFutureEXT` |
| Support check | Beside the SpaceWarp properties query (~l.742) | `XrSystemEnvironmentRaycastPropertiesMETA`. Also log `XrSystemGraphicsProperties::maxLayerCount` (grounding report §7). |
| The worker and its estimate | A new section, "XR worker: surface under the stage", after "arena and screen placement" (~l.2500) | §5.2 |
| Worker start | Next to `create_hand_trackers()` (l.4265), after the session exists | It creates the raycaster itself, off the XR thread |
| Worker stop | Session teardown (~l.4335), **before** `xrDestroySpace(B.space)` / `xrDestroySession` | Join, then destroy the raycaster; cancel any pending future |
| Quad layers | `render_xr_frame` layer assembly (~l.3955–4090) | §5.4 |
| Stage data in | New `aurora_xr_set_stage_footprint(...)` and `aurora_xr_set_fighter_marks(tick, n, xyz[], r)` in `include/aurora/xr.h` | Called from `src/pc/xr_scene.c` |

### 5.2 Threading: the worker owns the raycaster

**Why a worker, and not the XR thread.**
- `xrPerformEnvironmentRaycastMETA` is synchronous, and we don't know its cost. It may be a quick search of a depth map in our process, or an IPC round trip to the runtime service.
- The XR thread's loop is what paces the game and samples pads (`render_xr_frame`: `xrWaitFrame` → `update_input` → … → `xrEndFrame`). The pad-alarm and SpaceWarp work (`xr-pacing-pad-alarm`, `spacewarp-late-frames` memories) showed how sensitive pacing is to stray milliseconds there.
- Even 9 rays at an unknown cost inside that loop could push `xrEndFrame` late, or delay the next `update_input`.

**The design:**

```
 game thread (xr_scene.c)            XR thread (render_xr_frame)                 surface worker
 ───────────────────────            ──────────────────────────                 ──────────────
 aurora_xr_set_stage_footprint ──►  g_arenaMutex: g_arena, footprint
 aurora_xr_set_fighter_marks ───►   g_markMutex: ring[8] by tick
                                    publish g_lastDisplayTime (atomic)
                                    publish hands (touch/grip) snapshot ───────►  wake: placing? burst? event?
                                    publish g_surfaceWant (atomic enum) ───────►  snapshot pose+footprint+hands
                                                                                  N × xrPerformEnvironmentRaycastMETA
                                                                                  filter (§2.4) → SurfaceEstimate
                                    read g_surface (mutex, ~48 B copy)  ◄───────  publish under g_surfaceMutex
                                    build shadow/blob/drop-line quads
                                    xrEndFrame
```

- **Waking.** The worker waits on a condition variable with a timeout: 66 ms while placing, otherwise until an event or a burst. It never takes `g_mutex` or any lock the render worker holds while drawing. It copies the arena pose under `g_arenaMutex` and lets go before casting.
- **Thread safety.**
  - Only the worker uses the raycaster handle, so nothing it holds needs external synchronisation. (`xrDestroy…` needs the handle externally synchronised, as destroy calls usually do. The worker is the only user, so that's met.)
  - `baseSpace = B.space` (LOCAL) is used read-only, like any concurrent `xrLocateSpace`. The spec page for `xrPerformEnvironmentRaycastMETA` lists no externally synchronised parameters.
- **Creating the raycaster.** The worker calls `…AsyncMETA`, polls `xrPollFutureEXT` every 50 ms, then `…CompleteMETA`.
  - `futureResult` or the call itself returning `XR_ERROR_PERMISSION_INSUFFICIENT` → state `Declined`. Log it once and never retry this launch.
  - `FEATURE_UNSUPPORTED` → state `Unsupported`.
- **Lifetime: depends on P0.**
  - If a live, idle raycaster costs ≈0, create it at session start and keep it. It's then ready, past its warm-up, before the first placement.
  - If it costs measurable GPU or power, create it on demand (placing, or a burst pending) and destroy it ~2 s after the last cast. A ~100–500 ms creation delay (unknown) is then covered by the placement hold's own duration, or by the "Ready… GO!" countdown.
- **Priority.** A normal-priority thread, not pinned, never real-time. It sleeps 95 % or more of the time.

**Input latency is untouched.** The XR thread adds one mutex-guarded copy and up to a few quads per frame. The game thread adds two small setters per rendered frame. No pad path, no `xrWaitFrame` timing and no Dawn work changes.

### 5.3 From hits to a pose in LOCAL and in stage space

- **In LOCAL.**
  - The hit pose is in `B.space` (LOCAL).
  - Surface: `y_s = hit.position.y`; normal `n = qrot(hit.orientation, {0,0,1})`.
  - Only the height is kept. Flatness has already filtered for `n ≈ up`, and the quads lie exactly horizontal, which avoids tilt jitter from noisy normals.
- **In stage space** (for the game side, if ever needed, and for the logs).
  - `y_game = c_y + (y_s − a.pos.y) / (a.scale · g_stageZoom)`. This is `to_game()` (l.2597) applied to a point.
  - The arena has yaw only, so `y_game` is one number for the whole footprint.
- **Shadow quad pose** (LOCAL), with the footprint centre `f = (cx, ybottom, cz)` in game units:
  - `p = A(f)` with its y replaced by `y_s + 0.002 m`. The 2 mm keeps it from fighting a passthrough reconstruction of the same plane. It's a compositor quad, so there's no z-fighting as such; the offset is about perceived parallax.
  - Orientation `q = R_y(a.yaw) · R_x(−90°)`. Quads face +Z, so this lays the quad flat, facing up.
  - Size `(width · s, depth · s) · (1 + spread · h / R_eff)`, where `s = a.scale · g_stageZoom`.
- **Blobs.** Each fighter mark `(x, y, z)` in game units → `A(x, y_game_surface, z)` → the same flat orientation.

### 5.4 Reaching the quad layers

In `render_xr_frame`, inside `if (fs.shouldRender && fight)`, the new layers go **right after the passthrough layer, before the HUD-under quad and `proj`**:

```
passthrough → [footprint shadow] → [fighter blobs ×≤4 | drop line + outline while placing] → HUD (under, MR) → projection (3D) → placing quads → pointer quads
```

- **Textures.** Static swapchains made procedurally on the XR thread with `create_static_swapchain` (l.3233), as the pointer beam, dot, bar and back textures are:
  - the footprint: 128×64, a rounded rectangle blurred at 3 levels (or the mask texture of §2.6);
  - one 64×64 radial blob;
  - the existing beam texture for the drop line.

  All premultiplied black: RGB 0, falling alpha.
- **Opacity.** An `XrCompositionLayerColorScaleBiasKHR` on each quad's `next`, with colour scale `(1, 1, 1, alpha)` and bias 0.
  - The extension is already enabled and `add_pointer_quad` already chains it. RGB is 0, so scaling alpha alone scales the darkening.
  - Opacity eases with no texture upload.
  - Blur levels are chosen with ±10 % hysteresis on `h`, crossfading over 150 ms by drawing both levels during the fade.
- **The drop line.** Under the projection layer, the stage's pixels would hide its top end where it meets the main floor. That's physically right: the line starts *under* the floor. Keep it under `proj`. The visible part runs from the underside down to the surface, which is the part that matters.
- **Layer budget.**
  - `layers` is sized `3 + kMaxPlacingLayers (5) + kMaxPointerLayers (6) = 14` today.
  - The spec minimum for `maxLayerCount` is 16. I couldn't confirm Quest 3's value; P0 logs it.
  - **During a hold** the HUD is off (`placing == 0` is required for `showHud`), so we use passthrough 1 + proj 1 + placing ≤ 5 + pointers ≤ 6 = 13. Adding footprint 1 + drop line 1 = 15. No blobs.
  - **In a paused fight with pointers:** 1 + 1 + HUD 1 + pointers 6 = 9. Adding footprint and blobs (5) = 14.
  - **In a normal fight:** 3 + 5 = 8.
  - Raise the array to `3 + 5 + 6 + 2` and add a budget check: when `maxLayerCount` is smaller, drop blobs first, then the outline.
- **SpaceWarp.** Quads are reprojected by the compositor every display frame with their true pose, so the shadow stays put on synthesised frames (grounding report T1). The stage's alpha edge over the shadow comes from the extrapolated projection layer, and may move a pixel against the shadow on synthesised frames. That's the same registration issue as any contact, and the 096d7da/fcbca0c vector fixes are what keep it small.
- **Known limitation.** Quads have no depth test against the real room, so a mug standing inside the footprint gets darkened along with the table under it. That's acceptable for a soft, 18–40 % shadow. Only T8 (environment-depth occlusion) would fix it.

### 5.5 Determinism and netplay

- **The raycaster, the estimate, the quads and the snap are all aurora XR-side presentation.** The snap moves `g_arena`, the same render-only pose a grab moves (7b717fa's invariant).
- **The game side only *reads*:** floor extents at stage load and fighter positions once per rendered frame, in `xr_scene.c`, the same kind of reads as the follow window and `stage_top`.
- **Nothing writes sim state, pads, the wire or tick timing.** `pc_net_active()` needs no new check: in netplay the appearance burst runs during "Ready… GO!" like any other render work, and there's no hold.
- **Under rollback,** blobs follow whatever ticks get rendered and shown, matched by `shownTick`, so a re-simulated tick just carries its own marks.
- **Record/replay** is unaffected. `MELEE_NET_RENDER_AUDIT` should stay clean, because nothing new is drawn by the game's render path.

### 5.6 Quest performance at CPU 3 / GPU 2

| Cost | What we know | Expectation | How P0 measures it |
|---|---|---|---|
| **Eye pass** | Quads are composited, not drawn in our pass | **0** | — |
| **Compositor** | A few small quads; the grounding report estimates well under 0.2 ms (unmeasured) | Small | `/perfmetrics_meta/compositor/gpu_frametime` (`XR_META_performance_metrics`, already enabled), with and without the quads |
| **Per-ray CPU or IPC** | Unpublished | Unknown: µs (a local depth-map search) to ~1 ms (IPC) | Time each call with `monotonic_ns`; log p50, p99 and max every 10 s |
| **The depth service** (the runtime making depth because a raycaster exists) | Unpublished. "Runtimes may initialize needed resources" at creation. Meta warns depth "might be too expensive". On Quest 3 the depth sensing may already run for passthrough, but **I found no statement either way.** One anecdotal forum report saw 72 → 30–50 fps with Unity soft occlusion (that's depth plus a heavy per-pixel shader, not raycasting). | **The big unknown.** It could be ≈0 (shared with passthrough) or a measurable GPU/DSP load. That shared GPU budget is what decides whether we can keep the raycaster alive. | The 4P Brinstar replay `bench-c.mrc`, 90 s means, at CPU 3 / GPU 2. Three arms: raycaster absent; alive and idle; alive and casting 9 rays at 15 Hz. Compare app GPU (eye pass), compositor GPU, CPU frame time and the "3D fps" line. Watch VrApi for forced level changes. |
| **Power and thermal** | Unknown | Small unless the depth service is new load | The same runs; note battery drain over a long session |

**Rule:** if "alive and idle" costs more than the benchmark's noise floor (~0.3 ms on 90 s means, per `quest-benchmark`), the raycaster is created only on demand and fights never cast. The design above already works that way. Only the optional 0.2 Hz validation depends on the answer.

---

## 6. Costs and risks of adding `USE_SCENE`

| Risk | Detail | Mitigation |
|---|---|---|
| **A consent dialog** | Meta: a "one-time permission explainer dialog, followed by a permission consent confirmation dialog". The wording is unknown. It's about "spatial data" in general, not just "find the table". | Ask on first use, as Meta recommends: when the player first opens the Place Stage scene (or a hold) with "Surface shadows" on, not at boot. Better still, make it an **opt-in launcher row**, "Surface shadows (uses spatial data)", and ask in the 2D launcher. Android grants are per package, so a grant there covers the `:xr` process, and the dialog never interrupts the headset scene. |
| **The OS may prompt automatically at launch** (Meta: "The system will automatically show a permission dialog when the app is launched", for apps on current SDKs) | It may not apply to Khronos-loader apps; unverified. If it does, every player sees it on first launch whether or not they want the feature. | P0 step 1 tests a manifest-only build. If the OS auto-prompts, a "Surface shadows" opt-in can't stop it. Then decide whether the feature is worth an unasked dialog for everyone (D1). |
| **Blocked unattended launches** | Like the Guardian dialog (`guardian-pause-gamepad`), a permission dialog would stop `quest_perf` runs until answered | Grant once in the headset. For scripted runs, `adb shell pm grant dev.melee.game <permission>` should work for a runtime permission; that's standard Android, untested on Horizon OS. |
| **Decline** | Raycaster creation fails (probably `XR_ERROR_PERMISSION_INSUFFICIENT`); after repeated denials Android stops asking | State `Declined` → no shadow, no drop line and no snap. Placement works exactly as today. The launcher row shows "Not allowed — enable Spatial data in Settings". Never re-ask on our own. |
| **It unlocks more than raycast** | From the log, declaring `USE_SCENE` makes the runtime offer `XR_META_environment_depth`, `XR_META_spatial_entity_mesh`, `_room_mesh`, `XR_EXT_spatial_plane_tracking`, `XR_EXT_spatial_marker_tracking`, `XR_META_dynamic_object_tracker` and `_keyboard`. | Offered isn't enabled: we enable only `XR_EXT_future` + `XR_META_environment_raycast`, by name. The upside is that the same grant later serves T8 (environment depth) and parts of T10. Note that plane tracking also needs `XR_EXT_spatial_entity`, which is gated on `USE_ANCHOR_API`, and that `XR_FB_scene` is under `USE_ANCHOR_API`, not `USE_SCENE`. The downside is that the consent covers all of them, so the user is asked about more than this feature uses. Say in-app what we use it for. |
| **Store or review** | Meta's Horizon Store requires apps to justify sensitive permissions and their data use (Data Use Checkup), and spatial data is treated as sensitive user data. I couldn't find the exact requirements for `USE_SCENE`. | This is probably moot. I've assumed this Melee port is sideloaded and never goes through store review; **confirm** (D6). If that ever changes: we read only ray hits, keep nothing beyond one height per spot (plus `gapM` with an anchor), and send nothing off the device. That is an easy declaration. |
| **Device coverage** | Quest 3 and 3S only; the manifest also lists Quest 2 and Pro | `uses-permission` doesn't restrict devices. On Quest 2 and Pro the extension isn't offered, so the state is `Unsupported` and the feature is silently off, with the launcher row hidden. Quest 3S makes depth from its cameras alone, with no depth projector (stated in Meta's spec sheets; not checked for this report), so its accuracy may differ. Test there if one is available. |
| **Permission name drift** | `horizonos.` vs `com.oculus.` | Declare both. Request whichever the package manager knows; P0 tells us which. |

---

## 7. Implementation plan

### P0: probe build (enable, log, look). Nothing user-facing.

Purpose: settle every unknown above on the user's own headset before anything is designed in detail. Everything is behind `AURORA_XR_RAYCAST=probe` (set in `melee-env.txt`), except the manifest lines.

1. **Manifest:** add `<uses-permission android:name="horizonos.permission.USE_SCENE"/>` and `<uses-permission android:name="com.oculus.permission.USE_SCENE"/>` to `platforms/android/app/src/xr/AndroidManifest.xml`.
   - **First run with the probe env *off*:** does a dialog appear on its own at launch? (The auto-prompt question.) Re-read the "OpenXR extensions offered" lines: `XR_META_environment_raycast` should now appear, along with depth, mesh and plane tracking.
2. **Request.** With the env flag on, call `SDL_RequestAndroidPermission` (SDL3 already routes this through `SDLActivity.requestPermission`) for each name, **after** `xrCreateInstance`.
   - Log `checkSelfPermission` for both names before and after. That shows which name is real, and whether one grant covers the other.
   - The dialog itself: note what it says, with a screenshot if possible.
3. **XR side.**
   - Enable `XR_EXT_future` + `XR_META_environment_raycast`.
   - Log `supportsEnvironmentRaycast`, `maxLayerCount`, and the time from the async create to READY.
   - Log the create result **before and after the grant**. That answers whether a grant after instance creation is enough, or whether the permission must be asked before the XR activity starts, for example in the launcher.
4. **The worker** (§5.2) with two modes:
   - **Laser mode:** a ray from each hand's aim pose at 10 Hz whenever the lasers show (pause, hold, Place Stage scene). A **dot quad at the hit** (the existing `dotSwapchain`, tinted green for HIT, yellow for OCCLUDED, red for other statuses) gives a visual accuracy check: the dot should sit *on* the desk or floor in stereo.
   - **Footprint mode:** the 3×3 grid of §2.2 at 15 Hz while `g_placing > 0`. Once a second, log status counts, the consensus `y`, its σ, `coverage`, `h`, `R_eff` and the AO alpha that *would* be drawn.
   - **Timing:** every 10 s, log the call count and p50/p99/max µs per call.
   - **Reference:** create an `XR_EXT_local_floor` space for **logging only**, and print `hit.y − floor.y` for laser hits.
5. **Headset checklist for the user,** one sitting, with `adb logcat -G 64M` running:
   1. Point at the floor in 4 places: `hit.y − floor.y` should be about 0 (target ≤ 1.5 cm).
   2. Point at the desk; measure the desk with a tape: compare `hit.y − floor.y` (target ≤ 1.5 cm).
   3. Place Battlefield about 20 cm over the desk and hold still for 10 s: σ (target ≤ 5 mm), status mix.
   4. Wave a hand under the stage, then rest the hand there for 3 s: does the filter reject it?
   5. Move the stage half off the desk edge: `coverage`.
   6. Look up and away: out-of-FOV counts; the estimate should hold.
   7. Dark, glossy and glass surfaces (a monitor stand, a glass table if there is one).
   8. Dim room light.
   9. A Quest 3S, if one is available.
6. **Benchmark:** `quest_perf.py run --cpu-level 3 --gpu-level 2 --replay bench-c.mrc`, three arms (§5.6), 90 s means, compared as in `quest-benchmark`. Also check for SpaceWarp flapping ("SpaceWarp idle" count) and for a frame-pacing change while casting.

**Exit criteria:**
- Accuracy ≤ 1.5 cm on desk and floor.
- σ ≤ 5 mm when static.
- p99 call time known.
- Idle-raycaster cost known (≈0, or a number).
- The dialog behaviour known.

If accuracy is much worse than 1.5 cm, T1 still works with a softer `a_max` and a wider contact blur. The snap zone then has to grow to match.

### P1: surface estimate (no visuals)

- The worker, the filter and the state machine of §2.4, the cadence table of §2.3, and the `REFERENCE_SPACE_CHANGE_PENDING` handler (only to invalidate the estimate here).
- `aurora_xr_set_stage_footprint` from `xr_scene.c` (floor extents, z from `s_clips` or a default, underside from `CLIP_BELOW` or a small table).
- Log the state changes: "Surface under stage N: known at y …, gap … m, coverage …".
- Replay on desktop: Monado has no raycast, so on desktop the state is always `Unsupported`. That's a useful check that the off path changes nothing.

### P2: T11 placement aids

- Drop line and outline in the hold and the Place Stage scene; magnet snap on release; haptic tick and sound.
- The `SpotSurface` record on adopt (this launch only).
- **Headset look test:** does snap feel right? Is the snap zone right?

### P3: T1 footprint shadow

- Procedural footprint textures (3 blur levels); the AO law; the coverage fade; colour-scale opacity; layer order; budget check.
- **Blind A/B,** the user's method from `spacewarp-late-frames`: shadow on vs off, 2 runs each, in a random order the user doesn't see, labelled 1–4. Then tune `a_max` and blur.

### P4: fighter blobs

- `aurora_xr_set_fighter_marks` with a tick ring, matched to `shownTick`; the stylised drop law; hidden below the surface; dropped first under the layer budget.
- Check on Battlefield edge-guarding and on a stage with a sunk underside (Great Bay at the desk).

### P5: permission UX and persistence

- The launcher row "Surface shadows (uses spatial data)": opt-in, off by default (D1), with the request made in the launcher, and the states Not allowed / Unsupported.
- With anchors (placement UX C.5, phase 2): save `{snapped, gapM}` with the spot, hint only, confirmed by a burst.

### P6: optional

- The coverage-masked texture (§2.6).
- 0.2 Hz validation in fights, if P0 shows a live raycaster is free.
- The "never sink below a known surface" lift (D3(b)), which belongs with tabletop.

---

## 8. Decisions for the user

1. **D1: permission prompt.** Should surface shadows be an opt-in launcher row ("uses spatial data"), with the permission asked in the launcher? Or on by default, asked the first time the stage is placed? If P0 shows the OS prompts at launch anyway, is the feature still worth a dialog every new player sees?
2. **D2: cadence.** Freeze after placement (recommended), or keep validating during fights if it proves free?
3. **D3: other stages over a snapped spot.** (a) Each stage at spot × factor, with its own surface measure (recommended now). (b) Lift any stage that would sink below the known surface (belongs to tabletop).
4. **D4: blobs.** Real-surface blobs only, relying on the stage hiding them when fighters are on-stage (recommended)? Or no fighter blobs at all, footprint only?
5. **D5: `XR_EXT_local_floor` as a shadow receiver.** Use the system floor as a fallback receiver when no raycast exists? This isn't seeding placement, which the user ruled out on 2026-10-03, but it touches that decision. Under the AO law it rarely shows at desk scale.
6. **D6: distribution.** Confirm the Quest build is sideload-only, so store data-use review doesn't apply.

## 9. Open questions that only the headset can answer

- Is `XR_META_environment_raycast` offered once the permission is declared, and under which permission name does the grant register?
- Does the OS prompt automatically at launch? Does a grant that arrives after `xrCreateInstance` allow the raycaster to be created?
- What does a cast cost (p50 and p99)? How long does creation take, and how long until the first valid hit?
- How accurate is a hit on a desk and on the floor, against `LOCAL_FLOOR` and a tape measure? How noisy is it on dark, glossy or glass surfaces, and in dim light?
- Does a live raycaster cost GPU, compositor time or power at CPU 3 / GPU 2, idle and while casting?
- What is `maxLayerCount` on Quest 3?

---

## Sources

**Specification and headers**
- Khronos OpenXR registry pages:
  - [xrPerformEnvironmentRaycastMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrPerformEnvironmentRaycastMETA.html)
  - [XrEnvironmentRaycastHitMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEnvironmentRaycastHitMETA.html) (normal = +Z rotated)
  - [XrEnvironmentRaycastHitStatusMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEnvironmentRaycastHitStatusMETA.html)
  - [XrEnvironmentRaycastHitGetInfoMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEnvironmentRaycastHitGetInfoMETA.html)
  - [xrCreateEnvironmentRaycasterAsyncMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreateEnvironmentRaycasterAsyncMETA.html)
  - [XrEnvironmentRaycasterMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEnvironmentRaycasterMETA.html)
  - [XrEnvironmentRaycastFilterDistanceMETA](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEnvironmentRaycastFilterDistanceMETA.html)
- The spec chapter source: KhronosGroup/OpenXR-Docs, `specification/sources/chapters/extensions/meta/meta_environment_raycast.adoc` (the `XR_EXT_future` dependency, overview, filter semantics).
- Our 1.1.63 headers: `build/android-arm64-xr/_deps/openxr_loader_android-1.1.63/prefab/modules/headers/include/openxr/openxr.h`.
- Device support: KhronosGroup/OpenXR-Inventory, `runtimes/meta_quest_3_mobile.json` and `meta_quest_3s_mobile.json` list the extension; the Quest 2 and Pro files don't.

**Meta**
- [MRUK Instant Placement with Environment Raycasting](https://developers.meta.com/horizon/documentation/unity/unity-mr-utility-kit-environment-raycast/) (live depth buffer, depth-camera frustum, Quest 3/3S, "Depth API no longer needed" as of MRUK v81)
- [MRUK EnvironmentRaycastHitStatus, v207](https://developers.meta.com/vr/reference/mruk/v207/enum_meta_x_r_environment_raycast_hit_status) (NotReady: "several frames")
- [IWSDK environment raycast](https://developers.meta.com/horizon/documentation/web/iwsdk-guide-environment-raycast/) ("does not require room scanning")
- [Instant Content Placement motif, blog](https://developers.meta.com/vr/blog/mixed-reality-motif-instant-content-placement-virtual-objects-project-setup/) and [docs](https://developers.meta.com/horizon/documentation/unity/unity-mrmotifs-instant-content-placement/) (downward ray, grounding shadow, offset on release)
- [Native spatial data permission](https://developers.meta.com/horizon/documentation/native/native-spatial-data-perm/) (explainer and consent dialogs, automatic prompt on current SDKs, request on use, fallback)
- [Unreal spatial data permission](https://developers.meta.com/horizon/documentation/unreal/unreal-spatial-data-perm/) (automatic request when a feature needs it)
- [Native Depth API](https://developers.meta.com/horizon/documentation/native/android/mobile-depth/) (0.2 m near limit, `USE_SCENE`, hand removal)
- [Depth API overview (Unity)](https://developers.meta.com/vr/documentation/unity/unity-depthapi-overview/) (Quest 3/3S only, passthrough required)
- [Passthrough occlusions (Unity)](https://developers.meta.com/horizon/documentation/unity/unity-customize-passthrough-passthrough-occlusions/) ("might be too expensive", latency)
- Meta samples: oculus-samples/Unity-PassthroughCameraApiSamples, `EnvironmentRayCastSampleManager.cs` (`com.oculus.permission.USE_SCENE`, `IsSupported` check); meta-quest/Meta-OpenXR-SDK (XrPassthroughOcclusion: request before instance creation). I found **no native Meta sample** that uses `xrPerformEnvironmentRaycastMETA`.
- Unity's Meta OpenXR package (needle-mirror/com.unity.xr.meta-openxr, `Runtime/Subsystems/Raycast/ARRaycastFeature.cs`): enables `XR_EXT_future XR_META_environment_raycast` and checks the system capability.
- [Meta forum: POW level stuck at 3 with Depth API](https://communityforums.atmeta.com/quest-development-128/quest-3-pow-level-stuck-at-3-passthrough-depth-api-151541) (anecdotal)

**Repo**
- `build/quest-perf/ext-warmup.log` (runtime 209.113.0: the skip lines, `XR_EXT_future v2` offered)
- `extern/aurora/lib/xr/xr.cpp`: extension scan l.671–720; `xr_proc` l.604; system properties l.742; `publish_views` l.2449; `ArenaPose` l.344; `set_arena_pose` l.2583; `to_game` l.2597; `end_grab` l.2877; `create_static_swapchain` l.3233; `add_pointer_quad` l.3422; layer assembly l.3955–4090; `poll_events` l.4197; teardown ~l.4335; `aurora_xr_adopt_stage` l.6755
- `src/pc/xr_scene.c`: `s_clips` l.241, `stage_top` l.1234, `pc_xr_stage_part_begin` l.1255
- `src/pc/xr_place.c`: `pc_xr_place_hold` l.195
- `platforms/android/app/src/xr/AndroidManifest.xml`; `SDLActivity.requestPermission`
- Docs: `docs/xr-stage-grounding-report.md` (T1, T7, T8, T10, T11, §7), `docs/xr-stage-placement-ux.md` (A.1, A.4, C.1–C.5)
- Memories: `placement-calibration-plan`, `stage-default-placement`, `stage-placement-hold`, `quest-benchmark`, `spacewarp-late-frames`, `guardian-pause-gamepad`, `mr-modes-model`
