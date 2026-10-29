# AvatarForge

A standalone Android app that builds, rigs, animates and renders a customizable full-body 3D avatar
on-device from a text prompt and optional image references. Prompt and photo analysis are classical
and deterministic; the app does **not** perform photogrammetry or neural identity reconstruction, and
does not promise photorealism or exact likeness.

No server. No network of any kind. No game engine. The app is designed to work offline, and an
`INTERNET` permission in the APK is treated as a build failure.

```
text prompt ─┐
photo ───────┼──▶ anthropometry ─▶ SDF body field ─▶ isosurface ─▶ UV atlas ─▶ PBR bake
2 images ────┘         │                                              │
                       └──▶ skeleton ─▶ skin weights ─▶ 52 ARKit ─▶ 15 clips
                                                          blendshapes
                                                                      │
                              OpenGL ES 3.1 renderer ◀────────────┤
                              GLB export / GLB import ◀───────────────┘
```

## Status, in one paragraph

Phase 1–3 source passes **464 native host checks** and **20 Kotlin/JVM tests**. The C++ engine exports
47/47 public ABI symbols; the Rust alternative is partial at 15/47 and passes parity over its covered
surface. The pinned Android toolchain has built the arm64-v8a native libraries, debug and release APKs,
and release SDK AAR; static gates pass for API 26/34, no `INTERNET`, native/JNI symbols, alignment and
signatures. The app's **☰ MENU → Live device report** refreshes thermal status, RAM, storage and CPU
resources every second without USB/ADB or network; generation is allowed through Severe (3) and
blocked/cooperatively cancelled at Critical (4)+. Android 26–28 explicitly reports thermal telemetry
as unavailable. **This is build/host evidence, not physical-device acceptance.** The user's USB plug is not working and no Android device is connected, so runtime
rendering, offline generation, GPU uploads, performance, memory, thermal and lifecycle behavior remain
unverified; Phase 4 remains deferred. See `STATUS.md` and `DEVICE.md`.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/afcore.h` | **The shared C ABI** — the single contract every native implementation answers to |
| `native/src/` | C++17 engine kernel: anatomy → field → isosurface → UV → bake → rig → blendshapes → clips → GLB |
| `native/src/render/` | Custom OpenGL ES 3.1 renderer, GPU profiles and compressed atlas uploads |
| `native/third_party/` | Vendored Apache-2.0 ASTC Encoder and Etc2Comp codecs |
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

tools/build_host.sh test              # kernel + 464-check host harness (no device, no Android SDK)
tools/build_android.sh                # arm64 native libraries into prebuilt/
tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease
```

Full details, including the 2 GB container recipe and JDK/SDK paths, are in **`BUILD.md`**.

## Verification

Every one of these is a gate that fails loudly; none of them needs a device.

```bash
tools/verify.sh                       # everything below, in one run
tools/build_host.sh test              # 464 checks: geometry, render profiles/compression, ABI, GLB, LOD + stages
python3 tools/abi_manifest.py --check # the checked-in manifest still matches afcore.h
python3 tools/abi_conformance.py build-host/libafcore.so --require-complete
python3 tools/abi_conformance.py rust/target/release/libafcore_rust.so --reference build-host/libafcore.so
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
* **The custom renderer stays custom.** OpenGL ES 3.1 is the only shipped graphics backend; no Vulkan renderer or third-party engine is linked.
* **Determinism.** The same spec produces byte-identical geometry, and that is asserted, not assumed.

## Licence

Not yet chosen. Add a `LICENSE` before publishing this repository.


## Download and install the Android APK

**Fastest route:** open the [Android APK workflow](https://github.com/davealone69-gif/AvatarForge/actions/workflows/android-apk.yml) and open the latest successful run. Under **Artifacts**, download `AvatarForge-Android-<commit>` and extract the ZIP. Install `AvatarForge-debug-arm64.apk` for testing on a modern ARM64 Android phone.

For a versioned, easy-to-find download, open [GitHub Releases](https://github.com/davealone69-gif/AvatarForge/releases). Pushing a version tag such as `v0.10.0` runs the same build and attaches the debug APK, release APK, and SDK AAR to that release.

The release APK currently uses the repository's fallback debug signing certificate unless `keystore.properties` is supplied to a local build. It is suitable for testing, not a production signing identity. Never commit a release keystore or its passwords.

Install from the phone: download the APK, open it in Downloads, and allow installation from that source if Android asks. The APK is ARM64 and targets Android 8.0 (API 26) or newer. AvatarForge is intended to work offline and its APK must not request the `INTERNET` permission.

