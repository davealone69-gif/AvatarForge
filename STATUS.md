# AvatarForge — status

**What this is.** A standalone Android app that generates, auto-rigs, animates and renders a
photorealistic full-body 3D avatar entirely on the device, from a text prompt, a photo, or a
two-image reference flow (identity + equipment). No server, no network, no game engine.

**Where it stands (2026-10-20).** The current native host harness passes **464/464** checks; the C++
kernel implements **47/47** ABI symbols and the Rust alternative **15/47**. Phase 1–3 source includes
renderer-owned index-only LODs, adaptive atlas selection, per-device GPU profiles, ASTC/ETC2 encoding
and compressed GL/Vulkan upload paths, plus the Kotlin stage graph, thermal feedback, renderer leases,
render-thread picking/blendshapes and optional Android 12+ ADPF hints. Host tests cover the profile policy,
codec payloads, semantic mip chains and async job polling. The GL/Vulkan/JNI translation units pass
host syntax-only checks against downloaded Khronos headers and Android type stubs; host CMake
configuration and scalar codec targets also build. These are **not** Android NDK/GPU runtime tests.
Kotlin changes and JVM tests remain **uncompiled**: Java 11, no Android SDK/NDK or standalone Kotlin
compiler, and Gradle dependency resolution failed on Maven TLS handshakes. The checked-in 0.10.1 APK/AAR
predate this follow-up; Android package rebuild and all device/GPU validation remain outstanding. Phase 4
is deferred. **No physical-device run exists** — see *Device verification*.

The SDK requests a default **1.5 GiB** native generation budget, not a guaranteed fixed allocation;
it may lower the cap to respect measured live RAM minus a **512 MiB headroom** (with a 64 MiB floor).

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

### 2026-10-19 initial culler review (411-check run)

- Fixed the shader-table regression: the host check now includes `forward_plus.comp`, so all **13**
  baked SPIR-V/GLES stages are checked instead of expecting 12. Added `[10b]` host coverage for the
  renderer's avatar culler; the full harness passes **411/411**.
- Added `AvatarCuller`, a CPU adapter from `CameraState`/model bounds into the tested `Scene`
  visibility pass. Vulkan and GLES now skip forward avatar draws only when the conservative sphere is
  outside the frustum; shadow rendering remains independent. `[10b]` covers visible/off-screen,
  missing-model, malformed-bounds and degenerate-FOV fail-open cases. Current engine contracts provide
  only LOD 0, so this does **not** claim GPU LOD rendering or multi-node scene support.
- Fixed the Rust clip test to compare the deformed mesh against `vertices_rest()`, then assert exact
  REST restoration within tolerance and repeat-playback stability. The ignored end-to-end test passed
  on the host (34.9 s pipeline).
- Renamed the Rust cdylib to `libafcore_rust.so`. This prevents Cargo's `LD_LIBRARY_PATH` from loading
  the Rust 15/47-symbol library instead of the C++ `libafcore.so` when running workspace tests. The
  full offline Rust workspace tests now pass (6 wrapper tests + 31 core tests; two expensive
  integration tests remain ignored by default).
- Made the documented shell/CLI entry points executable (`gradlew`, `tools/*.sh`, `tools/*.py`).
- `tools/verify.sh --quick` passes the host/ABI/Rust-parity/GLB gates. The shader source/table check
  sees all 13 stages and matching GLES text, but reports `glslc=NOT FOUND`; `aapt2` is also absent, so
  the Android shader recompilation and APK-permission audit were skipped.
- `cargo fmt --manifest-path rust/Cargo.toml --all -- --check` now correctly targets the virtual
  workspace, but exits non-zero on existing rustfmt differences across the Rust crates. No broad
  reformat was applied; the changed clip-test block was kept rustfmt-compliant.
- **Not verified in that initial review:** Vulkan/GLES translation units and APK/AAR were not rebuilt,
  because this environment has no Android SDK/NDK; no GPU/device pass was possible.

