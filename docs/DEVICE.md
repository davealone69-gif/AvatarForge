# AvatarForge — on-device verification sheet

Host verification passes: **464/464** native checks, **47/47** C++ ABI symbols, **20/20** Kotlin/JVM
tests, Rust workspace tests and **72/72** prompt-parity cases. The pinned JDK 17 / Gradle 8.9 / SDK 34 /
NDK r26b toolchain has built the current arm64 native libraries, debug/release APKs and release AAR.
Static checks pass for API 26 minimum / API 34 target, no `INTERNET`, native and JNI symbols, APK
alignment and v2 signatures. Rust remains partial at 15/47 C ABI symbols. The app includes a live
one-second device report and a tested generation cutoff at Critical (4)+; these policies have not been
observed on a physical device.

**No on-device step in this file has been executed.** `adb devices -l` is empty and the user's USB plug
does not work; no Galaxy S24 SM-S921B or emulator/GPU is available. This is the ordered pass to run on
the target phone. The current APK/AAR are real built artifacts, but installation, offline generation,
Vulkan/GLES behavior, compressed GPU uploads, lifecycle, visual quality, performance, memory and
thermal behavior remain UNVERIFIED.

The **posed-geometry path** has host regressions (`[12]` and the Rust end-to-end test) proving that
clips are evaluated from bind vertices and REST restores the baseline. On-device, confirm the
renderer displays those poses correctly; actual GPU rendering, frame timing and thermals remain
outside what the host can judge.

---

## 0. What you need

| | |
|---|---|
| Device | arm64 Android 8.0+ (API 26), Vulkan 1.1 preferred — any GLES 3.1+ device also works, the renderer falls back |
| Host | `adb` from platform-tools, and a copy of this repo (for `tools/validate_glb.py`) |
| Artifact | `artifacts/avatarforge-debug-arm64.apk` (or the release one, see §1) |
| Free storage | ~2 GB on the device: every generation writes a 33–39 MB `.glb` plus its textures |
| Time | 15 minutes for the smoke path, 45 for the full sheet |

---

## 1. Install

The current APK is signed with the local debug certificate SHA-256 `960c75a9…`. The preceding
artifact set used a different debug certificate (`9d24f3af…`), so Android may reject it as an in-place
update over that build. Export saved avatars before uninstalling the older app; uninstalling can erase
app-private data.

```bash
adb install -r artifacts/avatarforge-debug-arm64.apk
# or, after `tools/build_gradle.sh :app:assembleRelease`:
adb install -r app/build/outputs/apk/release/app-release.apk
```

Expected: `Success`. The app appears as **AvatarForge**, no launcher icon from a stock set (it ships
its own vector).

## 2. Permission and network audit — must pass

```bash
adb shell dumpsys package com.avatarforge | grep -A4 "requested permissions"
```

Expected: exactly one line, `com.avatarforge.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION` (an AndroidX
internal receiver permission). **`android.permission.INTERNET` must not appear.** If it does, treat the
build as failed — the whole point of this project is that it works with the radio off.

```bash
adb shell cmd connectivity airplane-mode enable      # or use the quick-settings tile
adb shell settings get global airplane_mode_on       # -> 1
```

Turn off Wi-Fi as well, then launch the app and run §4. Everything must work identically.

## 3. Launch and first paint

1. Launch the app. The Input screen appears with the prompt field, the build controls, and the two
   image slots (Image A = identity, Image B = equipment reference).
2. Take a screenshot with the stats visible — you will compare frame times later:
   `adb exec-out screencap -p > before.png`.

## 4. Text-only generation (the baseline path)

1. Prompt: `a tall athletic woman in a green linen tunic, athletic build, ponytail`.
2. Leave Image A and Image B empty. Press **Generate**.
3. Watch the progress screen. It reports the current stage, the stage fraction and an overall bar that
   is weighted by measured stage cost — it should advance through
   `fit anatomy → geometry → UV unwrap → bake textures → rig → blendshapes → animation → export`
   without ever jumping to 100% and stalling.
