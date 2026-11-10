# AvatarForge

A standalone Android 3D engine, avatar-generation SDK and test lab. It contains a deterministic
procedural avatar generator, a separate GLB import/export pipeline, native rigging and animation,
and custom rendering backends. These are distinct capabilities: successful GLB display does not
prove generated avatars have the same visual quality, and generator host tests do not prove Android
GPU rendering.

**Generation status:** the procedural pipeline is implemented and has host-side geometry, rig,
blendshape and GLB tests. Its visual likeness, facial detail, anatomy, proportions, material realism
and quality against real reference models still need measured comparison. Photo analysis currently
feeds structured measurements/features into the generator; it is not a neural identity-reconstruction
system. Do not describe it as photorealistic reconstruction until reference-image comparisons prove it.

No server. No network of any kind. No game engine. The app must work in airplane mode, and an
`INTERNET` permission in the manifest is treated as a build failure.

```
text prompt ─┐
photo ───────┼──▶ anthropometry ─▶ SDF body field ─▶ isosurface ─▶ UV atlas ─▶ PBR bake
2 images ────┘         │                                              │
                       └──▶ skeleton ─▶ skin weights ─▶ 52 ARKit ─▶ 15 clips
                                                          blendshapes
                                                                      │
                              Vulkan / GLES 3.1 renderer ◀────────────┤
                              GLB export / GLB import ◀───────────────┘
```

## Status, in one paragraph

The kernel, renderer, SDK and app have build and host-test evidence recorded in STATUS.md. That
status file currently contains conflicting host-check totals (386 and 405), so the exact current
count is **UNVERIFIED until a fresh harness run reconciles it**. The C++ kernel implements 47/47
ABI symbols; the Rust core implements 15/47 with parity on the symbols it covers. Physical-device
rendering and generation quality are not established by those host checks. DEVICE.md is the ordered
hardware test sheet, and tools/device_check.sh collects evidence when a device is attached.


## Reference-model benchmark: mylove.glb

The user's supplied reference file is recorded as a **static display/geometry benchmark**, not as a
ready-made generator input. Its inspected metadata is:

| Property | Reference model |
|---|---:|
| File size | 9,287,272 bytes (about 9.29 MB) |
| glTF | GLB 2.0, valid declared file length |
| Scene / nodes | 1 scene / 2 nodes |
| Meshes / materials | 1 mesh / 1 material |
| Geometry | 163,756 vertices / 144,073 triangles |
| Textures | 2 images/textures, both 2048 × 2048 WebP |
| Vertex attributes | positions, normals and UVs present |
| Skeleton / animations | none / 0 animations |

The generator's existing host records describe different test fixtures: one generated model was
reported at 285,657 vertices / 382,388 triangles; a separate 34 MB GLB round-trip fixture contains
266,797 shared-pool vertices, 10 primitives, 71 joints, 52 morph targets and 5 PNG images. These are
not the same asset and must not be collapsed into one measurement.

**What the comparison says:** the generator already creates more raw geometry than the reference
model in the cited host fixture, so simply increasing polygon count is not the next quality fix. The
reference is static and has no rig or morph targets; it cannot validate generated skinning or facial
animation. The useful next comparisons are silhouette/proportions, face topology/detail, normals,
UV layout, texture quality, material response and measured body dimensions.

**Not safely transferable yet:** the available GLB inspection record contains no trustworthy tape
measurements or af_spec values (height, chest, waist, hip, shoulder width, inseam, etc.). Vertex
count and triangle count are not body measurements. Do not feed guessed anthropometry into the
procedural generator. Extract the model's position bounds and metric scale first; then derive
cross-section/landmark measurements where topology permits, and record confidence for every value.

Visual comparison inputs supplied on 2026-11-10 are in the user's Dropbox /Model folder:
generated_avatar_diagnostic.png and two Screenshot_20261110_*.jpg captures. A measured side-by-side
visual review is still **PENDING**; the image files are not treated as numerical geometry evidence.