### 2026-10-19 Phase 1–3 follow-up (native run at that point: 421 checks)

- Added `[10c]` and `[10d]` host coverage: `parallelFor` propagates worker exceptions after joining
  and observes cancellation (it also joins already-created workers if thread creation fails);
  `clusterDecimate` now scales cells to target triangle count, preserves material regions, bounds packed
  keys, and rejects malformed indices without mutation. `tools/build_host.sh test` passed **421/421**
  before the Kotlin-only follow-up below.
- Serialized Kotlin engine access with a native mutex; generation children now belong to the lifecycle
  root, cancellation completes even before dispatch, and `release()` delays destruction until jobs and
  renderer leases finish. `RenderSurfaceView` now attaches/releases a lease, serializes render operations,
  performs picking and blendshape updates on its render thread, waits for GPU teardown before the Surface
  is invalidated, and adds optional ADPF frame-work reporting on Android 12+.
- Moved prompt parsing and bitmap/reference preparation off the UI thread. Kotlin image-derived specs now
  mirror the native analyzer's height, skin/hair colour, garment colour, outfit and footwear rules; the
  generation path re-analyzes and returns the native canonical spec after prompt overrides. Added JVM
  tests for resolver parity cases and idempotent renderer-lease release; these tests could not be run.
- Generation now defers under low-memory, low-storage or severe-thermal snapshots and caps the native
  memory budget to measured available RAM minus headroom. The Kotlin render presets were aligned to the
  native low/medium/high tier values.
- **Still blocked/unverified:** these Kotlin/Android edits have not been compiled; this environment has no
  Android SDK/NDK or standalone Kotlin compiler, runs Java 11 rather than the required JDK 17, and the
  Gradle attempt failed during Maven dependency resolution with TLS handshake errors. No GPU/device is
  available. The checked-in 0.10.1 APK/AAR remain the older 2026-10-18 build. GPU LOD index/range drawing
  and ASTC/ETC2 texture compression are still missing.

### 2026-10-20 intermediate Phase 1–3 review (426-check snapshot before latest work)

- Fixed native image-role selection when an earlier input fails analysis: the engine now maps each
  original image index to its compact successful-analysis record. New host test `[0b]` rejects an
  intentionally invalid first image and verifies the explicitly marked identity still drives the spec.
  Kotlin `ImageSpecResolver` now selects the last IDENTITY/CLOTHING role, matching native behavior; a
  fifth JVM test covers duplicate role selection.
- Tightened the single-slot engine/renderer boundary. An engine now permits one SDK-managed renderer;
  generation and other SDK-owned direct native calls fail before mutating or reading state while that
  lease is active (the raw-pointer API remains caller-synchronized). The view's blendshape path uses a
  render-thread-only SDK operation. Surface dimensions are
  cached from `surfaceChanged` so picking no longer reads View dimensions from the renderer thread.
  These Kotlin edits remain source-only pending compilation.
- Re-reviewed Phase 2 LOD rather than wiring the existing helper prematurely. `Mesh::clusterDecimate`
  rebuilds positions/UVs/regions/normals but has no rig-weight or morph-target representation; it cannot
  safely produce a renderable LOD for skinned/blendshaped avatars. The C ABI exposes one opaque-engine
  mesh/range set, and GL/Vulkan draw those original ranges. Safe LOD requires deformation-compatible
  per-LOD vertex/index/submesh data and a renderer-facing accessor (or a renderer-owned equivalent with
  a validated deformation sync path). No GPU LOD is claimed. ASTC/ETC2 likewise remains missing because
  there is no block-compressed encoder/upload payload in the current path. Audit also corrected the
  device-profile KDoc: texture baking is actually fixed at 2048² today, not reduced on LOW tier.
- At this intermediate snapshot, full `build-host/host_tests` passed **426/426**. It validates native
  host code only. Android compilation/tests, GPU behavior and device measurements remained
  blocked/unverified; Phase 4 was deferred.

