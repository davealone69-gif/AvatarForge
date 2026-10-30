# AvatarForge — status

**Current branch (2026-10-30).** The default is GLES 3.1, with an explicitly selectable native
Vulkan backend restored. The Vulkan path has source/host checks but no completed device pixel test;
see [PROJECT_STATUS.md](PROJECT_STATUS.md) and [the restoration report](docs/VULKAN_RESTORE_2026-10-30.md).
Production signing and the signed GitHub Release workflow are intentionally removed; the active
artifact contract is a debug test APK plus explicitly unsigned compile-only AAB/AAR outputs. See the
current signing-path record in [PROJECT_STATUS.md](PROJECT_STATUS.md).
The entries below preserve an older 2026-10-18/29 checkpoint and its artifact references; they
are not proof that the current branch was built or tested on a phone.

**What this is.** A standalone Android app that generates, auto-rigs, animates and renders a
photorealistic full-body 3D avatar entirely on the device, from a text prompt, a photo, or a
two-image reference flow (identity + equipment). No server, no network, no game engine.

**Where it stands.** The kernel, renderer, SDK and app build; the current host suite passes 438 kernel
checks, 102 renderer checks, 7 API checks and 40 model regressions. A user-provided SM-S921B/API 37
recovery report records requested Vulkan falling back to `gles31`, critical thermal state and 29.3 FPS
by input-delta EMA. The report has no APK SHA/commit, is not independently reproduced, and accepted
presents do not prove visible pixels. No device is attached to this environment; image correctness and
live renderer behavior remain unverified. See [the device verification sheet](DEVICE.md).

### 2026-10-29 stability self-improvement path

A no-AI, offline renderer-stability path is implemented: bounded safe fallback (max two automatic
attempts), private fault journal, frame-submission probe, and copyable/exportable diagnostics report.
It is covered by SDK JVM tests and the renderer host suite in CI. **Device visibility remains
unverified**; the probe does not claim that pixels are visible. See
[docs/SELF_IMPROVEMENT_PATH.md](docs/SELF_IMPROVEMENT_PATH.md).

### 2026-10-29 device fault

A supplied phone screenshot shows successful generation (417,150 triangles) followed by a viewer
startup failure: Vulkan was disabled in settings and GLES initialization failed. The GLES path had
invalid sampler/VAO setup ordering and discarded useful driver diagnostics. Those paths now have
host fault-injection coverage, explicit retry/backend switching, conservative MSAA/HDR fallback, aligned camera UBO slices, and a restored Self diagnostics menu.
**The phone has not been retested; visible rendering and FPS remain unverified.** See
[the fault report](docs/RENDERER_FAULT_2026-10-29.md).

### 2026-10-18 continuation status

- The complete 28-float image-analysis wire format and 1–4 record regressions pass. The full native
  host harness passes **405/405** checks, including exact packing and adult-only nude validation.
  Kotlin/JVM SDK tests pass **7/7**, and the standalone Rust core passes **31/31**.
- `AF_OUTFIT_NONE` is now labelled as the real **adult-only nude anatomical base** in the Android UI.
  Age/prompt safety is enforced independently in Kotlin, the C ABI, and Rust. Detailed genital,
  nipple, areola, and pubic-hair controls remain hidden because their geometry/material path is not
  implemented.
- Two supplied lists are preserved under `requirements/custom-inputs/`: 58 general customization
  inputs and 10 adult-anatomy inputs. Deterministic normalized contracts live in
  `config/customization/` and ship as offline SDK assets. The production schema gate exposes only
  `VERIFIED` inputs; `PARTIAL` and `BLOCKED_*` entries cannot become placeholder controls.
- A temporary pinned JDK 17/Android SDK/NDK r26b toolchain rebuilt the 0.10.1 arm64 native libraries,
  debug/release APKs, and SDK AAR. Both APKs are aligned, v2-signed, contain the normalized offline
  schema assets, report engine 0.10.1, and request no `INTERNET` permission. Airplane-mode,
  lifecycle, GPU, performance, and visual validation still require a physical Android device.

---

## What the artifacts in `artifacts/` actually contain