See docs/REFERENCE_MODEL_COMPARISON.md for the detailed feature matrix and next measurement steps.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/afcore.h` | **The shared C ABI** — the single contract every native implementation answers to |
| `native/src/` | C++17 engine kernel: anatomy → field → isosurface → UV → bake → rig → blendshapes → clips → GLB |
| `native/src/render/` | Custom Vulkan (primary) and OpenGL ES 3.1 (fallback) renderers, written from scratch |
| `native/src/android/` | JNI bridge |
| `rust/af-core/` | Rust implementation of the same ABI — engine logic, anatomy, prompt grammar, tables |
| `rust/af-sys/`, `rust/af/` | Raw FFI bindings and the safe Rust wrapper |
| `app/` | Jetpack Compose UI (input → progress → viewer/editor → export) |
| `sdk/` | Documented Kotlin SDK (`AvatarEngine.health/capabilities/generateFromText/generateFromImage/…`) |
| `abi/afcore.abi.json` | Generated ABI manifest — sizes, offsets, enum values, symbol list |
| `tools/` | Build scripts and every verification gate (see below) |
| `engine/` | Renderer-side assets/shaders support |
| `STATUS.md`, `BUILD.md`, `DEVICE.md` | Where it stands · how to build · how to verify on hardware |

## Build

```bash
tools/bootstrap_toolchain.sh          # SDK + NDK + CMake + Gradle + JDK 17 + Rust (once)