### 2026-10-20 preceding Phase 2–3 implementation (448-check run, before GPU compression)

- Added `AvatarLodSet`, a renderer-owned, index-only LOD builder. It retains the engine's original
  vertices (including skin weights/morph compatibility), keeps disconnected chart islands and
  submesh ranges separate, and falls back to the original indices on malformed inputs. GL and Vulkan
  build the LOD set on engine attach, upload the combined index stream, select a level using camera
  distance plus the effective quality tier, and draw each submesh using its LOD range. Host tests
  `[10e]` and the generated-avatar portion of `[11]` cover source-vertex preservation, full-source LOD 0,
  per-submesh bounds, and coarse-index reduction. LOD allocation failures clear the partial set and fall
  back to the source stream rather than escaping the C/JNI boundary. These paths are implemented but
  still require Android NDK compilation, GPU validation, and performance/visual review.
- Atlas bake resolution now selects the largest of 2048, 1024, 512, or 256 that fits a conservative
  19-byte-per-texel workspace estimate after the geometry-stage peak estimate. The 1.5 GiB request
  usually leaves 2048 available; tighter budgets can select lower resolutions, and below-minimum
  budgets return a specific budget error. Host chooser assertions passed in the 448-check run.
- Added the process-lifetime `GenerationJobGraph` in the SDK and execute native stages through its
  stable topological order, with cancellation checks between stages; the single native engine remains
  serialized, so this does not claim parallel generation. The native C ABI host test now executes the
  staged chain before `run_all` finalization. Kotlin graph tests are present but unrun.
- Fixed whole-job progress accumulation to add monotone per-stage fraction deltas (not repeatedly add
  every absolute callback), and added a pure Kotlin regression for repeated/decreasing callbacks and
  reset behavior. This test is also unrun pending a working JVM/Android toolchain.
- Added API-29+ thermal-status observation to `RenderSurfaceView`: moderate pressure lowers the
  effective render preset by one tier, severe pressure selects LOW, and recovery restores the user's
  requested preset. Android 12+ ADPF frame-work hints remain optional. Unit tests cover the policy,
  but Android/Kotlin compilation and on-device thermal behavior remain unverified.
- At that point, `./tools/build_host.sh test` passed **448 checks, 0 failures**, and
  `./tools/verify.sh --quick` passed the host/ABI/GLB gates. The run preceded GPU compression/profile
  work; the follow-up status is recorded below. Phase 4 remains deferred.

### 2026-10-20 GPU compression/profile follow-up (464-check full host run)

- Added capability-first `GpuDeviceCaps` and conservative Adreno/Mali/vendor-family profiles. Reported
  texture dimensions, common color/depth sample counts, HDR support, software renderers, and ASTC/ETC2
  format features bound the tier; the profile caps quality, MSAA, shadow size, bloom, LOD distance and
  CPU encoder effort. The active profile and texture-compression status are included in renderer info.
- Vendored Apache-2.0 ASTC Encoder 5.4.0 and Google Etc2Comp. ASTC is built with ABI-specific NEON
  (`arm64-v8a`), SSE2 (`x86_64`) or portable scalar ISA; ETC2/EAC is the fallback. CPU preparation
  creates semantic, alpha-preserving and normal-aware mip chains; ASTC and ETC2 preserve base-colour
  alpha and encode AO in EAC R11. Compression runs asynchronously. GL and Vulkan upload supported
  compressed formats, retain raw RGBA8/R8 fallbacks, update mip/filter state and compressed byte
  accounting. Vulkan replacement waits for idle and handles single-level/mip layout transitions.
- Host group `[10c]` now tests vendor/profile limits, no-compression fallback, ASTC block sizes and
  decode round-trip, odd-dimension mip payloads, ETC2 sRGBA/RGBA/EAC encodings, invalid descriptors and
  async job polling. `bash tools/build_host.sh test` passes **464/464, 0 failures**. Focused
  `--compression-only` also passes **16/16**.
