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

The kernel, renderer, SDK and app are written and build. The host test harness passes **386 checks
with 0 failures**; the C++ kernel implements **47/47** ABI symbols, and the Rust core implements
15/47 against the same ABI with byte-identical results on everything it covers. **Nothing in this
repository has been run on a physical Android device yet** — there is no phone and no GPU in the
build environment — so the honest claim is *implemented and host-verified*, not *device-proven*.
`DEVICE.md` is the ordered sheet a human runs on hardware, and `tools/device_check.sh` collects the
evidence in one command. `STATUS.md` keeps the per-phase state and the list of what is deliberately
still missing.

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