The debug APK, release APK, and AAR were rebuilt on 2026-10-18 from the current 0.10.1 source with
NDK r26b for arm64-v8a. Static inspection verifies all three native libraries, both normalized
customization assets, engine version 0.10.1, APK alignment/signature, and no `INTERNET` permission.
The release APK uses the fallback debug certificate because no release keystore was supplied. The
sample GLB was not regenerated in this pass and remains a prior valid export; it must not be used as
proof of the latest source version.

Known compiler warnings still present (all unused-variable/parameter class, none behavioural):
`af_field.cpp:274 backZ`, `af_image.cpp:321 fracToY`, `af_isosurface.cpp:167 field`,
`af_isosurface.cpp:654 a`, `af_rig.cpp:206 isArm`, `af_rig.cpp:414 jawR`, `af_rig.cpp:566 e`.

## Build order

The project is built in four phases, in this order. Every item is either **implemented**, **not
started**, or **device-verified** — never "probably fine".

| Phase | Scope | State |
|---|---|---|
| **1. Reliable rendering, GLB loading, crash-free lifecycle, profiling** | GLB import, scene graph, culling/LOD, frame profiler, hostile-input robustness | **implemented + host-verified** (harness `[14]`, `[15]`); renderer wiring for the new scene layer is the remaining code step. **0 % device-verified** |
| **2. Vulkan, LOD, texture compression, device profiles** | Vulkan as the primary backend (already written, never executed on a GPU), GLES 3.1 fallback, per-tier profiles, ASTC/ETC2 | custom Vulkan + GLES backends written; LOD selection landed in Phase 1's scene layer; **ASTC/ETC2 and per-tier profiles not started** |
| **3. ADPF, adaptive quality, multithreaded jobs** | thermal headroom feedback, quality controller, job graph | not started; the quality input the controller needs (`buildVisibility(camera, quality, …)`) exists |
| **4. GPU-driven rendering, advanced lighting, optional ray tracing** | indirect draws, Forward+ light culling, RT where available | not started |

Two decisions that shape all four phases:

* **The renderer is custom (Vulkan primary, GLES 3.1 fallback), not Filament.** The brief allowed
  Filament "if using" it, but the standing rule for this project is a renderer written from scratch
  with no pre-existing engine, so Filament is used as a reference for ideas only and never linked.
* **Nothing is device-verified yet.** There is no phone and no GPU in this build environment, so the
  claim this repo makes is *implemented and host-verified*; `tools/device_check.sh` exists so that a
  single command turns that into evidence on real hardware, and `DEVICE.md` §9a defines the numbers a
  phase has to hit.

## Verified in this session

| Claim | Evidence |
|---|---|
| Kernel host harness passes | `tools/build_host.sh test` → **All 386 checks complete: 0 failure(s)** (`build-host/host_tests_run.txt`) |
| GLB import into a live engine | harness `[16]`: the export loads back through the ABI into an empty engine (285 657 vertices / 382 388 triangles, exactly what was generated), regions survive, the file's skeleton is refused (joint count 0, clip playback rejected), a second import is idempotent, and a hostile buffer leaves vertices, triangles and rig untouched |
| GLB round trip | harness `[14]`: the exporter's own 34 MB file imports back — 10 primitives sharing **one** vertex pool (266 797 unique vertices instead of 2 134 376), 71 joints, inverse bind matrices, 52 morph targets, 5 PNGs decoded; 45 hostile inputs (truncation, bit flips, cyclic graphs, out-of-range accessors) rejected cleanly |
| Scene, culling, LOD, profiling | harness `[15]`: transform composition, conservative sphere culling, monotone LOD in distance, deterministic light order, profiler exact against a synthetic clock and reporting **−1** (not 0) for GPU timings the device cannot measure |
| Rust core parity | `tools/abi_conformance.py rust/target/release/libafcore.so --reference build-host/libafcore.so` → 15/47 symbols, all invariants pass, identical clip/blendshape/region tables and **byte-identical prompt analysis over 72 corpus prompts** |
| Prompt grammar parity | `tools/prompt_parity.sh` → **72 prompts, C++ and Rust identical** |
| Posing is stateless and `REST` is exact | harness `[12]` → the walk moves 280 290 of 280 290 vertices (max 0.286 m) and `REST` leaves **0** of them off the bind pose; walk → REST → walk is **bit-identical** to walk alone |
| Android natives build | `tools/build_android.sh` → `libafcore.so` 11 199 560 B, `libafrender.so` 1 846 152 B, `libavatarforge.so` 652 624 B (arm64-v8a, NDK r26b, static STL) |
| Debug APK builds, no warnings | `tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease` → **BUILD SUCCESSFUL in 47 s**, 0 Kotlin warnings |
| Release APK builds and signs | `:app:assembleRelease` → **BUILD SUCCESSFUL in 24 s**, `app-release.apk` 6 784 655 B, signed (debug cert — no keystore supplied) |
| No network capability | `aapt2 dump permissions` on both APKs → only `com.avatarforge.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION`; **no `INTERNET`** |
| Export surface intact | `aapt2 dump xmltree` → `FileProvider`, `exported=false`, `grantUriPermissions=true`, authority `com.avatarforge.avatarforge.fileprovider` |
| Natives packaged and stripped | `file` on the APK's `lib/arm64-v8a/*.so` → *stripped*, arm64, "for Android 26" |
| Shaders compile and validate | `tools/embed_shaders.py --check` → 12 stages, 18 096 SPIR-V words, `spirv-val` clean, GLES dialect type-checked |
| Blendshapes all move real vertices | `tools/shape_probe.cpp` → **0 empty, 0 sub-millimetre** across all 52 ARKit channels |
| Topology for every body type | harness `[4/5]`: four build variants, **0 open edges, 0 boundary loops, 0 holes** each |
| Nothing regressed | `tools/verify.sh` → all host-side sections pass |
| Device evidence collector | `tools/device_check.sh` → refuses to report without a device (exit 3, explicit message); with one, writes `artifacts/device-report.txt` |