- `bash tools/verify.sh --quick` passes the host quick harness, shader-table check, sample GLB
  validation and C++ ABI conformance (**47 symbols**); Rust and APK permission audit were skipped because
  `cargo` and `aapt2` are unavailable. GL/Vulkan/JNI syntax-only checks pass using downloaded Khronos
  headers and Android type stubs. Host CMake configure plus scalar ASTC and Etc2Comp static targets build.
  These checks are not an Android NDK build, GPU upload test, or device validation. Kotlin changes remain
  uncompiled; APK/AAR are stale; no physical-device test has run. Phase 4 remains deferred.

---

## What the artifacts in `artifacts/` actually contain

The debug APK, release APK, and AAR were rebuilt on 2026-10-18 from the 0.10.1 source with NDK r26b
for arm64-v8a. Static inspection verifies all three native libraries, both normalized customization
assets, engine version 0.10.1, APK alignment/signature, and no `INTERNET` permission. The release APK
uses the fallback debug certificate because no release keystore was supplied. These artifacts **do
not include the 2026-10-19 renderer-culling or Phase 1–3 follow-up changes** and must be rebuilt before
they are treated as representative of the current source. The sample GLB was not regenerated and
remains a prior valid export.

Known compiler warnings still present (all unused-variable/parameter class, none behavioural):
`af_field.cpp:274 backZ`, `af_image.cpp:321 fracToY`, `af_isosurface.cpp:167 field`,
`af_isosurface.cpp:654 a`, `af_rig.cpp:206 isArm`, `af_rig.cpp:414 jawR`, `af_rig.cpp:566 e`.

## Build order

The project is built in four phases, in this order. Each state distinguishes implementation
completeness from host/device evidence — never "probably fine".

| Phase | Scope | State |
|---|---|---|
| **1. Reliable rendering, GLB loading, crash-free lifecycle, profiling** | GLB import, scene graph, conservative culling, frame profiler, hostile-input robustness | Import/culler host coverage passes (`[10b]`, `[14]`, `[15]`); Kotlin adds renderer leases/exclusivity, render-thread work and teardown but is **uncompiled**. Renderer sources received a host syntax-only check; no Android package or device run. **0 % device-verified** |
| **2. Vulkan, LOD, texture compression, device profiles** | Vulkan primary, GLES 3.1 fallback, per-tier profiles, ASTC/ETC2 | Index-only LOD and capability-first device profiles are implemented and host-tested. ASTC/ETC2 encoding, mip generation and async polling pass host tests; compressed GL/Vulkan upload paths pass host syntax checks. Android NDK compilation, GPU upload behavior, visual quality and per-device tuning remain **unverified** |
| **3. ADPF, adaptive quality, multithreaded jobs** | thermal feedback, quality controller, job graph | Native frame-budget quality control and fork-join work are host-tested; SDK graph executes stages in dependency order with cancellation boundaries; API-29+ thermal tier override and Android 12+ ADPF hints are implemented in Kotlin. Kotlin/JVM tests, Android build, and device thermal/headroom validation remain **unverified** |
| **4. GPU-driven rendering, advanced lighting, optional ray tracing** | indexed indirect draws, Vulkan Forward+ tile-light compute, RT where available | **Deferred by current scope; partial, not device-verified**: Vulkan builds CPU-authored indirect commands and dispatches a Forward+ compute pass; lights are synthetic, GLES has no equivalent tile path, and GPU-driven culling/compaction and RT are not started |

Two decisions that shape all four phases:

* **The renderer is custom (Vulkan primary, GLES 3.1 fallback), not Filament.** The brief allowed
  Filament "if using" it, but the standing rule for this project is a renderer written from scratch
  with no pre-existing engine, so Filament is used as a reference for ideas only and never linked.
