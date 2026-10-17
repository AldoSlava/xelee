# XR stage placement: where, when and how

A design review, written 2026-10-16 against `quest-xr` at bc2bb00. It changes no code.

**Scope.** This covers **MR floating** only, the one mode we ship. VR and MR tabletop appear only as forward-compatibility notes.

**Question.** We now have a "set it once" placement for netplay, the Slippi Place Stage flow. How should that idea extend to the whole game? Where, when and how should a player set placement, and what role is left for per-stage adjustment?

**Constraints.**
- No added input latency.
- Netplay stays render-only: no sim state, wire or tick timing changes.
- Few interruptions, but placement is easy to reach.
- The UI is Melee-styled.
- Quest GPU and CPU headroom is tight.

The document has four parts:
- **Part A:** the facts as built.
- **Part B:** the design space, along five axes.
- **Part C:** the recommendation, the trade-offs rejected, and wireframes.
- **Part D:** decisions for the user.

Earlier user decisions are cited by date. Where I recommend changing one, the change is marked **[changes a decision]** with the reason.

---

## Part A: what exists today

### A.1 The pose pipeline

A stage's 3D placement is `A = T(pos) · R_y(yaw) · S(scale)`, from game units to the room. It's applied around a per-stage **center point** in game units: the game point that sits at `pos`. It lives in `extern/aurora/lib/xr/xr.cpp`:

| State | What it is | Lifetime |
|---|---|---|
| `g_defaultArena` | `AURORA_XR_ARENA_POS/_YAW/_SCALE`, default `(0, -0.25, -0.7)` m, yaw 0, 0.0035 m per unit (desk-tuned, 7042d83) | Constant |
| `g_adoptedArena`, `g_adoptedSet` | The **global base**. Written only by `aurora_xr_adopt_stage()`, which takes the current arena and divides out the stage's factor, so it's stored "before any stage's size". | Process memory: **lost at exit** |
| `g_stageArenas[grkind]` | Per-stage grabs, as **absolute LOCAL-space poses** (`set_arena_pose`) | Session |
| `g_stageScale`, `g_arenaCenter` | The current stage's factor and center point, from `aurora_xr_set_stage()` | Per stage load |
| `g_arenaToHead` | No base and no grab: the XR thread places the default **in front of where the head is and looks** when the stage is set (l.3839, since 1d3d849) | One-shot |

`aurora_xr_set_stage(stage, center, factor)` (l.6820) resolves where a stage appears, in priority order:
1. this stage's session grab;
2. otherwise the adopted base × factor;
3. otherwise the default × factor, placed at the head.

`aurora_xr_reset_stage()` drops the grab and goes back to (2) or (3).

**`aurora_xr_adopt_stage()` clears every per-stage grab** (`g_stageArenas.clear()`). It has to: the grabs are absolute poses, so they can't follow a new base.

Grabbing is position plus **yaw only** (pitch and roll never change). Scale is clamped to 0.0015–0.05 m per unit, which puts FD's floor between about 25 cm and 8.5 m wide. The grab box is ±120 × −80..100 × ±60 game units around the center.

**Reference space.** The game uses `XR_REFERENCE_SPACE_TYPE_LOCAL` only (l.1091). `poll_events()` (l.4196) handles session state, refresh rate, instance loss and boundary visibility. **It ignores `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`.** Since every stored pose is in LOCAL, a system recenter moves the adopted base and all grabs along with the new origin.

### A.2 Per-stage data (`src/pc/xr_scene.c`)