## What changed in this session (build order: Phase 1)

Phase 1 is *reliable rendering, GLB loading, crash-free lifecycle and profiling*. What landed:

1. **GLB import exists — file reader and engine entry point.** `native/src/af_glb_read.{h,cpp}` + `af_json.{h,cpp}` + `af_png_decode.{h,cpp}`
   — a bounded JSON reader (depth and node caps, UTF-8/`\u` handling, no locale dependence), a
   colour-type-0/2/3/4/6 PNG decoder with a hand-written inflate, and a glTF 2.0 reader covering
   accessors (incl. **sparse**, which the exporter's morph targets require), skins, animations and
   embedded images. glTF extensions are refused, not guessed at.

   `af_engine_import_glb(path)` and `af_engine_import_glb_from_memory(bytes,size)` adopt a file into
   a live engine: vertices, indices, submeshes (regions recovered from the exporter's material
   names), materials and the base-colour texture are replaced; the generated rig, field, UV atlas and
   clip caches are **cleared**, not left stale. The file's own skeleton and clips are deliberately
   **not** adopted — a figure that looks rigged but cannot animate is exactly the plausible fake this
   project forbids — and every import that skipped one says so through `AF_LOG_WARN`. The import is
   assembled beside the live state and swapped at the end, so a rejected file leaves the engine
   byte-for-byte as it was (asserted).
2. **The engine can adopt a GLB.** `af_engine_import_glb` / `af_engine_import_glb_from_memory` add
   two entry points to the C ABI (45 → **47 symbols**, engine version **0.10.1**). Harness `[16]`
   drives the round trip through the ABI: 285 657 vertices / 382 388 triangles come back exactly,
   regions survive because the exporter names materials after them, the file's skeleton is refused
   (joint count 0, clip playback rejected with an error), a second import is idempotent, and a
   hostile buffer leaves vertices, triangles and the rig untouched.
3. **The importer found a 10x memory bug in the asset path.** The exporter (correctly) lets all ten
   region primitives share one POSITION accessor; the first reader copied it per primitive —
   2 134 376 vertices ≈ 188 MB of `af_vertex` for a 271 k-vertex avatar. Primitives now share a
   vertex pool: **266 797 stored, 1 pool, 10 index ranges.** Harness `[14]` asserts the memory bound
   so it cannot regress.
4. **Scene layer** (`af_scene.{h,cpp}`): node graph with cycle detection, cameras, frustum
   extraction, conservative sphere culling, per-instance LOD with quality scaling, and light culling
   with a deterministic order. Host-tested; **not yet consumed by the renderer** (that wiring is the
   next Phase 1 step, and it is listed as such in DEVICE.md rather than claimed).
5. **Frame profiler** (`af_profiler.{h,cpp}`): per-stage CPU timings over a fixed window with
   min/median/max/p95 and a missed-budget fraction. GPU timings are accepted only when a backend
   declares the capability; otherwise every GPU field reads **−1** and the UI must say "not
   available", never show a plausible zero.
6. **`tools/device_check.sh`** — the evidence collector for the golden rule: identity, GPU/renderer
   strings, Vulkan/GLES feature flags, thermal status before/after a soak, gfxinfo jank and p50–p99
   frame times, process PSS, crash-buffer count. Prints `UNAVAILABLE (reason)` rather than inventing
   a number, and exits 3 with an explicit message when no device is attached.
7. **Two real bugs found by the new tests**: the profiler's stage clock never advanced (every stage
   read 0 ms), and the LOD quality scaling was inverted (lower quality selected a *finer* level).
   Both are fixed and now asserted.

