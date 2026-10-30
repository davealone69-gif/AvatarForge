# AvatarForge — on-device verification sheet

## Current host evidence — 2026-10-30

`bash tools/verify.sh` passes: the kernel harness reports **438 checks, 0 failures**; renderer tests
report **102 checks, 0 failures** plus **7 API-version checks**; model-detail regressions report
**40 checks, 0 failures**. Compile-only artifact policy, shader, export, ABI and Rust workspace gates
also pass. Gradle produced the debug APK, unsigned compile-only AAB and SDK AAR, and the packaged
native-linkage guard passed. These results verify builds and host behavior, not device pixels.

## User-supplied SM-S921B recovery report — not independently reproduced

The owner supplied an AvatarForge recovery export for a Samsung **SM-S921B** (API 37, arm64-v8a).
It records the following session:

| Reported field | Device evidence |
|---|---|
| Requested / actual backend | `VULKAN` requested; `gles31` active |
| Recovery | `FALLBACK: Native Vulkan did not initialize`; GLES active |
| GPU string | ANGLE on Samsung Xclipse 940 / Vulkan 1.3.304 |
| Thermal state | `THERMAL_LIMIT`, **critical** |
| Frame pacing | **29.3 FPS**, 34.11 ms input-delta EMA (not present-completion timing) |
| CPU submit / GPU time | 20.18 ms / 25.81 ms as reported by the app |
| Frame counters | 1,740 attempts, submissions and accepted presents |
| Scene counters | 422,600 triangles, 15 draw calls; texture memory 90.7 MiB |
| Effective quality | Level 0, MSAA 1×, 1024 shadow map, bloom 0; last error `none` |

This report is evidence that the app used its GLES fallback in that session; the ANGLE/Vulkan GPU
string does **not** mean AvatarForge's native Vulkan renderer was active. The report explicitly warns
that accepted presents do not prove visible pixels. It was not reproduced here: `adb devices -l` is
empty in this environment, and no screenshot was supplied. The export contains no APK SHA or source
commit, so these values cannot be attributed to the working-tree build. Actual image correctness and
an independent runtime test therefore remain **UNVERIFIED**. The checklist below is still the full
on-device pass to run when direct device access is available.

The posed-geometry path is host-tested: skinning is evaluated from the bind pose, so composing poses
is stateless. Rendering, frame pacing, GPU behavior and thermals still require device evidence.

---

## 0. What you need

| | |
|---|---|
| Device | arm64 Android 8.0+ (API 26) with a Vulkan loader and OpenGL ES 3.1 fallback |
| Host | `adb` from platform-tools, and a copy of this repo (for `tools/validate_glb.py`) |
| Artifact | exact-head CI debug APK or a locally built arm64 debug APK |
| Free storage | ~2 GB on the device: every generation writes a 33–39 MB `.glb` plus its textures |
| Time | 15 minutes for the smoke path, 45 for the full sheet |

---

## 1. Install

```bash
adb install -r artifacts/avatarforge-debug-arm64.apk
```

Use the debug APK only; release-variant outputs are unsigned compile/package checks and are not
installable test builds. Expected: `Success`. The app appears as **AvatarForge**, no launcher icon
from a stock set (it ships its own vector).

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
2. Open the **Stats** drawer: it shows the backend actually initialized (`vulkan` or `gles31`),
   frame interval, FPS, triangles, draw calls, and the **quality level** the budget controller has
   settled on. Timing remains explicitly in warm-up state until a real render-loop interval exists.
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

1. Prepare **Image A** for coarse framing/proportion and appearance cues; prepare **Image B** for
   garment/armour colour and metal-confidence cues. The analyzer is deterministic and does not
   reconstruct identity, segment source garments into geometry, or transfer source pixels.
2. Load either or both images. Confirm that the A/B analysis cards show measurements before pressing
   Generate, and that clearing or replacing an image does not leave stale results.
3. If decoding or analysis fails, the app must show a recoverable message and must not freeze the
   main thread or silently claim that the image contributed.