4. Write down the per-stage times from the result card (`EngineResult.timings`, shown in the viewer):
   those are kernel timings, not wall-clock guesses.

 | Stage | device time | host reference |
|---|---|---|
| fit anatomy | | 0.3 s |
| geometry (field + isosurface + topology + cloth + hair) | | 29.4 s |
| UV unwrap | | 0.35 s |
| bake textures | | 0.50 s |
| rig (skeleton + weights) | | 1.04 s |
| export (.glb) | | 1–2 s |

The host reference is a *x86 desktop at `bodyCell 0.008` with two threads*. Any phone-time band is a
planning estimate only, not a measurement or promise; record actual stage times on the device rather
than extrapolating from host results.

5. Peak memory:
   ```bash
   adb shell dumpsys meminfo com.avatarforge | grep -E "TOTAL|Native Heap"
   ```
   Target total RSS < 1.5 GB, measured rather than assumed. The SDK's default requested native
   generation budget is 1.5 GiB, not a guaranteed fixed allocation; it may lower the request to
   measured available RAM minus 512 MiB of headroom (with a 64 MiB floor) before starting a build.

## 5. The viewer

1. The model appears on a lit ground plane. Orbit with a drag, pinch to zoom.
2. Open the **Stats** drawer: it shows backend (`vulkan`/`gles3`), frame ms, FPS, triangles, draw
   calls, and the **quality level** the budget controller has settled on.
3. Requirement: **≥30 FPS** while orbiting with the default (AUTO) quality. Orbiting fast on a
   mid-range phone should *step the quality down*, not drop frames: the three knobs the controller
   touches are MSAA, shadow-map size and bloom levels — camera, exposure and the model never change.
   Let it settle for ~10 s and read the level again: with headroom it climbs back slowly (step-up only
   after ~4 s of sustained 60 FPS).
4. Toggle the overlays: **skeleton** (joint lines over the mesh), **flat shading**, and the
   **region highlight** (pick a region id and the matching submesh is highlighted).

## 6. Animation, pose modes and blendshapes — the interactive core

Under **Animation**:

1. **Pose modes.** Tap `T-pose`, then `A-pose`, then `Rest`, then `Clip playback`. Each is a real
   library clip applied to the skeleton; `Rest` must return the avatar *exactly* to the build pose
   (this is host test `[12]` on the device — if REST leaves the avatar subtly skewed, the deform path
   is wrong and that is the highest-priority bug in the whole sheet).
2. **Clips.** Play `walk`, then `run`, then `wave`, then `jump`. All 15 clips are authored key data
   baked into the kernel. Check a fast one (`run`, 0.62 s) for a clean loop with no snap at the wrap.
3. **Blendshapes.** In the face panel pick `mouthSmileLeft`, slide to 1.00 → the left mouth corner
   lifts; slide back to 0 → the face returns. Now start `walk` and set
   `mouthSmileLeft` to 1.00 *while it plays* — the smile must survive the pose change (host test `[12]`
   covers this; on device confirm it visually).
4. `Reset all channels` must clear every ARKit channel to 0.

## 7. Two-image multi-reference flow (the generator's second half)

1. Prepare **Image A**: a portrait or half-body photo (any person). Prepare **Image B**: a photo of
   armour or a garment on a plain background.
2. Load both, tag A as `IDENTITY` and B as `EQUIPMENT` (the slots are labelled).
3. Generate. Expected behaviour, in stages, body first and equipment second so the peak stays inside
   the RAM ceiling:
   * the body is built from A's proportions and skin tone, with the lower body **inferred** when A
     only shows the torso;
   * B is segmented from its background and transferred to the body both geometrically (silhouette
     influence on the SDF) and texturally (albedo/roughness contribution on the affected submeshes).
4. Compare against a run with A alone: the equipment pass must visibly add parts (pauldrons, plates,
   boots) rather than recolour the skin. Open the **Inspect** panel and tap the added geometry — the
   region name should read as the equipment region (`CLOTH`/`METAL`), not `SKIN`.
