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
                              Vulkan / GLES 3.1 renderer ◀────────────┤
                              GLB export / GLB import ◀───────────────┘
```

## Status, in one paragraph

The current native source passes **464 host checks with 0 failures**; the C++ kernel implements **47/47**
ABI symbols, while the Rust alternative implements **15/47** and matches the C++ reference on the
functions it covers. Host coverage includes culling, index-only renderer LOD data, adaptive atlas
budget selection, GPU-profile policy, ASTC/ETC2 encoding and mip generation, an ASTC decode round-trip,
asynchronous compression polling, a staged native pipeline, and image-role mapping after a rejected
image. GL/Vulkan sources pass host syntax-only compilation with downloaded Khronos headers and an
Android-window stub; CMake host configuration and the scalar ASTC/ETC2 static codec targets also build.
This is **not** an Android NDK build or a GPU upload/runtime test. Kotlin lifecycle, dependency-graph,
thermal and resource changes remain **uncompiled**. The compressed GL/Vulkan upload paths are integrated
but have not been exercised on hardware. The checked-in 0.10.1 APK/AAR predate this follow-up.
**Nothing has been run on a physical Android device**; `DEVICE.md` is the hardware checklist and
`tools/device_check.sh` collects evidence when a device is available.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/afcore.h` | **The shared C ABI** — the single contract every native implementation answers to |
| `native/src/` | C++17 engine kernel: anatomy → field → isosurface → UV → bake → rig → blendshapes → clips → GLB |
| `native/src/render/` | Custom Vulkan (primary) and OpenGL ES 3.1 (fallback) renderers, GPU profiles and compressed atlas uploads |
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


## Update imported from Dropbox (2026-10-21)

The update notes and verification records have been imported into
[`docs/dropbox-update-2026-10-21/`](docs/dropbox-update-2026-10-21/). This is documentation and build-evidence
synchronization only: the shared Dropbox folder contains APK/AAR binaries and documentation, **not a full
source checkout**. The binaries have not been uploaded into this repository by this change, and this
commit does not claim a new build or a phone-runtime pass.

- [Updated status and evidence](docs/dropbox-update-2026-10-21/STATUS.md)
- [Build instructions](docs/dropbox-update-2026-10-21/BUILD.md)
- [Physical-device acceptance checklist](docs/dropbox-update-2026-10-21/DEVICE.md)
- [Build 0.10.1 evidence](docs/dropbox-update-2026-10-21/BUILD-0.10.1.txt)
- [SHA-256 manifest for the Dropbox artifacts](docs/dropbox-update-2026-10-21/SHA256SUMS.txt)
- [Open the original Dropbox folder to obtain the APK/AAR files](https://www.dropbox.com/scl/fo/s22v1herlm6dgzc825gx7/AOAZnyUA6jJ3KC1NXT_pXWs?rlkey=b7jvxccotfdaqoab72c88x9qv&st=bml9bve2&dl=0)

**Runtime status remains UNVERIFIED:** the update's own report says the Galaxy S24 installation, rendering,
offline generation, GPU behavior, performance and thermal/lifecycle acceptance have not been tested.
