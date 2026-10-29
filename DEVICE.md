# AvatarForge — on-device verification sheet

Everything in this repository has been verified on the host: the kernel harness runs **386 checks with
0 failures**, the arm64 native libraries link against NDK r26, and both the debug APK and the SDK AAR
are built and inspected (manifest, permissions, FileProvider, stripped natives, dex contents).
`tools/abi_conformance.py` passes for the C++ kernel (47/47) and reports the Rust core's progress
against the same ABI.

**Nothing in this file has been executed yet.** There is no Android device and no emulator in the
build environment — the emulator would need an x86_64 build plus a GPU-capable host, and the container
has neither. So this is the ordered pass a human runs on a phone, with the exact commands and the
numbers to write down. Until it is filled in, the app is *built and statically verified*, not
*device-proven*, and the project's status says so.

The one thing to watch is the **posed-geometry path**: the kernel evaluates skinning from the bind
pose, so composing poses is stateless (verified in host test `[12]`, which is the only part of the
pipeline that cannot be checked without a GPU). Rendering, frame timing and thermals are the areas
the host cannot judge at all.

---

## 0. What you need

| | |
|---|---|
| Device | arm64 Android 8.0+ (API 26) with **OpenGL ES 3.1**; that is the only backend AvatarForge implements |
| Host | `adb` from platform-tools, and a copy of this repo (for `tools/validate_glb.py`) |
| Artifact | `artifacts/avatarforge-debug-arm64.apk` (or the release one, see §1) |
| Free storage | ~2 GB on the device: every generation writes a 33–39 MB `.glb` plus its textures |
| Time | 15 minutes for the smoke path, 45 for the full sheet |

---

## 1. Install

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

The host reference is a *x86 desktop at `bodyCell 0.008` with two threads*; a mid-range phone is
expected to be 2–4× slower, i.e. 30–90 s of geometry. If a phone stage is far outside that band,
that is a finding — record it.

5. Peak memory:
   ```bash
   adb shell dumpsys meminfo com.avatarforge | grep -E "TOTAL|Native Heap"
   ```
   Expected total RSS < 1.5 GB (the engine's own budget is 1.5 GiB by default,
   `af_engine_set_memory_budget`).

## 5. The viewer

1. The model appears on a lit ground plane. Orbit with a drag, pinch to zoom.
2. Open the **Stats** drawer: it shows backend (`gles31`), frame ms, FPS, triangles, draw
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

Expected: valid glTF 2.0 binary, ~34 MB, ~270 k vertices, 71 joints, 52 blendshape channels, 5
textures at 1024².

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
   The budget controller is the app's own defence: quality steps down when the frame time exceeds
   33.3 ms for ~0.6 s. Record whether the renderer held ≥30 FPS on the last run.

## 9a. Phase 1 report — collect it with one command

The build order for this project is by phase, and every phase is judged on hardware, not on a build
log. Phase 1 (reliable rendering, GLB loading, crash-free lifecycle, profiling) has a harness that
collects the evidence in one file:

```bash
tools/device_check.sh --apk artifacts/avatarforge-debug-arm64.apk --soak 120
```

It writes `artifacts/device-report.txt` and prints the same report. It installs nothing unless
`--apk` is given and needs no network. What each column is for:

| Column | Why it is in a Phase 1 report |
|---|---|
| `app GLES context` | This app's **own** `GL_RENDERER`, `GL_VERSION` and `GL_VENDOR`, logged after GLES initialization and scoped to the running app PID. `UNAVAILABLE` means the viewer was not started, an older APK was installed, or the log was lost; it is not a pass. |
| `system GLES` / `system Vulkan` | SurfaceFlinger/phone capabilities **only**. They are not proof of the driver selected for the app or an app Vulkan dependency. |
| `declared gfx` | The platform's `opengl.*` feature flags, so a device without ES 3.1 is identified. |
| `thermal status` before/after | A soak that throttles can obscure image-quality/performance comparisons. |
| `janky`, `p50`–`p99` | Android **UI** (`gfxinfo`) pacing; SurfaceView's native GLES frame time is **not** proved by this row. Use the in-app diagnostics FPS, frame attempts, submissions and accepted presents as well. |
| `memory now` | The PSS of the running process against the < 2 GB budget. |
| `crash entries` | Native crash entries for this app, counted from the crash buffer. |

Any missing measurement is printed as `UNAVAILABLE (reason)`. In particular, do not substitute
SurfaceFlinger's GPU string for the `app GLES context` line.

**Phase 1 is complete when** a current app context reports `backend=gles31` and GL ES 3.1 or later,
non-empty frames are accepted, the in-app FPS and UI jank meet their targets (`janky` < 5 %,
`p95` < 33.3 ms, ≥ 30 native FPS), memory is under 1.5 GB, there are zero crash entries, thermal
did not reach `SEVERE`, **and the image is visually correct on the device**. Counts alone do not
prove visible/sharp pixels.

### Samsung SM-S921B: driver and blurry clip retest

1. Install the **exact-head** debug APK; run `python3 tools/verify_gles_artifacts.py <apk>` to
   reject a bundled/linked Vulkan loader. Start the viewer, play `walk` and `run`, and run
   `tools/device_check.sh --soak 120` with USB debugging authorised. Export **MENU → Self diagnostics**
   during playback. Check `Backend: gles31`, the app context line, frames accepted and thermal
   status. The previous report said `ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304)` and
   `OpenGL ES 3.2 Driver version: 25.6.4`: that is GLES implemented by the **OS's ANGLE driver**;
   the app does not initialize Vulkan. This is historical evidence, not a test of the new APK.
2. If you specifically need the **native vendor GLES driver** rather than GLES-through-ANGLE,
   use the phone's *per-app graphics-driver preference* to choose the system/vendor driver **if
   offered by that Android build**, then relaunch and repeat the report. Do not infer a change
   from the presence of a Vulkan capability flag: confirm the app's `GL_RENDERER` changes and
   still reports `backend=gles31`. The app cannot override a platform-selected translation layer.
3. With bloom **off**, capture one PNG with playback paused and one during `run` at the **same**
   camera distance/FOV. Note quality level and thermal status; compare face/texture edges and
   geometry, not screenshots at different zooms. Check orbit/pinch while playing. Share those
   screenshots, both diagnostics exports and the exact APK SHA so the source of any remaining
   blur can be isolated. This renderer has no motion-blur pass; disabling Vulkan in the app is
   not by itself a sharpness fix.

### Phase 1 host-side state (what is proven, and what is not)

| Phase 1 item | Host evidence | Device evidence |
|---|---|---|
| rendering runs | shader tables compile (12 stages), draw path exercised through the API surface | **none yet** — cannot be claimed |
| GLB loading | harness `[14]`: the exporter's own 34 MB file imports back with all 10 primitives sharing one vertex pool, 71 joints, inverse bind matrices, 52 morph targets, 5 decoded PNGs, and 45 hostile inputs rejected cleanly | **none yet** |
| crash-free lifecycle | harness `[14]` hostile-input sweep (truncation, bit flips, cyclic graphs, bad accessor ranges) under ASan/UBSan; cancel/rotate paths are §9 below | **none yet** |
| profiling | harness `[15]`: CPU stage timings exact against a synthetic clock, GPU timings reported as −1 when the device cannot measure them | **none yet** |
| scene, culling, LOD | harness `[15]`: parent-to-child composition, conservative sphere culling, monotone LOD, deterministic light order | **none yet** |

The honest summary: Phase 1 is *implemented and host-verified*, and not one row of it is
device-verified. The report above is how that changes.

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