5. Re-run the *same* pair twice: geometry must be byte-identical apart from the seed-dependent
   details (the kernel is deterministic — host test `[6]`).

## 8. Export

Three independent paths, all real:

| Path | Expected |
|---|---|
| **Save as…** (SAF) | System picker appears, writing to Downloads succeeds, toast reports the MB written. No storage permission is requested. |
| **Copy to app storage** | `adb shell run-as com.avatarforge ls -l files/avatars/` shows `avatar-<id>.glb` |
| **Share** | Android share sheet offers the file as `model/gltf-binary`; send it to yourself and open it in any glTF viewer. A `FileProvider` URI is used (`content://com.avatarforge.avatarforge.fileprovider/...`), not `file://`. |

Verify the exported model properly on the host:

```bash
adb shell run-as com.avatarforge cat files/avatars/avatar-*.glb > /tmp/avatar.glb
python3 tools/validate_glb.py /tmp/avatar.glb
python3 tools/glb_census.py /tmp/avatar.glb      # vertices, joints, 52 morph targets, textures
```

Expected: valid glTF 2.0 binary, ~34 MB, ~270 k vertices, 71 joints, 52 blendshape channels, and 5
textures. Atlas resolution is budget-selected (typically 2048² at the requested default budget; it may
fall to 1024², 512² or 256² on tighter budgets).

## 9. Lifecycle, memory and thermals

1. **Cancel** a generation halfway (progress screen → Cancel). The job must stop and the input screen
   return; no orphaned work and no crash. Start a second generation immediately: the progress bar
   must start at 0 % (the accumulators are reset per job), and the result must be correct.
2. Rotate the device during generation and in the viewer: the surface is recreated, the renderer
   re-initialises on the same engine handle and the model is still there.
3. Background the app mid-generation, bring it back: the job continues (it is not tied to the
   surface), the UI re-attaches.
4. Run four generations back to back and watch the battery/thermal behaviour:
   ```bash
   adb shell dumpsys battery | grep -E "level|temperature"
   adb shell dumpsys thermalservice | head -20
   ```
   The native source policy is intended to step quality down when frame time exceeds 33.3 ms for
   roughly 0.6 s. Kotlin preflight refuses to start under low-memory or low-storage conditions. On API
   29+, generation is allowed through Severe (3), but blocks at Critical (4) and hotter; an active job
   is checked about once per second and cooperatively cancelled if it reaches Critical+. The renderer
   drops to LOW at Severe+ and restores the requested preset on recovery. The SDK caps the requested
   native generation budget to live RAM headroom and (on Android 12+) reports render-thread work
   duration through ADPF. These paths compile and pass host/JVM tests, but hardware effects are
   UNVERIFIED. Record whether the renderer held ≥30 FPS on the last run and whether a deferred build
   recovers after resources normalize.

## 9a. Phase 1 report — collect it with one command

The build order for this project is by phase, and every phase is judged on hardware, not on a build
log. Phase 1 (reliable rendering, GLB loading, crash-free lifecycle, profiling) has a harness that
collects the evidence in one file:

```bash
tools/device_check.sh --apk artifacts/avatarforge-debug-arm64.apk --soak 120
```

It writes `artifacts/device-report.txt` and prints the same report. It installs nothing unless
`--apk` is given, needs no network, and changes nothing on the device. What each column is for:

| Column | Why it is in a Phase 1 report |
|---|---|
| `gles renderer`, `vulkan` | which backend the device can actually run, and therefore which of the two renderer paths is being judged |
| `declared gfx` | the `vulkan.*` / `opengl.*` feature flags, so a GLES-only device is identified as such instead of looking like a Vulkan failure |
| `thermal status` before/after | a soak that throttles is not a rendering bug; without this column it looks like one |
| `janky`, `p50`–`p99` | frame pacing over real frames, which is the only honest source for "30 FPS minimum" |
| `memory now` | measured process PSS; target <1.5 GiB, with 2 GB as the broader app-process ceiling |
| `crash entries` | any native crash during the run, counted from the crash buffer rather than assumed absent |