* **Nothing is device-verified yet.** There is no phone and no GPU in this build environment, so the
  claim this repo makes is *implemented and host-verified*; `tools/device_check.sh` exists so that a
  single command turns that into evidence on real hardware, and `DEVICE.md` §9a defines the numbers a
  phase has to hit.

## 2026-10-19 host verification

| Claim | Evidence |
|---|---|
| Native host harness | `tools/build_host.sh test` → **421 checks, 0 failures**, including `[10b]` culler and `[10c]`/`[10d]` worker/decimator coverage |
| C++ ABI | `python3 tools/abi_conformance.py build-host/libafcore.so --require-complete` → **47/47** |
| Rust workspace | `cargo test --offline` → 6 wrapper tests + 31 core tests pass; two integration tests ignored by default |
| Rust end-to-end animation | explicit ignored `builds_a_watertight_avatar_end_to_end` → pass; compares bind/rest/repeat-playback buffers |
| Rust ABI + prompt parity | `libafcore_rust.so` → 15/47 symbols; parity passes for implemented tables and 72 prompts |
| GLB structure | `python3 tools/validate_glb.py artifacts/avatarforge-sample.glb` → pass |
| Unified quick gate | `tools/verify.sh --quick` → exit 0; quick host sections, ABI manifest/conformance and GLB pass; Cargo not on PATH, `aapt2` audit skipped |
| Shader source/table check | 13 stages and GLES text match; `glslc=NOT FOUND`, so SPIR-V recompilation/`spirv-val` were skipped |
| Kotlin/Android compile | **Blocked before compile**: Gradle 8.9 could not resolve Maven dependencies (TLS handshake failures); Java 11 only and Android SDK/NDK absent |
| Rust formatting | workspace `cargo fmt --all -- --check` runs but reports pre-existing formatting diffs; no broad reformat applied |
| Android renderer package/device | **Not tested** in this environment; checked-in APK/AAR predate the culler and current Kotlin follow-up |

## 2026-10-18 build verification (historical)

| Claim | Evidence |
|---|---|
| Kernel host harness (2026-10-18) | `tools/build_host.sh test` → **405 checks, 0 failures** (historical; later runs reached 411, 421, then 426) |
| GLB import into a live engine | harness `[16]`: the export loads back through the ABI into an empty engine (285 657 vertices / 382 388 triangles, exactly what was generated), regions survive, the file's skeleton is refused (joint count 0, clip playback rejected), a second import is idempotent, and a hostile buffer leaves vertices, triangles and rig untouched |
| GLB round trip | harness `[14]`: the exporter's own 34 MB file imports back — 10 primitives sharing **one** vertex pool (266 797 unique vertices instead of 2 134 376), 71 joints, inverse bind matrices, 52 morph targets, 5 PNGs decoded; 45 hostile inputs (truncation, bit flips, cyclic graphs, out-of-range accessors) rejected cleanly |
| Scene, culling, LOD, profiling | harness `[15]`: transform composition, conservative sphere culling, monotone LOD in distance, deterministic light order, profiler exact against a synthetic clock and reporting **−1** (not 0) for GPU timings the device cannot measure |
| Rust core parity | `tools/abi_conformance.py rust/target/release/libafcore_rust.so --reference build-host/libafcore.so` → 15/47 symbols, all invariants pass, identical clip/blendshape/region tables and **byte-identical prompt analysis over 72 corpus prompts** |
| Prompt grammar parity | `tools/prompt_parity.sh` → **72 prompts, C++ and Rust identical** |
| Posing is stateless and `REST` is exact | harness `[12]` → the walk moves 280 290 of 280 290 vertices (max 0.286 m) and `REST` leaves **0** of them off the bind pose; walk → REST → walk is **bit-identical** to walk alone |
| Android natives build | `tools/build_android.sh` → `libafcore.so` 11 199 560 B, `libafrender.so` 1 846 152 B, `libavatarforge.so` 652 624 B (arm64-v8a, NDK r26b, static STL) |
| Debug APK builds, no warnings | `tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease` → **BUILD SUCCESSFUL in 47 s**, 0 Kotlin warnings |
| Release APK builds and signs | `:app:assembleRelease` → **BUILD SUCCESSFUL in 24 s**, `avatarforge-release-arm64.apk` 6 864 226 B, signed (debug cert — no keystore supplied) |
| No network capability | `aapt2 dump permissions` on both APKs → only `com.avatarforge.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION`; **no `INTERNET`** |
| Export surface intact | `aapt2 dump xmltree` → `FileProvider`, `exported=false`, `grantUriPermissions=true`, authority `com.avatarforge.avatarforge.fileprovider` |
| Natives packaged and stripped | `file` on the APK's `lib/arm64-v8a/*.so` → *stripped*, arm64, "for Android 26" |
| Shader table/source check | Current baked tables and `embed_shaders.py --check` validate **13 stages** (including `forward_plus.comp`) and matching GLES text; `glslc`/`spirv-val` were unavailable for Android shader recompilation |
| Blendshapes all move real vertices | `tools/shape_probe.cpp` → **0 empty, 0 sub-millimetre** across all 52 ARKit channels |
| Topology for every body type | harness `[4/5]`: four build variants, **0 open edges, 0 boundary loops, 0 holes** each |
| Nothing regressed | `tools/verify.sh` → all host-side sections pass |
| Device evidence collector | `tools/device_check.sh` → refuses to report without a device (exit 3, explicit message); with one, writes `artifacts/device-report.txt` |

