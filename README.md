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
