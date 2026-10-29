# AvatarForge

A standalone Android app that **generates, auto-rigs, animates and renders a photorealistic
full-body 3D avatar entirely on the device** — from a text prompt, a photo, or a two-image
multi-reference flow.

No server. No network of any kind. No game engine. The app must work in airplane mode, and an
`INTERNET` permission in the manifest is treated as a build failure.

```
text prompt ─┐
photo ───────┼──▶ anthropometry ─▶ SDF body field ─▶ isosurface ─▶ UV atlas ─▶ PBR bake
2 images ────┘         │                                              │
                       └──▶ skeleton ─▶ skin weights ─▶ 52 ARKit ─▶ 15 clips
                                                          blendshapes
                                                                      │
                              GLES 3.1 / opt-in native Vulkan ◀──┤
                              GLB export / GLB import ◀───────────────┘
```

## Status, in one paragraph

The default renderer is **GLES 3.1**; an **opt-in native Vulkan** path has been restored separately,
with GLES fallback on Vulkan startup/frame failure. The UV-atlas overflow and clip skinning hot path
have focused host regressions, but the Vulkan restoration is **not yet verified on a Samsung GPU**.
Camera movement was user-confirmed earlier (exact build SHA not supplied). CI builds exact-head
Android artifacts; visible pixels, sustained performance/thermal behaviour and production signing
remain unverified or BLOCKED. See `docs/VULKAN_RESTORE_2026-10-30.md` and `DEVICE.md`.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/afcore.h` | **The shared C ABI** — the single contract every native implementation answers to |
| `native/src/` | C++17 engine kernel: anatomy → field → isosurface → UV → bake → rig → blendshapes → clips → GLB |
| `native/src/render/` | Separate native GLES 3.1 and experimental Vulkan backends, no game engine |
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

tools/build_host.sh test              # kernel host harness (no device, no Android SDK)
bash tools/test_model_host.sh         # focused hand/garment detail regressions
tools/build_android.sh                # arm64 native libraries into prebuilt/
tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease
```

Full details, including the 2 GB container recipe and JDK/SDK paths, are in **`BUILD.md`**.

## Verification

Every one of these is a gate that fails loudly; none of them needs a device.

```bash
tools/verify.sh                       # everything below, in one run
tools/build_host.sh test              # kernel checks: geometry, rig, bake, ABI, GLB round trip, scene
bash tools/test_renderer_host.sh      # GLES fault injection + Vulkan selector double (no GPU)
python3 tools/test_renderer_artifacts.py # dual API ELF/package guard tests
python3 tools/embed_vulkan_shaders.py --check # requires NDK glslc; validates native SPIR-V
python3 tools/abi_manifest.py --check # the checked-in manifest still matches afcore.h
python3 tools/abi_conformance.py build-host/libafcore.so --require-complete
python3 tools/abi_conformance.py rust/target/release/libafcore.so --reference build-host/libafcore.so
tools/prompt_parity.sh                # C++ and Rust prompt grammars, diffed over a 72-prompt corpus
python3 tools/validate_glb.py <file>  # exported GLB structure + accessor bounds
```

On hardware:

```bash
tools/device_check.sh --apk AvatarForge-debug-arm64.apk --soak 120
```

## Stability self-improvement path (no AI)

AvatarForge has a bounded, offline renderer-stability loop: diagnostics -> safe fallback -> private
fault journal -> frame-submission probe -> exportable regression report. It does not use AI, upload
data, or rewrite code. Open **MENU -> Self diagnostics** to copy/export the report, run **Safe
recovery**, or clear the journal. See [docs/SELF_IMPROVEMENT_PATH.md](docs/SELF_IMPROVEMENT_PATH.md).
The probe proves sustained non-empty frame submission, **not** visible pixels; device visibility still
requires human confirmation.

```bash
./gradlew :sdk:testDebugUnitTest      # recovery policy/probe/journal JVM regressions
bash tools/test_renderer_host.sh      # renderer fault-injection regressions
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
* **The custom renderer stays custom.** OpenGL ES 3.1 is the only supported backend; no
  third-party engine and no alternate graphics API are linked.
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