## Phase 1 implementation notes

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
4. **Scene layer and renderer culling** (`af_scene.{h,cpp}`, `af_avatar_culler.{h,cpp}`): the tested
   node graph supports conservative sphere culling, per-instance LOD selection, cameras and
   deterministic light culling. A PIMPL adapter now feeds the single avatar's camera/model bounds
   into that pass from both Vulkan and GLES forward rendering. Host coverage `[10b]` checks visible,
   off-screen, missing-model and invalid-bounds cases. Renderer-owned index-only LOD ranges have since
   been wired into both draw paths (see the 2026-10-20 latest implementation section); Android
   compilation and visual/device verification are still required.
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

### Other earlier changes

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
   harness grew from 259 to 275 checks at that point (it later reached 405 in the 2026-10-18 build, 411 in the initial 2026-10-19 culler review, 421 in the Phase 1–3 follow-up, and 426 on 2026-10-20).
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
4. **Release builds are signed and reproducible.** `app/build.gradle.kts` signs with
   `keystore.properties` when present and falls back to the debug keystore otherwise; the Gradle
   wrapper (8.9) is checked in; `tools/build_gradle.sh` pins JDK 17 + SDK + heap for this container.
5. **The native build is a first-class target**: `tools/build_android.sh` (run and verified here) and
   `tools/build-android.ps1` (Windows port, **not** executed — flagged as such in its header).
6. **`tools/verify.sh`** — one command that re-runs every host-side acceptance check, including the
   "no `INTERNET` in any APK" audit, and exits non-zero on failure.
7. **`DEVICE.md`** — the on-device pass, written as an ordered checklist with the exact `adb`
   commands, the expected numbers, and a record sheet to fill in.

## Device verification

There is no Android device and no emulator in this environment (an emulator would need an x86_64
build *and* a GPU-capable host, which this container has neither of). So the honest split is:

* **host-verified for current source** — native kernel behavior, ABI, GLB structure, scene tests,
  atlas chooser, index-only LOD data, GPU-profile policy, ASTC/ETC2 CPU encoding and async compression;
  scalar ASTC/Etc2Comp CMake targets also build on the host;
* **syntax-checked only** — GL/Vulkan/JNI translation units against downloaded Khronos/JDK headers and
  Android type stubs; this is not an Android NDK build or a runtime GPU upload check;
