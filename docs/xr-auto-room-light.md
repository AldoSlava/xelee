# Automatic room-light matching on Quest: research report

Branch `quest-xr` at `6417425`, written 2026-10-16. This is research only: no code was changed, nothing was built, and the headset was not touched. It follows `docs/xr-stage-grounding-report.md` (T4, T5, T6, §7). The goal is to replace T4's manual **Bright / Normal / Dim** launcher preset with something automatic.

Verdicts used below:
- **Confirmed**: seen in our own headset logs or stated in Meta/Khronos docs.
- **Plausible**: strong indirect evidence, but it needs the on-headset probe in §5.
- **Dead**: ruled out, or not worth doing.

## TL;DR

- **Surprise 1: Horizon OS already computes and logs what we want.** Once a second, the compositor logs the passthrough camera's auto-exposure (AE) state:
  ```
  I/PassthroughInCompositorRenderer: PTINC FOVEAMETA gain=63.94 exposureMs=11.000 tempC=0.0 lux=529.3
  ```
  - There are 54,270 of these lines across 1,146 of our `build/quest-perf/*.log` files.
  - The `lux` field is **not** lux: it *rises* as the room gets darker.
  - It behaves like a Qualcomm-style **lux index**: about **26 units per EV** (fit on 48k unsaturated samples, residual SD 9 ≈ 0.35 EV).
  - It keeps tracking after AE saturates at 11 ms × 64× gain.
  - Apps can't read other processes' logs, so this is a **calibration and ground-truth tool, not a shipping input**.
- **Surprise 2: Camera2 frames use the same exposure as the displayed passthrough.** The camera service's frame dispatcher feeds the colour camera frameset slot that passthrough uses to `cam2bridge` as well:
  ```
  FrameSetDispatcher: [{COL:FULPS}] purpose: passthrough, slots: SLOT_0
  FrameSetDispatcher: [{COL:FULPS}] purpose: cam2bridge, slots: SLOT_0
  ```
  - Each frame in that set carries its own `exposure:` and `gain:`.
  - So the Passthrough Camera API most likely sees *the same exposure* as the passthrough the user sees. That answers T6's "do they share AE?" doubt in our favour, with high (not certain) confidence.
  - It also means we almost certainly **can't** lock or set exposure ourselves.
- **Surprise 3: Quest 3 has a colour ambient-light and flicker sensor.** Our logs name it `VD628x` / `STALS`; ST's VD6283 has R, G, B, IR, clear and visible channels plus a flicker engine, which is enough for lux *and* colour temperature.
  - The compositor owns it through a private `libflickersensor` / `AmbientLightSensorClient`.
  - It is used to decide whether passthrough may sync to the display ("Ambient display-sync permission changed").
  - Android's own `DisplayPowerController` logs `lux=0.0`, which suggests the framework gets no ALS feed. Whether apps can see it through `SensorManager` is **unknown**; one `dumpsys` settles it.
- **Best automatic route (recommended): short Passthrough Camera API bursts.**
  - One camera at 320×240, a few frames at each placement hold plus a slow refresh.
  - Read the mean luma and chroma of the frame. Also read `SENSOR_EXPOSURE_TIME` × `SENSOR_SENSITIVITY` and `COLOR_CORRECTION_GAINS` if the HAL fills them.
  - Map *relative passthrough brightness*, not absolute lux, to `XrCompositionLayerColorScaleBiasKHR` on our projection layer.
  - Cost: one `HEADSET_CAMERA` prompt (pre-grantable with `adb pm grant` for test runs), and near-zero GPU outside bursts.
- **Dead ends:**
  - No light-estimation extension is offered or gated on our runtime. `XR_ANDROID_light_estimation` and `XR_BD_spatial_light_estimation` exist in the headers but aren't on Quest.
  - `XR_META_performance_metrics` has no camera counters.
  - Reading back the displayed passthrough (MediaProjection) works technically, but Meta's policy and the per-session consent rule it out.
  - ARCore doesn't run on Horizon OS.
- **The mapping insight:** AE holds the *displayed* passthrough near a fixed brightness until it saturates.
  - Our logs show it saturating at 11 ms × 64× in **~11 % of all samples**, all of them from evening or night sessions (19:00–22:00 and 02:00–04:00 local).
  - So in lit rooms the right scale is ≈ constant, and the curve only bites once AE saturates.
  - That is why the pixel mean (or exposure × gain plus the pixel mean) matters more than lux.

---

## 1. Evidence from our own headset logs

All from `build/quest-perf/*.log` (full `adb logcat` captures, Horizon runtime "Oculus 209.113.0", Android SDK 34).

### 1.1 The compositor's per-second AE line (`PTINC FOVEAMETA`)

`PassthroughInCompositorRenderer` (the compositor, pid 2930/2596) logs `gain`, `exposureMs`, `tempC` and `lux` once a second whenever passthrough runs. What the data shows:

- **Auto-exposure sequence** (54,270 samples):
  - exposure 1.54 → 6 ms at gain 1.0;
  - then gain 1 → ~15.5 at 6 ms;
  - then exposure 6 → 11 ms at gain ~16;
  - then gain → **64 at the 11 ms cap**.
  - Exposure × gain spans 1.54 … 704, about 8.8 EV, so the user plays across roughly 9 EV of room light.
- **"lux" is a darkness index.**
  - Below saturation it fits `lux ≈ 120 + 26.1·log2(exposure_ms × gain)`, residual SD 9.0. That is ≈ 26 units/EV, close to Qualcomm Spectra's lux index (log base ≈ 1.027).
  - Brightest observed: 143 at 1.54 ms × 1.0. Darkest: 606 at saturation.
- **It keeps moving after saturation.**
  - At 11 ms × 64 (5,886 samples), it ranges 380–606 (median 544), against 366 predicted from exposure × gain alone.
  - So it includes an image-brightness term: it measures how far the image falls short of the AE target once AE runs out of range.
  - The scale above saturation is unverified. 178 units would be ~7 EV at the unsaturated rate, which seems too much, so treat it as monotonic only.
- **It reacts within a second.** In `zb4-cs-0-mr.log` at 02:08:18 → 02:08:19, gain went 18.19 → 63.94 and lux 325 → 545 (a light switched off, or the head turned to a dark area).
- **`tempC` is always 0.0** in every sample, so there is no colour temperature here. It may well be a sensor temperature in °C that isn't populated.
- **Telemetry agrees:** `[Insights] PassthroughTelemetry: - avg exp/gain 11.0/63.9`. The grey stereo cameras are logged separately (`avg exp/gain stereo: 13.0/15.0`).

**Verdict:** confirmed that it exists, and confirmed as a dev-time instrument. **Dead as a shipping input**, because apps only see their own log lines.

`READ_LOGS` is a development permission, so `adb shell pm grant` can give it to us. But Android 13+ shows a per-access consent dialog for apps that read device logs, and we're on SDK 34. Whether Horizon OS shows that dialog is unverified, and it doesn't matter for shipping.

### 1.2 Camera2 frames come from the passthrough frameset

```
FrameSetDispatcher: [{COL:FULPS}] purpose: passthrough, slots: SLOT_0
FrameSetDispatcher: [{COL:FULPS}] purpose: ocal,        slots: SLOT_0
FrameSetDispatcher: [{COL:FULPS}] purpose: cam2bridge,  slots: SLOT_0
FrameSetCollator:  { cameraId: 4, ... exposure: 0.011, gain: 63.9375, ... }
FrameSetCollator:  { cameraId: 5, ... exposure: 0.011, gain: 65.4375, ... }
XrCameraHal: Xr3AManager.cpp:applyFrameBasedSettings: isp set Camera: 4, framerate to 60
CorePi: AWB cam0: AWBInterface.cpp: Create Meta AWB interface succeed!
```

- `cam2bridge` (by name, the Android Camera2 bridge behind the Passthrough Camera API) appears in 528 dispatcher configs. It always shares `SLOT_0` with `passthrough` and `ocal` (online calibration).
- **Inference: plausible, strong.**
  - Camera-API frames are the *same sensor exposures* as the displayed passthrough, so AE and AWB are shared.
  - The camera HAL has exposure and gain per frame, so it *can* fill `SENSOR_EXPOSURE_TIME` / `SENSOR_SENSITIVITY` in `CaptureResult`. Whether it does is unverified.
  - AE lock, `AE_MODE_OFF` and manual exposure are *very likely ignored*. If they weren't, an app could darken the user's passthrough.
  - Two caveats. The image pipeline after capture (tone curve, crop) may differ from what the compositor shows; Meta says the camera texture is "a rectangular area (1280x960) smaller than what a user sees". And the passthrough frames are gamma-encoded (`sensor_config_gamma=0.4545`).
  - Both left and right colour cameras (4 and 5) run their own gain (63.94 vs 65.44), so read one camera only.
- **AWB:** a "Meta AWB" engine runs on the colour camera. No AWB gains or CCT show up in the logs.

### 1.3 The ambient-light and flicker sensor

```
CompositorVR: Start to create ambient light sensor client
AmbientLightSensorClient: Flicker sensor service connected
AmbientLightSensorClient: ALS stream started
VD628x: Flicker results: freq 999.93, max 896.9, dcr 52.2, pct 0.000787, ... conf 0.0178, snr -17.3
libflickersensor: AmbientLightSensor SCM: client=2930 Failed to write sensor data, dropping data!
TrackingFidelityService: Ambient display-sync permission changed. Setting maximum low_latency_pt fidelity to 0   (534 times)
...                                                                                         to 5   (132 times)
I/DisplayPowerController2[0]: BrightnessEvent: disp=0, ... lux=0.0, preLux=0.0 ...
```