- **`s_placements[]`** (l.373): grkind → center point plus size factor. The factor is about `sqrt(171 / floor width)`, halfway between true proportions and "every floor as wide as FD's" (8231c90). Exceptions are hand-tuned: Temple 0.55, Corneria centered on the Great Fox deck at y 285, Peach's on the roof at y 84, Mute City pushed back 40 in z. Stages not listed sit at the world origin at factor 1.
- **Automatic per-stage presentation that already exists** and works without the player doing anything:
  - part hides (`s_builtin_rules`);
  - moves (`s_moves`: Stadium's screen, Great Bay's sea, Yoshi's Island's clouds);
  - clip boxes (`s_clips`, `CLIP_*`);
  - level-gated hazards (`s_levels`: Brinstar's acid);
  - follow windows (`s_follows`, which move the center with `aurora_xr_set_stage_center` and zoom with `aurora_xr_set_stage_zoom`);
  - HUD height from the stage's top floor (`stage_top` → `aurora_xr_set_stage_top`), and HUD-on-top for follow stages.
- `MELEE_XR_CENTER` and `MELEE_XR_STAGE_SCALE` override the stage being played, for tuning.

### A.3 Where per-stage adjustment happens

1. **The pre-fight hold** (`src/pc/xr_place.c`):
   - `pc_xr_stage_part_begin` measures the stage's top floor. Once the collision is in, it calls `pc_xr_place_stage_ready(grkind)`.
   - `gm_801A4D34` (`gmscene.c:432`) then calls `pc_xr_place_hold()` once per pass. While it returns true, **no `gm_RunSimTick` runs** and the pads are flushed (`HSD_PadFlushQueue`).
   - The hold is offered **every fight** (d064a16): `pc_xr_stage_load()`, called from `Ground_801C0754`, resets `s_top_grkind`, so the next measurement re-offers it.
   - Conditions: an XR session is already presenting (be20acd), `MELEE_XR_PLACE` isn't 0, and **not `pc_net_active()`**. All-Star's rest area (0x42) holds once a session.
   - Grabs write `g_stageArenas` for this stage only. A/Start releases on button-up, B resets, Y toggles the cards. The cards open on every hold, follow the head sideways (20° dead zone), and the HUD is hidden.
2. **The pause grab** (6b6f5a1): `pc_xr_world_camera` passes `gm_GetDbPauseFlag(1) || gm_GetDbPauseFlag(2) || gm_vs_soft_paused()` to `aurora_xr_set_paused()`. While paused, the grips grab the arena instead of pressing Z. This is the current stage only, with no cards. In netplay it works under the Unranked soft pause and Direct's synced pause (cb83238), and it's render-only.
3. **The Place Stage scene** (f9467be, ca31c5a):
   - Entry: `ONLINE_KIND_PLACE` in `gmonlinemode.c` → `gmVsMode_PlaceDummyEnter`.
   - The scene: tinted Battlefield, red Fox and Falcon idling. The sim keeps ticking but the pads never reach the fighters.
   - **A/Start → `aurora_xr_adopt_stage()`**, then return to the menu or go on to the mode.
   - **B → reset only**: the scene stays. In the code there's no "skip"; an untouched A adopts the head-relative default. (The memory file says "B skips", which is out of date since ca31c5a.)
   - Entry points: the Slippi **Place Stage** row (`mnonline.c`, only when `pc_xr_place_available()`), and `enterAfterPlacing()` before the **first Unranked or Direct of a launch** (`pc_xr_place_decided()`).
   - **LAN goes straight to `enterOnline(ONLINE_KIND_LAN)`**, so it never auto-places.

### A.4 Menus, launcher and platform

- **Main menu:** 1P, VS, Online Mode (PC), Trophies, Options, Data.
- **Options** (`forward.h`): Rumble, Sound, Screen Display, **`SEL_SETTINGS_3` (hidden in retail)**, Language, Erase Data. `mnmain.c` already draws PC labels in place of disc text for the Online rows (`mnOnline_Label`, l.1037).
- **Launcher:** on Quest it's a **2D panel in a separate process** from `MeleeXrActivity`, with no XR session. Its Quest-only rows are Resolution and SpaceWarp. It has no placement rows.
- **VR is not user-reachable** (1d3d849; `vi.c:240`, Select does nothing in XR).
- **Permissions** (the "skipping extension" lines in `build/quest-perf/ext-warmup.log`):

| Gated on a manifest permission we lack | Offered today, no new permission |
|---|---|
| **`horizonos.permission.USE_ANCHOR_API`**: `XR_FB_spatial_entity`, `_storage`, `_query`, `_container`, `XR_META_spatial_entity_persistence`, `_discovery`, **`XR_FB_scene`**, `XR_FB_scene_capture`, `XR_EXT_spatial_entity`, `_anchor`, `_persistence` | **`XR_EXT_local_floor`** (floor height) |
| **`horizonos.permission.USE_SCENE`**: `XR_META_environment_raycast`, `XR_META_environment_depth`, `XR_EXT_spatial_plane_tracking`, the room and space meshes | **`XR_EXT_user_presence`** (headset on or off) |
| `IMPORT_EXPORT_IOT_MAP_DATA` and `USE_COLOCATION_DISCOVERY_API`: sharing and colocation | `XR_META_boundary_visibility` (in use) |
| | Core: the `STAGE` space and `xrGetReferenceSpaceBoundsRect` (play-area rectangle) |

Note the permission's name: the runtime asks for `horizonos.permission.USE_ANCHOR_API`, not the `com.oculus.permission.USE_ANCHOR_API` in Meta's older docs. Meta documents `USE_SCENE` as a **runtime permission** with a consent dialog, to be requested only when the feature is used, with a fallback ([Meta: spatial data permission](https://developers.meta.com/horizon/documentation/native/native-spatial-data-perm/)). I found nothing documenting whether the anchor permission prompts; treat it as unverified.

### A.5 Decisions already on record

| Date | Decision | Source |
|---|---|---|
| 2026-10-03 | Calibration idea: a floor plus a Battlefield ghost; stages anchor to the floor or to platform height. Don't seed from LOCAL_FLOOR/STAGE; the default is head-relative. **The spatial anchor is the persistence mechanism, not polish.** Per-stage offsets are keyed by anchor UUID plus stage. Several anchors may exist. | placement-calibration-plan |
| 2026-10-03 | Three modes. Tabletop has no per-stage placement overrides and no free move or scale. MR floating has free move and scale plus per-stage overrides. | mr-modes-model |
| 2026-10-04 | A grab places only the current stage, for the session; not persisted. | 17bafb3 |
| 2026-10-05 | Desk defaults (0.0035, 0.7 m out, 0.25 m down). | 7042d83 |
| 2026-10-05 | The hold isn't Melee's pause; netplay never holds; placement is presentation only. Hold only if XR is already presenting. | 7b717fa, be20acd |
| 2026-10-09 | **Every fight holds**: "sometimes on, sometimes off" was confusing. The cards open every time. | d064a16 |
| 2026-10-14 | Slippi: a single Battlefield dummy writes the **global** base; it runs on the first Unranked or Direct of a launch; **no anchor yet and no "same spot?" interim: a placement is required per launch**. **Only Place Stage writes the global base**; holds place individual stages; the soft pause never adopts and shows no cards. | slippi-stage-placement-flow |
| 2026-10-04 → 10-09 | Hiding effects is preferred over dropping resolution (KO effects). This reflects how much perf headroom matters. | ko-explosion-cost |

---

## Part B: the design space

### Axis 1: the global default base

**What it is.** In MR floating, the base answers one question: *where does "the stage" go in my room, and how big is it?* It has four parts.

| Component | Today | Recommendation |
|---|---|---|
| **Origin** (3D point) | `g_adoptedArena.pos`: where every stage's **center point** (its main floor, `s_placements`) goes | Keep. Defining the origin as "the floor the fighters stand on" is what players grab and judge by. |
| **Facing** (yaw) | `g_adoptedArena.yaw`; no pitch or roll | Keep. Pitch or roll on a fighting stage only confuses. |
| **Scale reference** | `g_adoptedArena.scale`, meters per unit before each stage's factor | Keep. One number makes every stage's size follow from FD's. |
| **Surface height** | None. In floating mode the origin *is* the height. | **Don't add one for MR floating.** A separate surface (floor or table) and a per-stage "sits on the surface or floats at platform height" rule only mean something when there's a real surface under the stage, which is tabletop. Keep that for the tabletop work (the calibration plan's `CenterRule`-style table). |

**What it holds besides the pose:**
- a **mode key** (`mr-floating`), so future modes get their own bases;
- once persistence lands, an **anchor UUID**.

It deliberately does **not** hold the menu screen's pose. The screen and arena keep separate poses (current design), and the screen is head-relative at 0.85 m on each launch.

**Where it's set.** Per the 2026-10-14 rule, only by the explicit Place Stage flow. That's the right call. A Battlefield dummy with two fighters is a *neutral reference*: average size, factor 1.1, scale cues from the fighters. Adopting from Temple (0.55) or Mute City's diorama would produce a base the player didn't mean, even though the math divides the factor out.

**How often.**
- Today: once per launch, for netplay only. Offline, the base is never set, so every first appearance of a stage is head-relative and the hold is the only placement.
- Target: **once per room.** That needs an anchor, and the anchor needs a manifest permission (Axis 4).

### Axis 2: per-stage adjustment

**Traced in the code (A.3):**
- the hold before every fight (session grab);
- the pause grab (session grab);
- the static factors and centers (`s_placements`);
- automatic per-stage presentation (clips, moves, follow windows, HUD height).

The first two are manual and cost the player time. The last two are free.

**How necessary is manual adjustment once a good base exists?** Take the reasons a player would grab a particular stage, one at a time:

| Reason | Real? | Better handled by |
|---|---|---|
| **Size**: the stage feels too big or small next to others | Sometimes. The sqrt rule is a compromise by design: Temple reads big, FoD small (8231c90). | Tune the factor once in the headset (data). A player who wants *everything* bigger re-places the base, not each stage. |
| **Vertical extent**: the action sits above or below the main floor | Yes, for tall stages: Icicle's box spans y −126..140, Rainbow Cruise −66..198, Mute City −110..260, Corneria's center is 285 up, Peach's castle hangs under the roof. Pinning "main floor at the origin" can put the busy part of the box above eye level or into a desk. | **The automatic route:** a per-stage *vertical* center in `s_placements` (the y is already there). Tall stages center on their box's mid-height rather than the main floor. |
| **Floor or no floor**: ground stages have scenery below the main floor, floating ones don't | In floating mode it's cosmetic: scenery below just hangs there. It matters when a real surface is under the stage, which is tabletop. | Already handled by clips (Great Bay's turtle at y 0, Yoshi's Island at −40, MK at −40). No player action needed. |
| **Camera framing**: the flat camera framed the fight off-center | Handled by centers: Great Bay x −10, Kongo +5, Mute City z +40. | Data. |
| **Depth toward the player** (Mute City, Poké Floats z ±120) | Occasionally: the stage comes close to the face at desk distance. | Data (center z), or a base placed a little farther. |
| **Comfort shift**: the player moved seats mid-session | Yes, but it's **not per-stage**. Everything should move. | Re-place the base, or recenter (Axis 4, Part C.5). |

**Conclusion.** Once the base is good, manual per-stage adjustment should be **rare and optional**. Most of what's left is data we can tune once. That's why the every-fight hold becomes the main interruption once a base exists, and why Part C makes the hold opt-in.

**How per-stage grabs should relate to the base:**
- Store them as **deltas from the base** (`base⁻¹ · arena`), not absolute LOCAL poses. Then re-placing the base, recentering and anchor corrections carry every grab along, and `adopt` no longer *has to* clear them. Whether it *should* clear them is decision D4.
- They stay **session-only**, per the 2026-10-04 decision. The calibration plan's "persist per anchor + stage" is a later option and needs anchors first.
- Reset: B in the hold already resets this stage to the base. "Reset all stages" belongs in the Place Stage scene (C.3).

### Axis 3: types of stages

Grouped by how each stage sits in space. These come from `s_placements`, `s_clips`, `s_moves`, `s_follows` and `s_levels`. Sizes are at the desk base (0.0035 m per unit) × factor.

| Group | Stages (grkind, factor) | How it's built today | What the base should map to | What a player might adjust |
|---|---|---|---|---|
| **1. Compact floating platforms**, the tournament set and its relatives | BF 0x24 (1.1), FD 0x25 (1.0, floor about 0.6 m), DL 0x1C (1.05), YS 0x0A (1.15; pillar cut below −40), FoD 0x0C (1.15; vortex hidden, pole cut −80), **Pokémon Stadium** 0x10 (1.0; bowl hidden, column cut, screen pulled in to z −75 at 0.55), Flat Zone 0x1B (1.1), Yoshi's Island 64 0x1D (1.0), Venom 0x0F (0.95), Green Greens 0x0D (0.9) | One platform group in empty space, about 0.5–0.7 m wide | **Main floor at the origin.** This is the case the base is defined by. | Almost nothing. **All six Slippi Unranked stages are here, with factors 1.0–1.15**, so in netplay the base alone covers every stage the server can pick. |
| **2. Stages with a ground or scenery below** | Peach's 0x02 (0.8, roof center at y 84, castle body below), Kongo Jungle 0x04 (1.0; waterfall cut −70 with fade), Jungle Japes 0x05 (0.9; river bounded), Great Bay 0x06 (0.85; sea moved under, turtle and stilts cut at 0), Onett 0x14 (0.8; block boxed), Fourside 0x15 (0.8; three buildings), Mushroom Kingdom 0x18 (0.9; cut −40), MK II 0x19 (0.8; cliffs boxed ±150, cut −50), Yoshi's Island 0x0B (0.9; ground cut −40), Brinstar 0x08 (1.1; acid level-gated, boxed), KJ64 0x1E (1.0) | A floor or terrain hangs a little below the fight, trimmed to a block | Main floor at the origin. The block below hangs in the air in floating mode. **This is the group that tabletop will ground on the surface** (forward note). | Height, if the block below feels too low or tall at a desk. Tune centers rather than make players grab. |
| **3. Tall or vertical** | Icicle Mountain 0x16 (0.8; box y −126..140, about 0.74 m tall), Rainbow Cruise 0x03 (0.85; −66..198), Poké Floats 0x11 (0.85; −101..152, z ±120), Mute City 0x12 (0.8; −110..260, about 1 m tall), Corneria 0x0E (0.8; Great Fox fins), Brinstar Depths 0x09 (0.95; Kraid cut at −150), Escape from Brinstar 0x21 (follow window, HUD on top), **Break the Targets** 0x28–0x41 (**not in `s_placements`**: origin, factor 1, not surveyed) | Diorama boxes, or the real height of the stage | **The vertical middle of the action**, not the main floor. Data change: give these stages a center y near their box's mid-height, so the base's height is "where the action is". | Height and scale. These are where a player's grab is most likely. |
| **4. Moving: fixed window (diorama)** | Big Blue 0x13 (0.85; track boxed ±220, dissolving), Mute City, Rainbow Cruise, Poké Floats, Icicle | The fight stays put; the world streams through a clip box | Box center at the origin (as now) | Rarely. The box is what you place. |
| **5. Moving: following window** | Race to the Finish 0x27, Adventure courses 0x1F–0x22, Home-Run Contest 0x43 (held on the platform until the bag leaves) | Window `half` 160×110×80 follows the camera's focus; the center slides (`aurora_xr_set_stage_center`), the placement never moves | The window's center at the origin | Nothing useful: the window owns framing. A grab moves the window, not the course. |
| **6. Very wide** | Hyrule Temple 0x07 (0.55, "smaller than the rule gives, to fit in view"; geometry-bound on GPU, docs/quest-xr.md perf runs), also Great Bay and Kongo Jungle's wide floors | Shrunk to fit | Main floor at the origin | Scale (bigger for a standing room). **Perf caution:** Temple is the heaviest stage, and a larger arena adds fill on top of its geometry cost. |
| **7. Mode one-offs** | All-Star rest area 0x42 (0.8; meadow boxed; held once a session), Snag the Trophies 0x26 (0.95), Adventure trophy fights 0x44–0x46 (**unlisted**), Multi-Man and Fighting Wire Frames (BF/FD kinds) | Mixed | Main floor at the origin | Rare |

**What this means for placement:**
- **Netplay is the easy case.** The legal Unranked set is all group 1, so one base is enough. No per-stage step is ever needed online, which is consistent with "no hold in netplay".
- **1P modes carry the long tail:** groups 3, 5 and 7, plus two **unsurveyed families**, Break the Targets (26 stages) and the trophy fights. These sit at the origin at factor 1 until someone surveys them. That's a data gap, and no placement UI can fix it.
- **Raising tall stages to their action** (group 3) is the single biggest way to make per-stage grabs unnecessary, and it costs nothing at runtime.

### Axis 4: player environments

There's one headset, and the player usually holds a gamepad (bare hands can't start a fight, by design: 2026-10-06).

| Environment | Where the stage naturally goes | How the base should adapt | Detect or ask |
|---|---|---|---|
| **Seated at a desk** | 0.6–0.8 m ahead, 0.2–0.4 m below the eyes, a little above the desk; FD about 0.5–0.7 m. This is today's tuned default (7042d83). | Default as is | Today's default *is* this case. Desk height could only be detected with `USE_SCENE` (a table) or raycast. |
| **Couch** | Farther and bigger: about 1.2–1.8 m out, FD about 1–1.5 m, at or a bit below eye level (over a coffee table or floating) | A preset starting point (C.3) so the player doesn't need several two-hand pulls to scale ×2–3 | Head height above the floor (`XR_EXT_local_floor`, no permission) separates seated from standing, but not desk from couch. **Ask with a preset, then fine-tune by hand.** |
| **Standing, living room** | 2–3 m out, FD about 2–3 m, waist to chest height; the player may walk around it | Preset "Room" plus hand tuning | Head height (local floor), play-area size (`STAGE` bounds rectangle, core OpenXR, no permission) |
| **A table present** | On the table: that's the tabletop mode's job | Floating mode doesn't snap. The player can still hand-place it at table height. | Needs `USE_SCENE` (a runtime dialog) plus the user's Space Setup, or environment raycast (also `USE_SCENE`). **Not now.** |
| **No table** | Floating | — | — |
| **Small room or stationary boundary** | A large stage pokes through walls. In MR, content draws over passthrough with no occlusion, so it looks pasted on the wall. | Cap the preset at the play area's size | The `STAGE` bounds rectangle is the cheap signal (a stationary boundary is a small square). Walls need `USE_SCENE`. |
| **Large room** | Anything | — | — |
| **Boundary** | We suppress the boundary in MR (`BOUNDARY_VISIBILITY`, 1ee5d8a) | Room-scale presets invite walking with no visible boundary. **Keep "Room" modest** (about 2 m FD) and don't encourage walking around big stages. | — |
| **Others in the room, one headset** | Couch multiplayer: others play with pads and watch a cast of the headset view | The cast is the player's view, so a centered, head-facing base matters more than a clever one | Can't detect, and doesn't need to be |
| **Two headsets in one room** (LAN or Direct, colocated) | Ideally one shared stage | Needs shared anchors or colocation (`IMPORT_EXPORT_IOT_MAP_DATA`, `USE_COLOCATION_DISCOVERY_API`) | **Forward note only** |

**Quest performance and environment.** Placement itself costs nothing per frame: it's a matrix. But the base's *scale* changes how much of the view the stage covers. A couch or room base costs more fill in the eye pass (MSAA is about 3.7 ms of it; quest-benchmark memory), and dynamic resolution then gives up sharpness. So presets should be **bounded**: FD at most about 2 m in "Room", well below the 8.5 m clamp. And Temple should stay small. (This follows from how fill scales; I didn't measure it for this document.)

**Detect versus ask.** Ask with **one** gesture, the placement itself, plus at most a preset choice. Every environment property that matters (distance, size, height, facing) is captured by where the player puts the dummy. A questionnaire ("Are you seated? Is there a table?") would add an interruption and save nothing. Detection is worth adding only where it *removes* work:
- local floor and play-area size to pre-select a preset;
- anchors to skip placement entirely in a known room;
- later, the table for tabletop mode.

### Axis 5: netplay versus offline

| Flow | Sequence | Time available for placement | Sim-safe steps | Today |
|---|---|---|---|---|
| **Slippi Unranked** | Menu → CSS → **search** (seconds to minutes) → server picks the stage (random legal) → VS splash → fight | Before entering; during the search (but the CSS scene is running); none between stage reveal and fight | Before the mode (dummy scene); soft pause in-fight (render-only, match runs on) | Auto-run on the first Unranked of a launch; Place Stage row; soft-pause grab |
| **Slippi Direct** | Menu → CSS → connect → **SSS** (loser picks) → fight → back to CSS (link stays up) | Before entering; between games the player is in the CSS/SSS of a live lobby | Before the mode; Melee's real synced pause (render-only grab) | Auto-run on the first Direct; row; pause grab |
| **LAN** | Menu → LAN lobby → … → fight | Before entering | Before the mode; pause | **No auto-run** (`enterOnline(ONLINE_KIND_LAN)` bypasses `enterAfterPlacing`) |
| **VS (offline)** | CSS → SSS → load → **fight frame 0** | SSS, load, and an unlimited hold at frame 0 (sim-free) | Hold, pause, anything before | Every fight holds |
| **1P Classic, Adventure, All-Star, Event** | CSS → a sequence of stages with splashes; no SSS | Unlimited hold at each stage's frame 0 | Hold, pause | Every stage holds (Classic and Adventure: many per run) |
| **Stadium** (Targets, Home-Run, Multi-Man) | CSS → fight, with retries | Hold per load | Hold, pause | Every attempt holds (a Home-Run retry reloads the stage) |
| **Training** | CSS → SSS → fight | Hold, and Training's own menu | Hold; the menu (**unverified** whether it sets the pause flag that enables the grab) | Holds |
| **Replays** (`.slp`) | — | Hold (sim-free, so the replay can't tell) | Hold, pause | Holds |

**Why netplay can't hold, in brief.** The peer, which may be Slippi Dolphin, starts on the agreed frame. Any local wait would stall rollback or desync timing. Placement stays render-only: it touches no sim state, wire or ticks (7b717fa).

**Placement steps that fit netplay:**
1. **Before the mode** (built). This is the only point where the player has unlimited time.
2. **The in-match pause** (built). It's render-only, but it's the player's own pause, which in Direct the opponent sees.
3. Rejected:
   - **Placement during the search:** the CSS is a different scene; it would need a scene-free XR ghost (C.6).
   - **A grab during "Ready… GO!":** the grips map to Z, the window is about a second (not measured), and a grip still held at GO acts as Z.
   - **A "Place Stage" action from the live Direct CSS:** it would mean a scene change with the link open.

**Offline** has unlimited sim-free time at every stage's frame 0. The question there isn't *whether* we can place, but *whether we should stop the player every time*. Axis 2 says usually not, once a base exists.

---

## Part C: recommendations

### C.1 The model in one paragraph

**One base per room**, called **"your stage spot"** in the UI. It's set with the Battlefield dummy and captures origin, facing and scale. **Every stage starts there**, sized by its factor and centered by its center point. Tall stages get centers at their action's mid-height (data). **Per-stage grabs are optional exceptions**: session-only, stored as deltas from the spot, made from the pause (or an opt-in hold). The spot is **persisted with a spatial anchor** once we add `horizonos.permission.USE_ANCHOR_API`. Until then it lasts for the launch, as the user decided for Slippi.

### C.2 Where: entry points

| Entry | Role | Status |
|---|---|---|
| **Auto-run before the first fight mode of a launch** when no usable spot exists: 1P Mode, VS Mode, Online (Unranked, Direct, **and LAN**) | **Primary.** This is the Slippi `enterAfterPlacing` pattern applied to every fight mode. It runs just before the player needs the spot, never at boot, and never for Trophies, Options or Data. | Extends f9467be. Adds LAN. |
| **Options → Stage Spot** row (XR only) | **The permanent offline home.** Options is where Melee keeps how the game is presented; the hidden `SEL_SETTINGS_3` slot is free; `mnOnline_Label`'s mechanism draws custom labels. | New |
| **Slippi → Place Stage** row | Permanent online home, unchanged | Built |
| **Pause grab** (offline pause, Unranked soft pause, Direct synced pause) | **The per-stage exception path.** Already learned, render-only, available in every mode. | Built. Add a legend line (C.7). |
| **Pre-fight hold** | **Opt-in**, via the setting "Before each fight". | **[changes a decision]** (C.4) |
| **Launcher** | Status and policy only: "Stage spot: saved / not set", **Forget**, "Before each fight" | New. The launcher can't place (A.4). |

**Rejected:**
- **A boot or onboarding step:** it delays menus, and the session may not be presenting yet, while the hold rule wants XR already presenting.
- **The SSS as the primary entry:** 1P modes have no SSS.
- **A dedicated gesture:** the pause *is* the gesture offline; online, the grips are Z.
- **Placing from the launcher:** a 2D panel with no XR.
- **The first hold of a launch writing the base:** it bends the 2026-10-14 rule, and the base would come from an arbitrary stage.

### C.3 How: the Place Stage scene, extended

Keep the scene: real Battlefield, two idle fighters, the cards, the gestures, A to adopt. Add three things:

1. **Size presets** to start from, with no detection required. They're shoulder chips in the legend, changed with **L/R** (which select nothing else in the scene), and each places the dummy head-relative:
   - **Desk**: today's default, 0.0035 at 0.7 m.
   - **Couch**: about 0.007 at 1.4 m, 0.35 m below the eyes.
   - **Room**: about 0.011 at 2.0 m, 0.6 m below, capped by the `STAGE` bounds when known.

   The values are proposals to tune in the headset. If `XR_EXT_local_floor` reports a standing head height, pre-select Room; otherwise Desk. The presets only seed the pose; the player fine-tunes by hand as now. **New**; it doesn't contradict the 2026-10-03 "don't seed the *floor* from LOCAL_FLOOR" decision, because it seeds no surface, only the preset choice. Flagged as D3 anyway.
2. **One caption** that says the scope: "Every stage starts here." (f9467be already has "one caption line".)
3. **X: reset every stage to this spot.** Today `adopt` always clears grabs; with delta storage it's a choice (D4).

Keep **B = reset this placement** (current code). Leaving without placing is an untouched **A**, which adopts the head-relative default. Say that on the legend ("A: Done").

### C.4 When: prompts, defaults, re-prompts

**Default for "Before each fight": Start right away** (no hold) once a spot exists this launch or room. **[changes the 2026-10-09 decision]** Reason: d064a16 made every fight hold because *sometimes on, sometimes off* was confusing, at a time when there was no global base offline. With a base, holding every fight is the largest recurring interruption: Classic and Adventure hold many times per run, and the Home-Run Contest every retry. Axis 2 shows that per-stage adjustment is rarely needed. The predictability the user wanted is kept: the setting is always-or-never, never conditional. "Place the stage" restores today's behaviour exactly.

The one exception is the existing All-Star rest-area rule (once a session); keep it as is.

**Auto-prompt (run the Place Stage scene) only when the game can't place the stage where the player left it:**

| Situation | Detect with | Prompt? |
|---|---|---|
| No spot this launch, no anchor support | `pc_xr_place_decided()` false | **Yes**, once, at the first fight mode (C.2) |
| Anchor saved and found | `xrDiscoverSpacesMETA` → locatable → `xrLocateSpace` valid | **No.** The stage simply appears there. |
| Anchor saved, not yet located | Locate flags invalid | Wait. Localization is asynchronous and can need the user to look around; the menus give that time. Re-check at the first fight mode. |
| Anchor not found by then (different room) | Still invalid | **Yes**, captioned "Can't find your stage spot here". Save the new anchor **beside** the old one (the calibration plan: several anchors). |
| System recenter | `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` (LOCAL). The spec says the runtime "must queue" it on recenter. | **No.** See C.5 for what the stage does. |
| Recenter with `poseValid = false`, spot not anchored | The same event | Mark the spot unusable; prompt at the next fight-mode entry |
| Tracking loss | `POSITION_TRACKED` cleared while `VALID` stays set; the spec says runtimes keep inferred poses and snap back | **No** |
| Headset off/on, sleep | Session state; `XR_EXT_user_presence` (offered) | **No** |
| Every launch, every session, every N fights | — | **Never** |

### C.5 Persistence and recentering

- **Phase 1 (now, no new permission):**
  - the spot lasts for the launch, in LOCAL, matching the 2026-10-14 rule;
  - add the `REFERENCE_SPACE_CHANGE_PENDING` handler, so a recenter does something deliberate (D5);
  - store grabs as deltas.
- **Phase 2 (`horizonos.permission.USE_ANCHOR_API`):**
  - create an anchor *at the spot* (well within Meta's 3 m accuracy guidance), save it (`xrSaveSpacesMETA`) and store `{mode, uuid, scale, last-used}` in a small file;
  - at launch, discover the saved UUIDs and use the most recently used one that localizes;
  - re-placing in the same room (old anchor located and within about 3 m) replaces it; otherwise both are kept; prune the least recently used past about 8, erasing each anchor and its grabs;
  - per frame, the XR thread does one `xrLocateSpace`: no game-thread or input-path work. Ease large anchor corrections over a few frames.
  - The launcher's **Forget** deletes the file; the game erases orphaned anchors at its next start.
- **Not recommended:** persisting the LOCAL pose across launches. Quest's LOCAL origin is the last system recenter (the comment at `aurora_xr_set_stage` relies on this), so a pose saved in LOCAL would often survive a restart. But a recenter done in Home while the game isn't running is never reported to us, and the stage would silently appear in the wrong place. The user also rejected a "same spot?" interim.
- **Not now:** `USE_SCENE`. It's a runtime consent dialog, Scene also needs the user's Space Setup, and its value (tables, walls) belongs to tabletop mode.

### C.6 Flows by mode of play

| Mode | First fight of a launch, no spot | Later fights | Exceptions |
|---|---|---|---|
| **VS** | Place Stage scene, then CSS | Start right away at spot × factor | Pause grab; "Before each fight: Place the stage" for players who want the hold |
| **1P** (Classic, Adventure, All-Star, Event, Stadium) | Place Stage scene on entering 1P Mode, then CSS | Start right away. Every Classic, Adventure and Home-Run stage uses the spot. | Pause grab; rest area as now. **Survey the Target Test and trophy stages** (data). |
| **Training** | As for 1P (Training is under 1P Mode) | Start right away | Pause or Training menu grab (**verify** the flag) |
| **Replays** | — | Start right away (a hold is harmless if on) | Pause |
| **Unranked** | Place Stage scene before the CSS (built) | No hold, ever | Soft pause grab (built) |
| **Direct** | Same (built) | No hold | Synced pause grab (built) |
| **LAN** | **Add** `enterAfterPlacing` | No hold | Pause |

**Netplay invariant check.** The scene runs before any connection, as a local GS_VS scene. The spot, the anchor and the grabs are XR-thread presentation. `pc_xr_place_hold`'s single `pc_net_active()` check is untouched. Nothing reaches `gm_RunSimTick`, the pads or the wire.

**Later option: a scene-free ghost** (the calibration plan's "ghost with the XR thread's quad layers"). It would let the spot be placed *over any menu*, including the online CSS while searching, without loading the dummy scene. It costs a ghost asset and a second placement presentation. Not needed while the dummy scene works and runs only once per room.

### C.7 Wireframes (Melee style)

The art comes from `tools/xr_cards/make_cards.py`: the red and black slanted title plate with hazard stripes; gold-capped black cards; round A/B/X/Y chips and grey GRIP/START/L/R pills. Every label leans slightly and nothing is italic, as in the launcher.

**Place Stage scene.** The title changes from "Place the Stage" to "Your Stage Spot"; the four cards and clips are unchanged.

```
 ╱▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔╲ ///////
│  Your Stage Spot            │
 ╲▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁╱
 [ MOVE ][ TURN ][ SCALE ][ ALL SET? ]          ← existing cards and clips
   Every stage starts here.                      ← one caption line
        (tinted Battlefield, Fox and Falcon idling)
 ┌──────────────────────────────────────────────────────┐
 │ L ◀ DESK · couch · room ▶ R   (A) Done  (B) Reset  (Y) Tips │
 └──────────────────────────────────────────────────────┘
```

Variants:
- **Room not recognised:** the caption is "Can't find your stage spot here." If the old anchor localizes meanwhile, the dummy glides to it and the caption reads "Found your spot!", with A to keep it.
- **On A:** the menu confirm sound and a brief "SAVED" chip, then on to the mode (or back to Options or Slippi with the row hovered, as `onExitPlace` does now).

**Options:**

```
  RUMBLE
  SOUND
  SCREEN DISPLAY
▶ STAGE SPOT               ← XR presenting only (like Slippi's Place Stage)
  LANGUAGE
  ERASE DATA
 ─────────────────────────────────────────────
  Set where every stage appears in your room.
```

**Pause legend** (XR, offline and soft pause). It's presentation only and drawn like the placing legend:

```
 ┌──────────────────────────────────┐
 │ GRIP Move this stage  (X) Use spot │   ← X: back to the spot (reset this stage)
 └──────────────────────────────────┘
```

X looks free on Melee's pause screen, but that's unverified; check it in Training, which has its own menu. Z retries in some modes and B means things there too. This is decision D6.

**Launcher, Quest page:**

```
  Stage spot          Saved for 2 rooms            [ Forget ]
  Before each fight   ◀ Start right away ▶  (Place the stage)
```

Row descriptions:
- **Stage spot:** "Where every stage appears. Set it in the game under Options › Stage Spot."
- **Before each fight:** "Start right away uses your stage spot. Place the stage holds every fight so you can move it first."

### C.8 Rejected trade-offs, in one place

| Option | Why not |
|---|---|
| Keep the hold on every fight with a base | The biggest recurring interruption, and Axis 2 shows it's rarely needed |
| "Hold only on a stage's first fight per session" | The user removed this sometimes-on behaviour in d064a16 |
| First hold writes the base | Contradicts the 2026-10-14 rule; arbitrary reference stage |
| Boot-time onboarding | Delays the menus; the XR session may not be presenting yet |
| Placing from the launcher | It's a 2D panel in another process, with no XR session |
| SSS as the primary entry | 1P modes have no SSS |
| Netplay countdown grab | About a second, the grips are Z, and a held grip at GO acts as Z |
| Placing during the Unranked search | Needs a scene-free ghost (C.6); later |
| Persist the LOCAL pose across launches, or a "same spot?" prompt | Silent errors after a recenter in Home; the user rejected the interim |
| Environment questionnaire | Placement plus a preset captures the same; one fewer interruption |
| Table or wall detection now | `USE_SCENE` runtime dialog plus Space Setup; it's tabletop's value, not floating's |
| Persist per-stage grabs now | The 2026-10-04 decision; low need once tall stages are re-centered |
| A separate surface height for floating | Meaningless without a real surface; belongs to tabletop |
| Global resize carried by every grab (8231c90's first behaviour) | Replaced by 17bafb3. Per-stage grabs would fight the base. |

### C.9 Forward compatibility

- **Key every stored spot by mode** (`mr-floating` now). Adding `vr` or `mr-tabletop` adds a key and never reinterprets an old one; a first switch into a new mode prompts once for that mode.
- **Tabletop** derives its base from the Scene table (the calibration plan's floor and platform anchoring and the `CenterRule` table; groups 2 and 3 above), with no free move or scale. It needs `USE_SCENE`.
- **Colocated two-headset** play reuses the anchor and adds sharing permissions.

### C.10 Suggested order of work

1. Data: tall stages re-centered on their action; survey Target Test and trophy stages. No UI.
2. Delta storage for grabs, and the recenter-event handler.
3. "Before each fight" (default Start right away), the Options row, the auto-run on 1P/VS/LAN entry.
4. Presets and caption in the Place Stage scene; pause legend; launcher rows.
5. Anchors (`horizonos.permission.USE_ANCHOR_API`), the file, Forget.

### C.12 What was built from Part D (2026-10-17)

The user's decisions: D1, a launcher setting, "Before each fight" (Place
the stage / Start right away), the hold staying the default for now; D2,
yes; D4, clear; D5, leave the stage in place (anchors); D7, yes.

- **Before each fight** (`xr_place_hold` in launcher.cfg,
  `pc_xr_place_set_hold`): "Start right away" skips the hold once there's a
  spot (placed this launch, or found in the room); with none, the first
  fight still holds.
- **Sudden Death** never holds: it's the fight just played, on its stage
  (`pc_xr_place_set_scene`, GS_SUDDEN_DEATH).
- **Auto-run** (`pc_xr_place_needed`): every fight mode the main menu starts
  (VS and its special modes, the 1P modes, Training, Home-Run, Event,
  Tournament) and LAN go through the Place Stage scene first when there's no
  spot, then on to the mode (gmmenumode.c, `gmOnline_SetModeAfterPlace`).
- **Anchors** (`horizonos.permission.USE_ANCHOR_API`; xr.cpp "the stage
  spot's anchor"): an adopted spot is anchored and saved (up to eight rooms,
  `stage-spot.txt`); at launch the saved ones are looked for and the first
  found becomes the spot. Placed stages follow the anchor as it moves in
  tracking space, and without one a recenter's own report keeps them in
  place (D5). Re-placing where the old spot is found replaces it (D4 clears
  the grabs, as before).
- **Launcher**: "Stage spot: Saved / Not set"; selecting it forgets the spot
  (the game erases the anchors at its next start).
- Not yet: tall stages re-centred (to tune in the headset), Target Test and
  trophy stages surveyed, the pause legend (D6), the scene's caption.

### C.11 Room survey (built 2026-10-17, the user's idea)

Instead of asking with presets (C.3, D3), measure. Once a launch, as soon as
the head is tracked, the surface worker casts a fan of rays from the head (5
across, +-30 degrees; 4 down, 5-50 degrees under level) against the
headset's depth for up to 6 s, while the player looks about the menus:

- a **desk or table**: level hits 10-80 cm under the eyes, within 1.4 m, not
  the floor;
- the **floor**: `XR_EXT_local_floor`, or the lowest level surface 80 cm or
  more down; eyes 1.4 m or more over it is **standing**;
- **room ahead**: walls in the forward rays (their 20th percentile), or open
  when those rays find nothing within 5 m.

It picks **Desk** (a desk ahead, seated), **Couch** (seated, no desk) or
**Room** (standing), and seeds the defaults the player hasn't set:

| | Stage | Screen |
|---|---|---|
| Desk | 0.0035 m/unit, 0.3 m past the desk's near edge (0.55-0.95 m), 15 cm over it (at least 25 cm under the eyes) | as today: 0.7 m at 0.85 m |
| Couch | 0.007 at 1.4 m, 35 cm under the eyes | 1.3 m wide at 1.5 m |
| Room | 0.011 at 2 m, 60 cm under | 1.7 m at 2 m |

Each is pulled in and shrunk with it (the same size to the eye) to stay clear
of a wall ahead. The screen glides there over 0.8 s. Never over an adopted
spot, a stage already placed or up, a screen the player moved, or the
`AURORA_XR_ARENA_*` / `AURORA_XR_SCREEN_*` knobs; `AURORA_XR_ROOM_SURVEY=0`
turns it off. It needs the spatial data permission (as the shadow does);
without it only the floor is known, so standing is Room and seated Couch is
never guessed (Unknown keeps today's desk defaults). Logged as "Room survey:
...". Not yet tried on the headset.

---

## Part D: decisions for the user

1. **Hold default.** Once a stage spot exists, should fights start right away, with the hold available as a setting? This changes the 2026-10-09 every-fight rule; the reasons are in C.4.
2. **Auto-run scope.** Should the Place Stage scene run before the first fight mode of a launch offline as well (1P, VS), with LAN joining Unranked and Direct?
3. **Presets.** Should the scene offer Desk, Couch and Room starting points, with standing head height (`XR_EXT_local_floor`) pre-selecting Room? This uses the floor space only to pick a preset, never to seed a surface.
4. **Re-placing the spot.** Should it keep per-stage grabs (they follow, as deltas) or clear them as today?
5. **Recenter.** Without an anchor, should a system recenter carry the stage (it lands in front of you; today's accidental behaviour) or keep it fixed in the room (`poseInPreviousSpace`)? With an anchor, it stays in the room either way.
6. **Pause legend.** Add "GRIP Move this stage · X Use spot" during the pause?
7. **Anchors.** Add `horizonos.permission.USE_ANCHOR_API` so the spot is saved per room? This replaces "a placement is required per launch" once the spot is found reliably.
8. **Naming.** "Stage Spot" in Options and the launcher, with "Place Stage" kept as the Slippi row's verb?

---

## Sources

**Code:**
- `src/pc/xr_place.c`, `src/pc/xr_place.h`
- `src/pc/xr_scene.c`
- `extern/aurora/lib/xr/xr.cpp`, `extern/aurora/include/aurora/xr.h`
- `src/melee/gm/gmscene.c`, `src/melee/gm/gmonlinemode.c`, `src/melee/gm/gmvsmode.c`
- `src/melee/mn/mnonline.c`, `src/melee/mn/mnmain.c`, `src/melee/mn/forward.h`
- `src/melee/gr/forward.h`
- `src/pc/vi.c`
- `src/pc/launcher.cpp`, `resources/launcher.rml`
- `platforms/android/app/src/xr/AndroidManifest.xml`
- `build/quest-perf/ext-warmup.log`

**Commits:** 6b6f5a1, 8231c90, 17bafb3, 7042d83, 7b717fa, c3e46f7, d064a16, 1d3d849, f9467be, ca31c5a, cb83238.

**Docs:** `docs/quest-xr.md` (Placing the arena, Stage backgrounds), `docs/slippi-migration-plan.md` §"Not done".

**External:**
- [OpenXR 1.1 specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html):
  - §7.1.2: on a LOCAL recenter "the runtime must queue the XrEventDataReferenceSpaceChangePending event"; inferred poses on tracking loss;
  - XR_EXT_spatial_anchor: "anchors are unaffected by system-level recentering".
- [XrEventDataReferenceSpaceChangePending](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrEventDataReferenceSpaceChangePending.html)
- [Meta: Persist content across sessions (native)](https://developers.meta.com/horizon/documentation/native/android/openxr-lsa-persist-content/)
- [Meta: Spatial anchor lifecycle](https://developers.meta.com/horizon/documentation/native/android/openxr-spatial-anchors-features-lifecycle/)
- [Meta: Spatial anchor best practices](https://developers.meta.com/horizon/documentation/unity/unity-spatial-anchors-best-practices/) (3 m; choose by distance; "prompt the user to reposition the content" when not found)
- [Meta: Spatial data permission](https://developers.meta.com/horizon/documentation/native/native-spatial-data-perm/) (USE_SCENE is a runtime request, with a fallback if declined)
- [Meta: MR design guidelines](https://developers.meta.com/horizon/design/mr-design-guideline/) (about 1 m, slightly below eye line; about half of users seated; provide a way to recenter or summon content)
- **Prior art.** I found little documentation of how other games re-prompt. The patterns below are from secondary coverage:
  - Demeo snaps its board to a table mapped in system room setup ([UploadVR](https://uploadvr.com/demeo-mixed-reality/), [Android Central](https://androidcentral.com/gaming/virtual-reality/i-played-virtual-dandd-on-a-real-table-with-the-quest-pro-and-it-was-amazing));
  - MR Chess is hand-placed, with resize and height adjustment ([MIXED](https://mixed-news.com/en/meta-quest-3-mr-chess-hands-on/));
  - Angry Birds AR has an always-visible re-place icon ([TouchArcade guide](https://toucharcade.com/2019/05/02/angry-birds-ar-isle-of-pigs-tips-guide/)).