* **statically verified on 2026-10-18 artifacts only** — API level, permissions, provider config,
  native packaging and dex contents;
* **not verified for the current source** — Kotlin/Android compilation and JVM tests, Android
  package creation, actual Vulkan/GLES pipeline creation, GPU LOD visuals/performance, ADPF/thermal
  behavior, frame rate, or generation time on a SoC.

`DEVICE.md` is the pass that closes that gap; the highest-value item on it is the pose-mode check,
because `[12]`'s device equivalent (does `Rest` return the avatar exactly to the build pose?) is the
one thing that was actually broken a moment ago.

## Open work, in order

1. **Compile and test the Kotlin follow-up.** Install/use the pinned JDK 17, SDK 34, NDK 26b and
   Kotlin toolchain; run SDK/JVM tests and compile app + SDK, fix any source/API issues, then refresh
   APK/AAR. The current workspace has not passed this step.
2. **Device/GPU pass** — follow `DEVICE.md`; validate lifecycle cancellation/rotation, resource deferral,
   ADPF and thermal adaptation, culling/shadow behavior and both GPU backends where available. Record
   evidence rather than treating the 464 host checks as device validation.
3. **Validate renderer-owned LOD on hardware.** Index-only LOD streams are implemented and integrated
   in GL/Vulkan source, but need real Android compilation, visual checks under skinning/blendshapes,
   GPU timing and attach-time performance review. Multi-node scene traversal is still not claimed.
4. **Validate ASTC/ETC2 uploads and per-device GPU profiles.** Encoding, capability checks, compressed
   uploads, raw fallback and profile caps are implemented; they need an Android NDK build, real Vulkan/
   GLES format-query tests, device visual review, memory/performance measurement and model-family tuning.
5. **Verify Phase 3 policy.** The process-lifetime dependency graph and API-29+ thermal quality policy
   have source/JVM tests, but need Kotlin compilation, tests, lifecycle review and physical-device
   thermal/headroom validation. Generation remains serialized around one native engine handle.
6. **Optional polish** — per-strand hair direction for the hair shader (today it derives the strand
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
* **Renderer integration remains unverified at runtime.** GL/Vulkan translation units pass a
  host-only syntax check against downloaded system headers and an opaque `ANativeWindow` stub, but
  they were not built with the Android NDK. No GPU frame has been captured; shader tables are
  host-checked, but `glslc`/`spirv-val` were unavailable in this review.
* **Rust is still a partial ABI implementation.** Its workspace and end-to-end tests pass, but only
  15/47 C ABI symbols are implemented; this is not a substitute for the C++ engine.
* **The build machine is 2 GB.** Release and debug are built one at a time with `--no-daemon`,
  `-Xmx1200m` and `-Dorg.gradle.workers.max=1`; combining them in one invocation kills the daemon.
  A normal machine has no reason to care.

## Layout

```
native/include/afcore.h     the public C ABI (ABI version 1) - the contract everything speaks
native/src/                 kernel: anatomy, SDF field, isosurface, UV, PBR bake, rig, clips, GLB
native/src/render/          the renderer: af_render.h contract, af_vk.cpp, af_gl.cpp, shaders/
native/src/android/         af_jni.cpp - the only translation unit that includes <jni.h>
native/tests/host_tests.cpp 464-check host harness (no device needed)
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
tools/build_host.sh test                      # kernel + 464-check harness (no device, no Android SDK)
tools/verify.sh                               # every host-side check, including the APK audit
tools/build_android.sh                        # arm64 natives -> prebuilt/arm64-v8a
tools/build_gradle.sh :app:assembleDebug -Paf.abIs=arm64-v8a -Paf.prebuiltJni=$PWD/prebuilt
python3 tools/validate_glb.py out.glb         # structural check of an exported model
```

Full instructions and expectations: **BUILD.md**. Device pass: **DEVICE.md**.