### Earlier in this session

1. **The deform path is now evaluated from the bind pose, not from the displayed mesh**
   (`af_engine.cpp::applyCurrentPose`). The old code deformed the *current* geometry, so pose
   changes compounded: against the pre-fix library, `tools/pose_probe.cpp` shows `walk` → `REST`
   leaving **272 500 of 272 500 vertices** off the bind pose (max 0.2755 m) and a second `walk`
   differing from the first by the same amount — the avatar destroyed itself the moment a clip
   changed, and an export taken after any pose inherited that skew. Skinning now always starts from
   `restMesh` (blendshape weights still apply on top, in the order the GLB morph targets imply).
   Host test `[12]` locks this down: REST is exact, repeat playback is bit-identical, and a smile
   applied while walking survives the pose change.
2. **Host test `[12]` added** (16 checks) plus `tools/pose_probe.cpp` for interactive debugging. The
   harness grew from 259 to 275 checks at that point (it is 386 now).
3. **Three Kotlin warnings that were hiding two real bugs are gone:**
   * `exportGlbBytes` took both an `EngineResult` and an `AvatarSpec`; the result's own spec is the
     one that matters, so the redundant parameter (and the unused one) is dropped.
   * the dead local `weighted` in `AvatarEngine.start()` was a symptom: the progress accumulators
     `overall` / `lastProgressMs` were **never reset between jobs**, so a second generation started
     its progress bar where the first finished. They are now cleared per job.
   * the viewer's pose panel had an `onPose` callback nothing called - i.e. a **"Pose edit" line that
     reported a state the user could not change**. It is now a real control: `Rest` / `T-pose` /
     `A-pose` / `Clip playback` chips, backed by `AvatarEngine.clipIndex(name)` and the renderer's
     `setClip(clip, speed=0, loop=false)` (a held first key of a real library clip, same skinning
     path as playback). No magic clip ids in the UI any more.
4. **Release signing is fail-closed.** `app/build.gradle.kts` uses `keystore.properties` or the
   `AF_RELEASE_*` environment variables and **fails the build** when neither is present; there is no
   debug-keystore fallback for release. CI may compile an explicitly UNSIGNED AAB with
   `-Paf.allowUnsignedRelease=true` for packaging checks. A signed production AAB remains BLOCKED
   here: this environment has no release keystore.
5. **The native build is a first-class target**: `tools/build_android.sh` (run and verified here) and
   `tools/build-android.ps1` (Windows port, **not** executed — flagged as such in its header).
6. **`tools/verify.sh`** — one command that re-runs every host-side acceptance check, including the
   "no `INTERNET` in any APK" audit, and exits non-zero on failure.
7. **`DEVICE.md`** — the on-device pass, written as an ordered checklist with the exact `adb`
   commands, the expected numbers, and a record sheet to fill in.

## Device verification

There is no Android device and no emulator in this environment (an emulator would need an x86_64
build *and* a GPU-capable host, which this container has neither of). So the honest split is:

* **statically verified here** — api level, permissions, provider config, native packaging, dex
  contents, kernel behaviour, shader compilation, glTF structure;
* **not verified here** — anything that needs a GPU or a touchscreen: actual GLES 3.1 context and
  pipeline creation, frame rate, the budget controller's step-down, thermals, and the exact
  wall-clock of a generation run on a real SoC.

`DEVICE.md` is the pass that closes that gap; the highest-value item on it is the pose-mode check,
because `[12]`'s device equivalent (does `Rest` return the avatar exactly to the build pose?) is the
one thing that was actually broken a moment ago.