4. Generate with both images and compare against a text-only run. Confirm that preview and generated
   specs reflect the displayed analysis, and that the output remains an original procedural avatar.
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

**Phase 1 is complete when** Self diagnostics reports the actual selected backend (`gles31`
with GL ES 3.1+, or native `vulkan` with its own API/driver string), non-empty frames are accepted,
the in-app FPS and UI jank meet their targets (`janky` < 5 %, `p95` < 33.3 ms, ≥ 30 native FPS),
memory is under 1.5 GB, there are zero crash entries, thermal did not reach `SEVERE`, **and the
image is visually correct on the device**. The `app GLES context` line applies only to GLES;
system Vulkan capability and frame counters alone do not prove visible/sharp pixels.

### Samsung SM-S921B: driver and blurry clip retest

1. Install the **exact-head** debug APK; run `python3 tools/verify_gles_artifacts.py <apk>` to
   require system Vulkan/GLES linkage and reject a **bundled** Vulkan loader. Start with the current
   **Vulkan default**, play `walk` and `run`, and run `tools/device_check.sh --soak 120` with USB
   debugging authorised. Export **MENU → Self diagnostics** during playback. Record the actual
   backend, physical-device API/driver, accepted presents, CPU frame time and thermal status; also
   capture the `AF_VK_CONTEXT` log line, which separates the requested target label, application API,
   compiled header revision, loader API and physical-device API.
2. Select **GLES** explicitly and repeat the same clip, lifecycle, orbit and thermal checks. Confirm
   `Backend: gles31` and capture the app's GLES context/driver. Test the bounded fallback by recording
   the actual error and BACKEND_FALLBACK journal entry when Vulkan startup is unavailable; a fatal
   Vulkan runtime error must stop that backend and recover through GLES rather than count blank frames.
   The app requests Vulkan 1.3 using `VK_API_VERSION_1_3` (patch zero; Vulkan ignores the `apiVersion`
   patch field). The supplied checker label `1.3.304` is separate device-capability evidence, not proof
   that AvatarForge created an instance, selected Vulkan, or rendered correctly.
3. With bloom **off**, capture paused and running PNGs for **each** backend at the same
   camera distance/FOV and quality/thermal state. Compare actual face/cloth/gear texture edges,
   geometry and motion; the UV fix is a bake change, so regenerate the avatar with this APK rather
   than reopening an older export. Share the matched captures, diagnostics and exact APK SHA.
   This renderer has no motion-blur pass; Vulkan capabilities alone cannot establish sharpness.

### Phase 1 host-side state (what is proven, and what is not)

| Phase 1 item | Host evidence | Device evidence |
|---|---|---|
| rendering runs | 12 GLES stages / 13 Vulkan SPIR-V stages are gated; GLES draw path and API selection are host-tested, not Vulkan pixels | User-supplied report records GLES fallback and 1,740 accepted presents; not independently reproduced and no visual proof |
| GLB loading | harness `[14]`: the exporter's own 34 MB file imports back with all 10 primitives sharing one vertex pool, 71 joints, inverse bind matrices, 52 morph targets, 5 decoded PNGs, and 45 hostile inputs rejected cleanly | **none yet** |
| crash-free lifecycle | harness `[14]` hostile-input sweep (truncation, bit flips, cyclic graphs, bad accessor ranges) under ASan/UBSan; cancel/rotate paths are §9 below | **none yet** |
| profiling | harness `[15]`: CPU stage timings exact against a synthetic clock, GPU timings reported as −1 when the device cannot measure them | **none yet** |
| scene, culling, LOD | harness `[15]`: parent-to-child composition, conservative sphere culling, monotone LOD, deterministic light order | **none yet** |

The honest summary: Phase 1 is *implemented and host-verified*. The supplied report gives useful but
unreproduced evidence of a GLES fallback, critical thermals and sub-30 FPS input-delta pacing; it does
not complete device validation or prove a visible image. A direct run with logs and screenshots is
still required.

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