tools/build_host.sh test              # kernel + 386-check host harness (no device, no Android SDK)
tools/build_android.sh                # arm64 native libraries into prebuilt/
tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease
```

Full details, including the 2 GB container recipe and JDK/SDK paths, are in **`BUILD.md`**.

## Verification

Every one of these is a gate that fails loudly; none of them needs a device.

```bash
tools/verify.sh                       # everything below, in one run
tools/build_host.sh test              # 386 checks: geometry, rig, bake, ABI, GLB round trip, scene
python3 tools/abi_manifest.py --check # the checked-in manifest still matches afcore.h
python3 tools/abi_conformance.py build-host/libafcore.so --require-complete
python3 tools/abi_conformance.py rust/target/release/libafcore.so --reference build-host/libafcore.so
tools/prompt_parity.sh                # C++ and Rust prompt grammars, diffed over a 72-prompt corpus
python3 tools/validate_glb.py <file>  # exported GLB structure + accessor bounds
```

On hardware:

```bash
tools/device_check.sh --apk artifacts/avatarforge-debug-arm64.apk --soak 120
```

## Design rules this codebase holds itself to

* **Airplane mode is the normal case.** No HTTP, sockets, telemetry, or remote model fetches. Models
  and clips ship in the APK.
* **No fakes.** No placeholder generation, no disabled buttons, no feature that reports success
  without doing the work. When something is unsupported the code says so with a specific reason —
  e.g. a GLB import refuses to half-adopt a foreign skeleton and reports `joint_count == 0` rather
  than looking rigged and not animating.
* **A missing measurement is never a zero.** The frame profiler reports `-1` for GPU timings on
  devices that cannot measure them instead of a plausible-looking `0.0`.
* **The custom renderer stays custom.** Vulkan is the primary backend and OpenGL ES 3.1 the
  fallback; no third-party engine is linked.
* **Determinism.** The same spec produces byte-identical geometry, and that is asserted, not assumed.

## Licence

Not yet chosen. Add a `LICENSE` before publishing this repository.


## Download and install the Android APK

**Fastest route:** open the [Android APK workflow](https://github.com/davealone69-gif/AvatarForge/actions/workflows/android-apk.yml) and open the latest successful run. Under **Artifacts**, download `AvatarForge-Android-<commit>` and extract the ZIP. Install `AvatarForge-debug-arm64.apk` for testing on a modern ARM64 Android phone.

For a versioned, easy-to-find download, open [GitHub Releases](https://github.com/davealone69-gif/AvatarForge/releases). Pushing a version tag such as `v0.10.0` runs the same build and attaches the debug APK, release APK, and SDK AAR to that release.

The release APK currently uses the repository's fallback debug signing certificate unless `keystore.properties` is supplied to a local build. It is suitable for testing, not a production signing identity. Never commit a release keystore or its passwords.

Install from the phone: download the APK, open it in Downloads, and allow installation from that source if Android asks. The APK is ARM64 and targets Android 8.0 (API 26) or newer. AvatarForge is intended to work offline and its APK must not request the `INTERNET` permission.


## Renderer recovery retest (2026-10-21)

A separate recovery build is available for retesting the reported black-viewer / 0 FPS failure. Its physical-device result is still **UNVERIFIED**. See [renderer recovery notes and checksums](docs/RENDERER_RECOVERY_2026-10-21.md) for the bundle identity, exact checksums, download source, and device acceptance checklist. The APK/AAR binaries are not GitHub release assets yet; the notes link to the original Dropbox bundle.


## GLB measurement pipeline: implementation plan (2026-11-10)

This work establishes a reproducible measurement and controlled-edit loop before any AI generator
is connected. It is deliberately isolated from the production procedural generator and renderer.

### Phase 0 — Preserve the source asset

- Identify the exact diagnostic seed GLB and verify its Dropbox metadata before use.
- Create a separate, immutable backup copy before parsing or modifying the asset.
- Record source path, byte size, Dropbox content hash when available, and a locally computed SHA-256.
- Never overwrite the source. All generated files go to a separate output directory.
- If the named `diagnostic-seed.glb` is unavailable, stop and report the missing input; do not silently substitute another model.

### Current execution ledger (2026-11-10)

- **Branch:** `work/glb-measurement-pipeline`; based on current `main` with 0 commits behind and 19 commits ahead at the last compare.
- **Review:** Draft PR [#24](https://github.com/davealone69-gif/AvatarForge/pull/24) is open and unmerged; it is a review/CI gate, not a release claim.
- **Extractor source:** PARTIAL. `tools/glb_measurements.py` validates GLB v2 container lengths, embedded-buffer bounds, accessor offsets/alignment, POSITION format, index ranges and primitive topology. It limits geometry traversal to the active scene and deliberately withholds world bounds/height for skinned meshes until deformation is evaluated.
- **Regression tests:** PARTIAL. `tools/tests/test_glb_measurements.py` contains synthetic GLB tests for translated bounds, triangle modes, indexed geometry, invalid indices, invalid headers, units, active scenes and skinned-mesh exclusion.
- **CI definition:** REAL + VERIFIED as a committed workflow file at `.github/workflows/glb-measurements.yml`; execution result is UNVERIFIED because no successful run/check evidence has been retrieved.
- **Extractor execution:** UNVERIFIED. The tests have not been executed in this environment, and no fixture output has been independently checked yet.
- **Seed asset:** BLOCKED. Dropbox search found `/mylove.glb` (9,287,272 bytes; Dropbox content hash `388a94e7495e809577356ba6cf9890a13fe38f8cd4c63ba3228109d19a24b398`) but did not find `diagnostic-seed.glb`. No substitute was used and no backup was created.
- **5% dummy edit / round-trip:** MISSING. This cannot be implemented safely until the exact seed is identified, its skeleton/landmarks are validated, and the extractor has passed its tests.
- **Donor screening:** PARTIAL. Exact source files and immutable revisions for Grok-Girls, 3DDD, 3d-models and New-app are recorded below. Build, runtime, license and phone evidence remain unverified.

### Phase 1 — Build the read-only GLB extractor

- Parse the GLB v2 header and JSON/BIN chunks with strict length, alignment, bounds and accessor validation.
- Resolve node hierarchy and local-to-world transforms, including matrix and TRS forms.
- Report world-space mesh bounds, dimensions, vertex/triangle counts and coordinate-axis convention.
- Report whether metric units are actually known. GLB coordinates are not assumed to be metres without external scale evidence.
- If skins exist, report joint names, hierarchy, inverse-bind-matrix availability and world-space joint positions.
- Distinguish skeletal joint positions from geometric landmarks; do not invent missing anatomy.
- Emit deterministic JSON with source identity, measurements, confidence/availability flags and parser warnings.

### Phase 2 — Controlled dummy edit

- Apply one explicit edit: increase measured leg length by 5%.
- Only run the edit when the source has a usable skeleton and a validated leg-joint chain, or when an explicitly selected geometric method is supported.
- Preserve unrelated geometry, skin weights, materials, textures, node hierarchy and GLB validity.
- Write a new GLB; never modify the seed or its backup.
- If the required joints/landmarks cannot be identified safely, fail closed with a specific reason rather than guessing.

### Phase 3 — Round-trip verification

- Re-run the extractor on the edited GLB.
- Compare requested and observed changes, including absolute and percentage deltas.
- Assert the leg-length delta is 5% within a documented tolerance and unrelated dimensions remain unchanged within tolerance.
- Validate GLB structure and record output SHA-256, tool version, parameters and pass/fail result.
- Include automated fixtures for malformed headers, invalid accessors, transforms, skinned and unskinned assets.

### Phase 4 — Visual and renderer diagnostics

- Keep visual inspection separate from numerical measurement.
- Record the reported blue-texture glitch and investigate material/texture assignment, depth overlap and Z-fighting independently.
- Do not label overlapping surfaces as Z-fighting without depth/geometry evidence.
- Preserve the known working GLB display path; no renderer or production-generator changes are part of this measurement task.

### Acceptance gates and status

- **Backup:** REAL + VERIFIED only after a distinct backup exists and its content hash matches the source.
- **Extractor:** REAL + VERIFIED only after tests pass and output is independently checked against known fixture values.
- **Dummy edit:** REAL + VERIFIED only after the generated GLB re-imports and the requested measurement delta is observed.
- **Visual comparison:** UNVERIFIED until the source and output are inspected under matched camera, scale and lighting.
- **AI integration:** explicitly out of scope until the deterministic measurement/edit/verification loop is closed.

The initial Dropbox search did not locate a file named `diagnostic-seed.glb`. The extractor must not be run against a substitute without recording and approving the exact source asset.


### Phase 5 — Reusable components in the other 3D repositories (read-only inventory)

The measurement loop must not overlook existing work elsewhere in the user's repository collection.
Before reimplementing a capability, screen the relevant repositories and record exact reusable
components, their build/runtime evidence, dependencies, license, Android suitability and integration
cost. This is an inventory and evaluation task, not permission to alter or merge donor repositories.

Known repository roles and candidates:

| Repository | Current role / candidate parts | Handling |
|---|---|---|
| **3D-Mega-Hub** | Sole integrated Android application and integration target; proven GLB display path for the user's tested fixture/build | Preserve the working path. Integration target only. |
| **AvatarForge** | Standalone native engine, SDK, procedural generator and test lab; current home of this measurement-extractor work | Keep standalone. Do not turn it into the Hub app. |
| **3DDD / 3Double D** | Separate Filament + glTF/GLB rendering stack; candidate renderer/import components | Inspect and benchmark independently. No assumed merge. |
| **3D4ME** | Avatar editor and geometry work | Inspect for reusable editor, geometry and measurement logic. |
| **Morph-Lab** | 3D game / character work | Inspect for reusable character, mesh and animation components. |
| **Three.js / Expo GL editor** | Browser/mobile 3D editor work | Inspect asset editing, transform and GLB handling. |
| **Grok-Girls renderer** | Custom GLES high-definition renderer work | Inspect renderer/material/shader techniques and device evidence. |
| **Avatar Studio / character-studio projects** | Additional avatar authoring and character tooling | Identify exact repository names and inspect before considering reuse. |

This list is a starting register, not a claim that every item has been freshly inspected or is
buildable. First enumerate the user's full GitHub repository collection, including forks and
partially started projects, then classify each candidate. Do not rely on repository names or README
claims as proof that a component works.

For each candidate component, record: repository and immutable commit SHA; source path and API;
license/provenance; dependencies; target platforms; build/test/runtime evidence; GLB/glTF version
and feature coverage; coordinate/unit conventions; rig/skin/morph/animation support; renderer and
material constraints; and the smallest isolated reuse experiment.

### Fresh source-level donor screening (2026-11-10)

The following candidate files were located and fetched from immutable source revisions. This confirms
that the files exist at those revisions; it does **not** establish that they build, run correctly,
are licensed for reuse, or work on the target Samsung device.

| Donor | Inspected source at exact revision | Candidate capability | Current evidence / next gate |
|---|---|---|---|
| **Grok-Girls** | [GltfAvatarLoader.kt](https://github.com/davealone69-gif/Grok-Girls/blob/fd638fde15ab8dae51bfeec8da320590128d1e49/native/app/src/main/java/com/aura/avatarstudio/renderer/GltfAvatarLoader.kt) (blob `33fba04869dd1b4b4ffc678d3d5e708ab6fe10a5`); [native README](https://github.com/davealone69-gif/Grok-Girls/blob/fd638fde15ab8dae51bfeec8da320590128d1e49/native/README.md) | Android GLB parser, PBR material data, skin joints/weights, morph targets; native GLES 3.0 renderer and animation described by its README | **SOURCE INSPECTED; BUILD/RUNTIME/LICENCE UNVERIFIED.** Inspect accessor edge cases, image decoding, shader limits and GL-thread ownership; run fixture parity tests before any reuse. |
| **3DDD** | [GltfDocument.kt](https://github.com/davealone69-gif/3DDD/blob/186e54f05847d5e73ac40c9477a18d18108569dc/app/src/main/java/com/threedd/studio/data/gltf/GltfDocument.kt) (blob `1b45a9a593346bf3d83011f6982504135cb97d66`); [RuntimePipelineTest.kt](https://github.com/davealone69-gif/3DDD/blob/186e54f05847d5e73ac40c9477a18d18108569dc/app/src/androidTest/java/com/threedd/studio/RuntimePipelineTest.kt) (blob `e7205b07d03ab6199bcc19e5358ad03efa707e32`); [README](https://github.com/davealone69-gif/3DDD/blob/186e54f05847d5e73ac40c9477a18d18108569dc/README.md) | Filament renderer, glTF/GLB parsing, node transforms, skinning, animation, morphs, texture decode and runtime tests | **SOURCE + TESTS INSPECTED; CURRENT CI/DEVICE/LICENCE UNVERIFIED.** Evaluate as an isolated renderer/import reference; verify native library loading, backend selection, teardown and target-device compatibility. |
| **3d-models** | [SkeletonMatrices.kt](https://github.com/davealone69-gif/3d-models/blob/d0ba624ef7ce9174b5425476518c4662d340a377/app/src/main/java/com/aura/avatarstudio/renderer/SkeletonMatrices.kt) (blob `703bf5657f8847abe13703ed7b66ae6a3186701d`); [GltfTextures.kt](https://github.com/davealone69-gif/3d-models/blob/d0ba624ef7ce9174b5425476518c4662d340a377/app/src/main/java/com/aura/avatarstudio/renderer/GltfTextures.kt) (blob `5b5ac38661e0fe5637646172f3a21c2275584828`) | Android GLES skeleton matrix calculation and glTF texture/sampler handling | **SOURCE INSPECTED; BUILD/RUNTIME/LICENCE UNVERIFIED.** Compare matrix conventions and texture orientation against reference fixtures. |
| **New-app** | [FilamentRenderer.kt](https://github.com/davealone69-gif/New-app/blob/231c37ae898b36bb2f5da492bcf98c3b4f9720bc/app/src/main/java/com/threedoubled/app/renderer/FilamentRenderer.kt) (blob `10f1b9c0e8b4f9ba6376297089193c8b9c8eb0ec`) | Separate Filament renderer / viewport lifecycle | **SOURCE INSPECTED; BUILD/RUNTIME/LICENCE UNVERIFIED.** Compare engine singleton, GL thread, surface lifecycle and destroy order; do not assume it solves the Hub's native library loading failure. |

| **3D4ME** | [README](https://github.com/davealone69-gif/3d4me/blob/main/README.md) (blob `4451dc4e0058acd7e10be43831a437da9e1262ef`) | Android-first Three.js editor; editable mesh assembly, PBR values, GLB export, viewport capture and project backup/restore | **README INSPECTED; IMPLEMENTATION/BUILD/RUNTIME/LICENCE UNVERIFIED.** Strong candidate for editor workflow and geometry/export comparison. No root `LICENSE` file was found at the checked path. |
| **Aura-Studio-Avatar** | [AvatarEngine.kt](https://github.com/davealone69-gif/Aura-Studio-Avatar/blob/97a66ec874f53b68e2b8ee140097fff755a81341/app/src/main/java/com/aura/studio/engine/AvatarEngine.kt); [ARCHITECTURE.md](https://github.com/davealone69-gif/Aura-Studio-Avatar/blob/97a66ec874f53b68e2b8ee140097fff755a81341/ARCHITECTURE.md) | Android avatar/room/state/animation orchestration and local AI service integration | **SOURCE INSPECTED; 3D GEOMETRY/RENDERER/BUILD/RUNTIME/LICENCE UNVERIFIED.** Treat as product workflow/state reference, not a proven renderer. No root `LICENSE` file was found at the checked path. |
| **Morph-Lab** | [README](https://github.com/davealone69-gif/Morph-Lab/blob/main/README.md) (blob `5d7071b4a849861e6b3252209984a2162d2ab9d9`) | Browser-playable 3D avatar RPG with character lab, world/quest and progression systems | **README INSPECTED; SOURCE/BUILD/RUNTIME/LICENCE UNVERIFIED.** Potential character presentation and animation workflow reference; not an Android renderer donor without a separate technical assessment. No root `LICENSE` file was found at the checked path. |
| **Hunyuan3D-2** | [README](https://github.com/davealone69-gif/Hunyuan3D-2/blob/main/README.md) (blob `91de822eee09281220e8608757f777f7548a083a`); [LICENSE](https://github.com/davealone69-gif/Hunyuan3D-2/blob/main/LICENSE) (blob `a37e08f3a2967aa04fd6b6b514ba9b4a75c836c2`) | AI 3D asset generation/model pipeline, potentially useful as an offline/host-side asset-generation reference | **REPOSITORY DOCS + SPECIAL LICENCE INSPECTED; EXECUTION/ANDROID/RUNTIME UNVERIFIED.** Tencent Hunyuan 3D 2.0 Community License applies; do not copy weights/code or integrate until the actual use, distribution and territory terms are reviewed. Not a phone-native renderer candidate. |

The connected GitHub owner inventory returned 100 repositories on the first page and no second page.
This is the visible connected-account inventory, not proof that every fork or external repository
has been covered. Search also surfaced related projects such as `3d-models`, `Grok-Girls`,
`New-app`, `3DDD`, `3d4me`, `Morph-Lab`, `Aura-Studio`, `Aura-Studio-Avatar`,
`AuraAvatarStudio`, `3D-Mega-Hub` and `AvatarForge`. Continue screening the full inventory
before calling the donor audit complete.

**Hard boundary:** no donor branch edits, cherry-picks, merges, code copying or licensing assumptions
are authorized by this inventory. Keep donor repositories intact. Any selected component must first
pass isolated tests and an explicit compatibility review; integration changes belong in
3D-Mega-Hub only after the user authorizes that integration. AvatarForge remains a standalone
engine/test lab.