## Open work, in order

1. **Device pass** — `DEVICE.md`, then the numbers go into this file.
2. **Rust end-to-end clip check**: `rust/af/tests/engine.rs` still asserts against the deformed
   buffer instead of `vertices_rest()`. The kernel is fine (the C++ harness and `pose_probe` both
   pass); this is a test-side fix in the Rust crate.
3. **Optional polish** — per-strand hair direction for the hair shader (today it derives the strand
   direction from the world, documented as an approximation), lash geometry, and publishing joint
   matrices at rig time so the `.glb` carries `inverseBindMatrices` in a single pass.

## Honest limitations

* **No neural network anywhere.** Prompt and photo "understanding" is classical: a keyword grammar
  for text, colour segmentation + a head-count ratio for photos (`native/src/af_image.h`). An
  on-device diffusion model large enough to be worth shipping would need a GPU delegate that half
  the target devices lack and would make the result non-reproducible; this engine measures instead,
  reports a confidence per estimate, and lets the user correct it in the UI. This is a deliberate
  design decision, documented at the top of `af_image.h`, not a gap.
* **Photo-derived identity is a likeness, not a scan.** Stature comes from the head-to-body ratio
  (scale-free, works from any phone camera); skin tone from median CIE L\*a\*b\*; the garment from
  its segmented colour and metalness. The generated body is the anthropometric template matched to
  those measurements — it is not photogrammetry, and the UI shows the numbers so the user can see
  exactly what was read.
* **The reference render is CPU-side on the host.** Vulkan/GLES verification needs a device: the
  shaders are validated statically (`spirv-val` + a GLSL ES type-check) and the pipelines are built
  from them at runtime, but no GPU frame has been captured in this container.
* **The Rust end-to-end test is currently red** for the reason in *Open work* 2 (test-side assertion,
  not a kernel defect). Everything else green.
* **The build machine is 2 GB.** Release and debug are built one at a time with `--no-daemon`,
  `-Xmx1200m` and `-Dorg.gradle.workers.max=1`; combining them in one invocation kills the daemon.
  A normal machine has no reason to care.

## Layout

```
native/include/afcore.h     the public C ABI (ABI version 1) - the contract everything speaks
native/src/                 kernel: anatomy, SDF field, isosurface, UV, PBR bake, rig, clips, GLB
native/src/render/          the renderer: af_render.h contract, af_vk.cpp, af_gl.cpp, shaders/
native/src/android/         af_jni.cpp - the only translation unit that includes <jni.h>
native/tests/host_tests.cpp 386-check host harness (no device needed)
native/src/af_glb_read.cpp    glTF 2.0 importer (GLB load)
native/src/af_json.cpp        bounded JSON reader for the importer
native/src/af_png_decode.cpp  PNG decoder (inflate) for embedded textures
native/src/af_scene.cpp       scene graph, frustum culling, LOD, lights
native/src/af_profiler.cpp    frame/stage profiler with honest GPU-timing gating
sdk/                        Kotlin SDK: NativeBridge, AvatarEngine, AvatarRenderer, RenderSurfaceView
app/                        Compose UI: input, progress, viewer/editor, export
rust/af-sys, rust/af        Rust bindings over the same C ABI
tools/                      build_host.sh, build_android.sh, build_gradle.sh, verify.sh,
                            bootstrap_toolchain.sh, embed_shaders.py, probes, validators
artifacts/                  built APKs and the SDK AAR
DEVICE.md                   the on-device verification sheet
```

## Commands

```bash
tools/bootstrap_toolchain.sh                  # JDK 17, Gradle 8.9, SDK 34, NDK 26b, Kotlin, Rust
tools/build_host.sh test                      # kernel + 386-check harness (no device, no Android SDK)
tools/verify.sh                               # every host-side check, including the APK audit
tools/build_android.sh                        # arm64 natives -> prebuilt/arm64-v8a
tools/build_gradle.sh :app:assembleDebug -Paf.abIs=arm64-v8a -Paf.prebuiltJni=$PWD/prebuilt
python3 tools/validate_glb.py out.glb         # structural check of an exported model
```

Full instructions and expectations: **BUILD.md**. Device pass: **DEVICE.md**.