Any row the device will not report is printed as `UNAVAILABLE (reason)`. It never prints a zero in
place of a missing number — a blank that looks like a pass is the failure mode this project forbids.

**Phase 1 is complete when** that report shows: a renderer string, Vulkan or `GLES 3.1` (not lower),
`janky` under 5 %, `p95` under 33.3 ms, `memory now` under 1.5 GB, zero crash entries, and a thermal
status that did not reach `SEVERE` or `CRITICAL` during the soak.

### Phase 1 host-side state (what is proven, and what is not)

| Phase 1 item | Host evidence | Device evidence |
|---|---|---|
| rendering runs | 13 shader stages compile/check with NDK `glslc`; arm64 APK and native libraries build; single-avatar culler is host-tested | **none yet** — no GPU/device run |
| GLB loading | harness `[14]`: the exporter's own 34 MB file imports back with all 10 primitives sharing one vertex pool, 71 joints, inverse bind matrices, 52 morph targets, 5 decoded PNGs, and 45 hostile inputs rejected cleanly | **none yet** |
| crash-free lifecycle | harness `[14]` hostile-input sweep under ASan/UBSan; Kotlin lifecycle/renderer lease sources compile and JVM tests pass | **none yet** — rotate/cancel/background/release paths untested on hardware |
| profiling | harness `[15]`: CPU stage timings exact against a synthetic clock, GPU timings reported as −1 when the device cannot measure them | **none yet** |
| scene, culling, LOD | harness `[15]` tests transforms/LOD/lights; `[10b]` culler; `[10e]` index-only LOD; GL/Vulkan compile in the arm64 NDK build | **none yet** — rendered LOD appearance, deformation safety and performance need a device/GPU pass |

The honest summary: importer, profiler, culler, adaptive atlas chooser, index-only LOD, GPU-profile
policy and ASTC/ETC2 CPU encoding have host coverage. GL/Vulkan/JNI sources and Kotlin SDK/app have
compiled with the Android toolchain; GPU uploads and UI/runtime effects remain untested. No row is
device-verified yet. This report is how that changes.

## 9b. Live device report — no USB/ADB needed

On the phone, open **☰ MENU → Live device report**. The dialog refreshes every second using Android's
local system APIs; it uses no network, USB or ADB. It displays the Android thermal level, current and
total RAM, free storage, low-memory flag and available CPU cores. Thermal status is explicitly shown
as **Unavailable** on API 26–28, where Android does not expose the telemetry used by this feature.

The generation policy shown in the report allows generation through Severe (3), blocks new generation
at Critical (4)+, and requests cooperative cancellation of an active job if Critical+ is detected.
This generation cutoff is distinct from the stricter Phase 1 soak criterion above, which still aims
for no Severe/Critical reading during the soak. The report itself and cutoff compile and pass JVM
regression tests, but no physical reading or thermal-response behavior has yet been observed. It is
supporting telemetry, not a replacement for renderer/GPU/FPS/soak acceptance. The app calls this
Android's thermal pressure signal; it is not a battery-temperature measurement.

## 10. Record sheet

| Check | Result | Notes |
|---|---|---|
| install | | |
| permissions — no INTERNET | | |
| airplane mode, full run | | |
| geometry time / total time | | |
| peak RSS | | |
| FPS at AUTO quality (orbiting) | | |
| quality level after 30 s | | |
| REST restores the bind pose | | |
| blendshape survives a pose change | | |
| 15 clips play | | |
| two-image flow adds equipment | | |
| GLB validates on the host | | |
| share uses a FileProvider URI | | |
| cancel + immediate second run | | |
| battery / thermal after 4 runs | | |
| `tools/device_check.sh` report attached | | |
| Phase 1: janky % / p95 frame time | | |
| Phase 1: thermal status after soak | | |
| Phase 1: GLB imported through the viewer | | |

Send the filled sheet back and it goes into `STATUS.md` with the numbers attached — at that point the
project can claim device verification instead of host verification.