- `VD628x` / `STALS` match **ST's VD6283** family:
  - six ALS channels (red, green, blue, IR, clear, visible) plus a flicker channel;
  - ST lists its uses as "lux and color temperature measurement" and "camera white balance assistance";
  - sources: https://www.st.com/en/imaging-and-photonics-solutions/vd6283tab.html and https://www.st.com/resource/en/datasheet/vd6283tab.pdf.
- Nobody publicly confirms the part from a teardown; our log tag is the only evidence.
- **What the system uses it for:** the flicker result gates "camera-display sync". Meta: "In flicker-free lighting conditions, passthrough will synchronize the cameras with the display … only available on Quest3/Quest3S" (https://developers.meta.com/horizon/documentation/unity/unity-passthrough-bp/).
- **App access: unknown, leaning no.**
  - Clients are compositor pids, reached through a vendor "SCM" path, not Android's `SensorService`. (The `SensorService( 1071)` in our logs is Meta's camera service, not Android's.)
  - Android's `DisplayPowerController` reports `lux=0.0`.
  - The probe (§5, step 0) answers it with `dumpsys sensorservice`.

> **Aside (for `docs/xr-frame-rate-plan.md`):** the 534 vs 132 "display-sync permission … 0 / 5" lines suggest the user's room lighting usually *blocks* camera-display sync, which bears on the 72 Hz step.

### 1.4 OpenXR: nothing that reads light

From the 2026-10-11 enumeration in `ext-warmup.log`:
- **Offered:** no light, exposure or camera extension.
- **Gated:** the "skipping extension=… due to:" lines cover body, face and eye tracking, scene and anchors (`USE_ANCHOR_API` / `USE_SCENE`), and experimental `XR_METAX*`. None of them is light-related. `XR_META_local_dimming` is skipped for "device requirements (16)" (Quest Pro's local-dimming backlight), which is unrelated.
- `XR_META_performance_metrics` counters: the full list we enumerate is app/compositor frame times, latency, SpaceWarp mode and CPU/GPU utilisation. No camera counters.
- **Headers:** our 1.1.63 headers define `XR_ANDROID_light_estimation` (Android XR: ambient, directional and spherical-harmonic light) and `XR_BD_spatial_light_estimation` (PICO). Neither is offered or gated on Quest. The Android one also needs `SCENE_UNDERSTANDING_COARSE` on Android XR (https://developer.android.com/develop/xr/openxr/extensions/XR_ANDROID_light_estimation).

---

## 2. Avenue-by-avenue verdicts

| # | Avenue | Verdict | Evidence | Notes |
|---|---|---|---|---|
| A1 | **Camera2 `CaptureResult` exposure/gain** (`SENSOR_EXPOSURE_TIME`, `SENSOR_SENSITIVITY`) | **Plausible** | §1.2 (shared slot, exposure and gain per frame). Meta's Ocean framework reads these keys next to the Quest `camera_source` tag (https://github.com/facebookresearch/ocean, `impl/ocean/media/android/ALiveVideo.cpp`), but its code is generic Android, so that isn't proof. A third-party Quest probe plans to log them, with no published results (https://github.com/olibartfast/meta-quest3-lab, `specs/milestone-plans/milestone16-plan.md` item 12). Meta's docs only promise "fine-grained control over camera settings" (https://developers.meta.com/horizon/documentation/spatial-sdk/spatial-sdk-pca-overview/). | `CameraCharacteristics.getAvailableCaptureResultKeys()`, or `dumpsys media.camera` with no build, settles it. If present: relative EV = −log2(exposure × ISO), up to a constant. |
| A2 | **Camera2 AWB data** (`COLOR_CORRECTION_GAINS`, `SENSOR_NEUTRAL_COLOR_POINT`, `CONTROL_AWB_STATE`) | **Plausible / unknown** | An AWB engine exists (§1.2); there is no public evidence about the keys. | Gains → illuminant colour. Otherwise use mean chroma of the frame (gray-world, damped). |
| A3 | **AE lock / manual exposure** for absolute measurement | **Dead (probably)** | The frames are shared with the displayed passthrough (§1.2). The runtime can't let one app change everyone's passthrough exposure. | Not needed anyway: exposure × gain plus the pixel mean is absolute enough. The probe tests it for 10 s, watching the passthrough. |
| A4 | **Camera pixels** (Meta's sample: BT.709 Y average) | **Confirmed available; plausible as the right signal** | Meta's Brightness Estimation sample: BT.709, 640×480, 20 Hz, 10-frame mean, thresholds 10/50 (https://developers.meta.com/horizon/documentation/unity/unity-sample-camera-brightness). | Because AE is shared, the mean Y measures *displayed* passthrough brightness, which is what we must match (§4). It reads near-constant until AE saturates, and that is correct, not a flaw. The camera never sees our virtual stage, so there's no feedback loop. |
| A5 | **Cheapest Passthrough Camera API use** | **Confirmed knobs** | Unity docs: resolutions from **320×240** up; `MaxFramerate` cap (default 60) (https://developers.meta.com/horizon/documentation/unity/unity-pca-documentation). Native docs: ~1–2 % GPU *per streamed camera*, ~45 MB, 20–40 ms latency, YUV420 (https://developers.meta.com/horizon/documentation/native/android/pca-native-overview). | One camera, 320×240, `YUV_420_888`, `ImageReader` maxImages 2. Bursts: open the session, take 3–5 frames, close. The open-to-first-result time is unknown, so the probe measures it. Everything runs on a `HandlerThread`. |
| A6 | **Android `Sensor.TYPE_LIGHT`** (or a vendor colour sensor) | **Plausible hardware, unknown access** | §1.3: there is a VD628x ALS + flicker sensor, owned by the compositor through a private client. Android's DisplayPowerController sees `lux=0.0`. No public Quest `dumpsys sensorservice` exists. | If it's exposed: no permission and no GPU cost, and with RGB channels possibly CCT. That would become the primary input for colour. Settle with §5 step 0. |
| A7 | **Proximity / other Android sensors** | **Dead** for light | No relevant evidence. | The probe dumps the full list anyway. |
| A8 | **Display brightness / auto-brightness** (`Settings.System.SCREEN_BRIGHTNESS`) | **Dead** | Quest brightness is a manual slider. It scales passthrough and our layer *equally*, so it cancels out of the ratio we care about (§4). | — |
| A9 | **Horizon system props / settings** exposing exposure or lux | **Unknown, leaning dead** | No prop or setting lines in the logs; app reads of many vendor props are SELinux-denied (`Access denied finding property "ro.vendor.product.model_extended"`). | The probe greps `getprop` and `settings list` (§5 step 0). |
| A10 | **Reading `PTINC FOVEAMETA` via logcat** | **Confirmed, dev-only** | §1.1. | Ground truth for the probe and for curve fitting. Never ship it. |
| A11 | **OpenXR light estimation** (any vendor) | **Dead** on Quest today | §1.4. Meta's design page *recommends* ambient light estimation but provides no API (https://developers.meta.com/horizon/essentials/horizon-os-passthrough/). | Re-check the "offered" list after each OS update; it's a free log line. |
| A12 | **ARCore / Android XR light estimation** | **Dead** | ARCore needs Google Play Services for AR, which Horizon OS lacks (not formally verified). Android XR's extension isn't offered (§1.4). | — |
| A13 | **Reading back the displayed composite** (MediaProjection) | **Dead for shipping** | It works on Quest: Meta's compositor fills the surface ("populated directly by our own compositor"). A workaround gets 1024×1024 of the central 82° *including passthrough* (https://developers.meta.com/horizon/documentation/native/native-media-projection/, https://www.uploadvr.com/quest-3-raw-camera-access-better-workaround). But: consent is needed each session; policy allows it "only" for "casting, live streaming, or screen sharing"; it collides with the user's own recording or casting; and our own stage is in the image. | Usable as a **one-off calibration instrument**, to relate camera Y to displayed Y. |
| A14 | **Hand-tracking low-light mode** (`muxMode[worldTracking: lowLightHands]` in logs) | **Dead** | System-internal; there is no OpenXR or Android surface for it. | It does show the OS itself classifies "low light". |
| A15 | **Time of day as a prior** | **Speculative, weak** | Every saturated-AE sample in our logs falls between 19:00 and 04:00; daytime sessions (09:00–18:00) never saturate. | Free and needs no permission, but wrong for curtained or office rooms. Only as a fallback tie-breaker, and only if A/B supports it. |
| A16 | **What other Quest MR apps do** | **Confirmed: nothing better is public** | Meta's own sample is Y-average plus hysteresis. I found no talk or post on exposure-metadata-based estimation for Quest. Resolution Games used ARKit's estimate with *separate curves* per light (grounding report §1). | We'd be ahead of public practice by using exposure × gain. |

---

## 3. Recommended approach and fallback chain

### 3.1 Measurement: "room sampler" (Java, `:xr` process, off the frame loop)

1. **When:**
   - At each **placement hold** (it already opens every fight, sim-free; `stage-placement-hold.md`).
   - Then one burst every ~15 s during fights.
   - Only in MR: passthrough must be on for the camera API. In VR mode, keep the last value.
2. **Burst:**
   - Open the camera with `com.meta.extra_metadata.camera_source == 0` and `position == 0` (left).
   - One `YUV_420_888` `ImageReader` at the smallest size (expect 320×240), `TEMPLATE_PREVIEW`, the lowest `CONTROL_AE_TARGET_FPS_RANGE` offered.
   - Take 4 frames, drop the first, then close the session.
   - If the probe shows open/close is slow or costly, keep one session open with the repeating request and `acquireLatestImage()` at 1 Hz instead, and measure the steady-state GPU cost.
3. **Per frame, on the camera thread:**
   - Y mean of a centre-low window, where the stage usually sits (rows 40–90 %, columns 20–80 %), sampled every 8th pixel. That is ~1,000 reads.
   - Mean Cb/Cr of the same window.
   - If populated: `SENSOR_EXPOSURE_TIME`, `SENSOR_SENSITIVITY`, `CONTROL_AE_STATE`, `COLOR_CORRECTION_GAINS`.
4. **Hand-off:** linearise Y (frames are gamma ≈ 1/2.2), then post `{P_lin, chroma, EV_rel, valid}` through JNI into an atomic struct that `xr.cpp` reads when it builds the layer list. Nothing waits on it, and nothing touches sim, tick or netplay state.
5. **Later option:** use the camera pose (`GetCameraPose`-equivalent: lens pose plus intrinsics) to project the *stage footprint* into the camera image, then measure the room *behind the stage*. Simultaneous contrast is local, so this is the perceptually right window. It's plausible but not needed for v1.

### 3.2 Applying it

- Chain `XrCompositionLayerColorScaleBiasKHR` on the 3D projection layer (`proj` at `xr.cpp:3972`; the pointer quads already use the struct at `xr.cpp:3418`). Bias 0, alpha scale 1, as in T4.
- Leave the HUD layer unscaled, or give it its own gentler curve: legibility comes first.
- Smoothing:
  - Exponential with τ ≈ 3 s.
  - Limit the rate to ≤ 10 %/s during fights.
  - **Snap** at hold start.
  - Hysteresis of ±0.03 on the scale so 1 Hz noise never shows.
- Optionally drive T5 (the passthrough BCS/LUT grade) from the same signal, on its **own** curve (§4.3).

### 3.3 Fallback chain

1. **Camera metadata + pixels** (Quest 3/3S, `HEADSET_CAMERA` granted, OS v74+; Meta's native page says v76+ for Camera2, and we're far past both).
2. **Pixels only**, if the probe shows the exposure and sensitivity keys are absent. This loses absolute EV, which we mostly don't need (§4).
3. **Android light sensor**, if the probe finds one exposed. It needs no permission. It gives lux, and maybe RGB for CCT. Turn lux into a predicted displayed brightness with the saturation model in §4.1 (the threshold is calibrated against FOVEAMETA).
   - If it's exposed *and* has colour channels, it should **replace** step 1's chroma for white balance: gray-world on a camera frame is fooled by coloured walls.
4. **No signal** (Quest 2, Quest Pro, permission denied, VR mode): per-stage constants × a fixed "Normal". Keep one hidden override setting for power users, not a preset row.
5. **Dev only:** FOVEAMETA via `adb logcat` for tuning and regression checks.

---

## 4. The mapping problem: what to aim for

### 4.1 Model

The viewer's eyes adapt to the **display**, not the room. The display mostly shows passthrough, and the passthrough is auto-exposed. So:
- Displayed passthrough brightness is `P_disp ≈ D · T(scene)`:
  - `D` is the display brightness slider. It cancels, because our layer goes through the same display.
  - `T` is AE's target. It is roughly constant while AE has range.
  - Once exposure × gain hits 11 ms × 64, `T` falls in proportion to the room's light.
  - Qualcomm-style AE also tends to lower its target a little in dim scenes; the probe will show whether Quest does.
- Our stage's displayed brightness is `D · s · M_stage`:
  - `s` is the colour scale we control.
  - `M_stage` is the stage's intrinsic mean luminance: Brinstar Depths ≠ Fountain.

**Target:** keep `R = s·M_stage / P_disp` near a tuned constant `k`, but only *partially*:
- A stage that dims all the way with a dark room becomes murky and unplayable.
- A dark stage is meant to be dark.

```
P      = smoothed linear camera Y of the window      (≈ P_disp up to a constant)
s_lum  = clamp( (P / P0)^γ  ·  (M_ref / M_stage)^β ,  s_min, s_max )
```
- Starting values: γ ≈ 0.6, β ≈ 0.3, s_min ≈ 0.5, s_max ≈ 1.05.
- `P0` is the camera Y at which the stage looks right at scale 1, presumably any lit room.
- `M_stage` per grkind is measured offline, once, from desktop XR dump captures (`desktop-xr-survey.md`). That avoids any live readback of our eye buffer.

**Prediction:** in lit rooms `P ≈ P0`, so `s ≈ 1` and nothing changes. The curve acts in the dim evening regime, which is about 11 % of our logged samples and evidently the user's real evening setup.

**Colour:**
```
c      = mean RGB of the window (from Y/Cb/Cr), or the ALS RGB if exposed
t      = c / luma(c)                      (unit-luminance tint)
s_rgb  = s_lum · mix(1, t, w)  per channel, w ≈ 0.3, clamp each channel to ±10 %
```
- The passthrough AWB already removes most of the cast. We only want to match the **residual** cast the user actually sees, so `w` stays small.
- If AWB turns out to be fully neutralising (Cb/Cr ≈ 0 in a lamp-lit room), set `w = 0`. The probe settles this.

### 4.2 Tuning with the user's blind A/B method (`spacewarp-late-frames.md`)

1. **Rooms:** daylight, evening warm lamp, TV/monitor-only. Run the dev FOVEAMETA logger in each, so every trial carries the system's own lux index as ground truth.
2. **Luminance trials:**
   - Per room, test fixed scales {1.0, 0.85, 0.7, 0.55} on Final Destination and one dark stage.
   - A script picks a random order (2 runs each), labels them 1–4, and saves the key unread.
   - The question: "Which one looks like it's *in* the room?" Record the winner and `P`.
3. **Tint trials:** test `w` ∈ {0, 0.3, 0.6} in the lamp room.
4. **Fit:** `log s* = γ·log(P/P0)` by least squares, giving `γ` and `P0`. If two rooms with the same `P` want different `s*`, add the EV term (exposure × gain) as a second input.
5. **Validation:** in a new room, compare auto vs. the best fixed "Normal", blind. Ship if auto wins ≥ 3 of 4.

### 4.3 Separate curves per output (Resolution Games' lesson)

- `s_lum`: projection-layer luminance (above).
- `s_rgb`: projection-layer tint, with its own `w`.
- T5 passthrough grade:
  - Room dim and desaturation, keyed on *bright* rooms ("house lights down"). It's the inverse regime, so it needs its own curve.
  - Never darken the room hard: the user holds a pad in a real room.
- HUD: none, or a floor at 0.85.

---

## 5. On-headset probe plan

Run in this order. Steps 0 and 1 need **no build**.

Before any unattended run, set `adb shell setprop debug.oculus.guardian_pause 1`, and set it back to 0 at the end of the batch (`guardian-pause-gamepad.md`, `no-sleep-mid-session.md`). With two adb devices, prefix every command with `ANDROID_SERIAL=…`.

### Step 0: static facts (adb only, 5 minutes)

```sh
OUT=build/quest-perf/light-probe; mkdir -p $OUT

# Camera2 static metadata for every camera, including availableResultKeys and vendor tags
adb shell dumpsys media.camera > $OUT/media_camera.txt
grep -n -E "Camera ID|static information|android.request.availableResultKeys|android.request.availableRequestKeys|android.info.supportedHardwareLevel|android.request.availableCapabilities|android.sensor.info.exposureTimeRange|android.sensor.info.sensitivityRange|android.control.aeAvailableModes|android.control.aeLockAvailable|android.control.awbLockAvailable|android.control.awbAvailableModes|android.control.aeAvailableTargetFpsRanges|android.scaler.availableStreamConfigurations|com.meta" $OUT/media_camera.txt | head -200

# Android sensors: is there a TYPE_LIGHT (5) or a vendor colour sensor?
adb shell dumpsys sensorservice > $OUT/sensorservice.txt
grep -n -i -E "light|lux|als|flicker|color|rgb|vd62|stals|android.sensor\." $OUT/sensorservice.txt | head -80

# Is any ALS wired to Android's display stack?
adb shell dumpsys display | grep -i -E "lightsensor|light sensor|ambient|lux|autobrightness|mUseSoftwareAutoBrightness" | head -40

# Binder services and props that might expose light or exposure
adb shell service list | grep -i -E "flicker|light|als|ambient|camera"
adb shell getprop | grep -i -E "lux|als|light|flicker|exposure|awb|passthrough"
for ns in system secure global; do adb shell settings list $ns | grep -i -E "bright|light|lux|passthrough|ambient|night"; done
adb shell 'for d in /sys/bus/iio/devices/*; do echo "$d $(cat $d/name 2>&1)"; done'

# Permission definitions
adb shell dumpsys package permission horizonos.permission.HEADSET_CAMERA | head -20
```

**What decides what:**
- If `availableResultKeys` contains `android.sensor.exposureTime` and `android.sensor.sensitivity`, A1 is **confirmed**.
- If it contains `android.colorCorrection.gains`, A2 is **confirmed**.
- If `aeLockAvailable` is false, or `aeAvailableModes` has no OFF, A3 is **dead, confirmed**.
- If `sensorservice` lists a light sensor, A6 goes to the fallback chain's step 3, and possibly to primary for colour.

### Step 1: ground truth from the compositor (adb only, during any MR session)

```sh
adb logcat -c
adb logcat -v time -s PassthroughInCompositorRenderer:I MRSS:I \
  | grep --line-buffered -E "FOVEAMETA|avg exp/gain" | tee $OUT/fovea.log
```

- Walk through: lights on → lamp only → off with the TV on → near a window.
- Note the times. This gives the lux-index range in the user's real rooms, and where AE saturates.

### Step 2: debug probe build (one gated Java class, then remove it)

Add a `LightProbe` class to `MeleeXrActivity`, gated on an intent extra so normal launches are untouched. It logs under tag `MeleeLight`.

1. **Characteristics and sensors:**
   - For each camera ID: `getKeys()`, `getAvailableCaptureResultKeys()`, the vendor tags, the hardware level, capabilities, AE/AWB modes and lock availability, exposure and sensitivity ranges, `YUV_420_888` output sizes, FPS ranges.
   - `SensorManager.getSensorList(Sensor.TYPE_ALL)`: name, vendor, type, `stringType`, range. If a `TYPE_LIGHT` exists, register it and log the values at 1 Hz.
2. **Stream, 30 s:** smallest YUV size, left passthrough camera, `TEMPLATE_PREVIEW`. In `onCaptureCompleted`, log the full `result.getKeys()` list once. Then log at 1 Hz:
   - exposure time, sensitivity, frame duration;
   - AE mode and state, AWB state;
   - `COLOR_CORRECTION_GAINS`, `SENSOR_NEUTRAL_COLOR_POINT`, `CONTROL_POST_RAW_SENSITIVITY_BOOST`;
   - the Y/Cb/Cr window means.
   Compare against `FOVEAMETA` at the same second. Matching exposure and gain **confirms** the shared AE (§1.2).
3. **Control test, 10 s each, with the user watching the passthrough:**
   - `CONTROL_AE_LOCK=true`;
   - `AE_MODE_OFF` with 2 ms at minimum ISO;
   - `CONTROL_AWB_LOCK=true`.
   Log the result values and FOVEAMETA. Expect no change. If the passthrough *does* change, stop: that's both a finding and a hazard.
4. **Burst timing:** close the session, wait 5 s, reopen, and log the ms from `openCamera` to the first `onCaptureCompleted`. Repeat 5 times. Log the process RSS before and after.
5. **READ_LOGS test (dev curiosity):** if granted, run `logcat -d -t 5 -s PassthroughInCompositorRenderer:I` from the app. Log the line count and whether a consent dialog appeared.

```sh
adb shell pm grant dev.melee.game horizonos.permission.HEADSET_CAMERA   # avoids the prompt in unattended runs
adb shell pm grant dev.melee.game android.permission.READ_LOGS          # step 2.5 only; revoke after
adb shell am start -n dev.melee.game/dev.melee.MeleeXrActivity --es disc <disc> --ez lightprobe true
adb logcat -v time -s MeleeLight:I PassthroughInCompositorRenderer:I | tee $OUT/probe.log
# afterwards
adb shell pm revoke dev.melee.game android.permission.READ_LOGS
```

### Step 3: cost (the existing benchmark)

Run `quest_perf.py` with the 4P Brinstar replay at CPU 3 / GPU 2 (`quest-benchmark.md`) in three configurations:
1. probe off;
2. a continuous 320×240 stream;
3. a 4-frame burst every 15 s.

Compare app GPU time, `/perfmetrics_meta/compositor/gpu_frametime`, stale or dropped frames, and the `AURORA_XR_LOG_PERIOD=1` cost table. **Pass:** the burst mode stays within run-to-run noise.

### Step 4: one-off displayed vs camera check (optional)

Take a headset screenshot or a MediaProjection frame and compare the displayed passthrough mean in a no-stage area against the camera Y mean, in two rooms. This fixes the constant between `P` and `P_disp`. The A/B fit absorbs it anyway, so skip it if time is short.

---

## 6. Cost and permission impact

| Route | Permission / prompt | Frame-loop risk | GPU / memory | Devices | Notes |
|---|---|---|---|---|---|
| Camera bursts (recommended) | `horizonos.permission.HEADSET_CAMERA` (runtime, one prompt, persists). Test rigs: `pm grant` once. | None: camera thread plus an atomic hand-off; the scale is applied by the compositor | Meta: ~1–2 % GPU *while streaming* one camera and ~45 MB. Bursts every 15 s → est. well under 0.1 % average. **Unmeasured until step 3.** | Quest 3/3S only | Camera data is "Device User Data" (Meta policy). Store listing must disclose camera use; frames never leave the process. |
| Continuous stream at 1 Hz read | Same | None | ~1–2 % GPU continuously (Meta), plus ~45 MB | Quest 3/3S | Only if bursts prove slow to open |
| Android ALS (if exposed) | None | None | ~0 | Unknown (Quest 3 has the part; 3S unknown) | Best case. Probe step 0. |
| Colour scale on projection layer | None | None | ≈0 (compositor multiply) | All | Already enabled extension |
| FOVEAMETA via logcat | `READ_LOGS` via adb only; possible consent dialog | None (dev only) | 0 | Quest 3/3S | Never ship |
| MediaProjection | Consent every session; policy disallows this use | Encoder/compositor load | Unknown | All | Calibration only |

**Prompt strategy:**
- Ask for `HEADSET_CAMERA` the first time MR starts with "Auto room light" on (the default), explaining why: "Matches the stage to your room's lighting. Images never leave the headset."
- On denial, fall back silently (§3.3 step 4). Don't ask again until the user flips the setting.
- `pm grant` keeps unattended `quest_perf` launches prompt-free; add it to the script's setup next to `guardian_pause`.

---

## 7. Still unknown (each one is settled by the probe step named)

1. Does Camera2's `availableResultKeys` include exposure, sensitivity and AWB gains? (Step 0)
2. Do `CaptureResult` exposure and gain equal FOVEAMETA's at the same instant, which would prove shared AE? (Step 2.2)
3. Is the VD628x visible to `SensorManager`, with RGB channels? (Steps 0 and 2.1; also check Quest 3S)
4. Are AE and AWB lock ignored? Does the passthrough change? (Step 2.3)
5. How long from opening the session to the first frame, and what does a burst cost compared with streaming? (Steps 2.4 and 3)
6. Does Quest's AE lower its target in dim scenes before it saturates? (Step 1 + step 2.2: does Y fall while exposure × gain is still below the cap?)
7. How much colour cast does AWB leave under a warm lamp? (Step 2.2 Cb/Cr, and the tint A/B)
8. What does the lux index mean above saturation? (Step 1 against step 2.2 Y; dev interest only)

## 8. Sources

- Our logs: `build/quest-perf/*.log` (1,146 files with FOVEAMETA), `build/quest-perf/ext-warmup.log` (2026-10-11 enumeration).
- Meta, Passthrough Camera API (native): https://developers.meta.com/horizon/documentation/native/android/pca-native-overview
- Meta, Passthrough Camera API (Spatial SDK overview, vendor tags): https://developers.meta.com/horizon/documentation/spatial-sdk/spatial-sdk-pca-overview/
- Meta, Unity PCA docs (resolutions from 320×240, `MaxFramerate`): https://developers.meta.com/horizon/documentation/unity/unity-pca-documentation
- Meta, Brightness Estimation sample: https://developers.meta.com/horizon/documentation/unity/unity-sample-camera-brightness
- Meta, Android passthrough camera samples: https://developers.meta.com/horizon/documentation/android-apps/passthrough-camera-samples/ and https://github.com/meta-quest/Meta-Passthrough-Camera-API-Samples
- Meta, passthrough best practices (flicker-free camera-display sync): https://developers.meta.com/horizon/documentation/unity/unity-passthrough-bp/
- Meta, Horizon OS passthrough essentials ("Consider ambient light estimation"): https://developers.meta.com/horizon/essentials/horizon-os-passthrough/
- Meta, MediaProjection on Quest: https://developers.meta.com/horizon/documentation/native/native-media-projection/
- UploadVR, MediaProjection capture includes passthrough: https://www.uploadvr.com/quest-3-raw-camera-access-better-workaround
- Meta forum, Camera2 black stills on Quest 3: https://communityforums.atmeta.com/discussions/Questions_Discussions/quest-3-accessing-camera-via-android-camera2-api---process-succeeds-but-image-is/1346423
- Meta Ocean framework, Android camera (reads exposure and sensitivity; Quest vendor tag): https://github.com/facebookresearch/ocean (`impl/ocean/media/android/ALiveVideo.cpp`)
- meta-quest3-lab stereo probe plan (per-frame exposure parity; no results yet): https://github.com/olibartfast/meta-quest3-lab
- QuestRealityCapture (dumps Camera2 characteristics on Quest 3): https://github.com/t-34400/QuestRealityCapture
- ST VD6283 (6-channel ALS + flicker): https://www.st.com/en/imaging-and-photonics-solutions/vd6283tab.html and https://www.st.com/resource/en/datasheet/vd6283tab.pdf
- Android XR light estimation: https://developer.android.com/develop/xr/openxr/extensions/XR_ANDROID_light_estimation
- Khronos, `XrLightEstimateANDROID`: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrLightEstimateANDROID.html
- Resolution Games, grounding Isle of Pigs (separate tuning curves): https://www.resolutiongames.com/blog/how-we-grounded-angry-birds-ar-isle-of-pigs-in-your-living-room-part-1
- Repo: `docs/xr-stage-grounding-report.md` (T4–T6), `extern/aurora/lib/xr/xr.cpp` (`proj` at ~3972, colour-scale tints at ~3418), `platforms/android/app/src/main/java/dev/melee/MeleeXrActivity.java`, `platforms/android/app/src/xr/AndroidManifest.xml`.
