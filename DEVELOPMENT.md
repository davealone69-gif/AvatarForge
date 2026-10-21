# AvatarForge Development Entry Point

This document is the starting point for work on AvatarForge. Keep changes on a feature or fix branch; do not commit directly to `main`.

## Project boundaries

- Android-first standalone app and reusable Kotlin SDK.
- Native C++ engine behind `native/include/afcore.h`; keep the C ABI as the shared contract.
- Vulkan is the preferred renderer and OpenGL ES 3.1 is the fallback.
- Rust wrappers must remain compatible with the same ABI.
- Offline-first: do not add network permission, remote services, telemetry, or model downloads without an explicit product decision.
- Do not claim a feature works solely because its code exists. Distinguish host tests, Android package build, and installed-device runtime evidence.

## First commands

From the repository root:

```bash
tools/build_host.sh test
python3 tools/abi_manifest.py --check
python3 tools/embed_shaders.py --check
tools/verify.sh
```

Android package build (requires JDK 17, Android SDK 34, NDK 26.1.10909125):

```bash
tools/build_android.sh
./gradlew --no-daemon :app:assembleDebug :app:assembleRelease :sdk:assembleRelease \
  -Paf.abIs=arm64-v8a -Paf.prebuiltJni="$PWD/prebuilt"
```

Device verification requires a real Android device and must be reported separately:

```bash
tools/device_check.sh --apk artifacts/avatarforge-debug-arm64.apk --soak 120
```

## Current review state (2026-10-20)

- Native host harness: **464/464**. `[0b]` covers role-to-analysis mapping when an earlier image is
  rejected; `[10b]` the culler; `[10c]` GPU profiles, ASTC/ETC2 encoding and async polling plus
  `parallelFor` exception/cancellation; `[10d]` the safe mesh-decimator candidate; `[10e]` renderer-owned
  index-only LOD data. The native ABI test executes the staged graph before finalization. GL/Vulkan/JNI
  sources pass host syntax-only checks using downloaded Khronos headers and Android type stubs; this is
  not NDK validation. Host CMake configuration and scalar ASTC/ETC2 static targets build.
- Kotlin Phase 1–3 follow-up is in the working tree and **not compiled**: serialized engine jobs,
  renderer leases, render-thread picking/blendshapes/teardown, native-parity image roles, resource
  deferral, a topological stage graph with cancellation boundaries, monotone weighted progress,
  API-29+ thermal quality feedback, and optional Android 12+ ADPF frame-work hints. Graph/progress/
  thermal JVM tests are present but unrun.
- Atlas selection adapts among 2048², 1024², 512² and 256² using a conservative 19-byte-per-texel
  bake-workspace estimate. Vendored ASTC/ETC2 encoders, semantic mip generation, async compression,
  compressed GL/Vulkan uploads, device-format checks and raw fallback are now implemented. The codec
  tests pass; GPU uploads and per-device behavior remain unverified on Android hardware.
- The Rust end-to-end clip check compares against `vertices_rest()`, verifies REST restoration, and
  checks that walk → REST → walk does not accumulate drift. The Rust cdylib is `libafcore_rust.so` so
  Cargo cannot substitute it for the C++ reference `libafcore.so`.
- The environment has no Android SDK/NDK or standalone Kotlin compiler and only Java 11 (JDK 17 is
  required). Gradle dependency resolution failed before compilation due to Maven TLS handshakes. The
  checked-in APK/AAR remain the 2026-10-18 build; no Android package or physical-device verification
  has been performed for these edits.
- Renderer LOD, adaptive atlas selection, staged jobs and thermal policy are source implementations,
  not device-verified. Phase 4 is deferred by the current scope.

## Work order

1. Reproduce the checked-out branch state and capture the first failing command, if any.
2. Keep the host regressions green: `tools/build_host.sh test`, Rust workspace tests, and the ignored
   Rust end-to-end engine test. Do not weaken assertions to hide implementation defects.
3. Build the Android packages with the pinned JDK/SDK/NDK. Refresh prebuilt libraries/APKs only from
   that build and keep release signing credentials out of the repository.
4. Install the resulting APK on a physical ARM64 Android device; capture the renderer, lifecycle,
   thermal and performance evidence in `DEVICE.md` before marking runtime behavior VERIFIED.

## Evidence labels

- **REAL + VERIFIED**: the exact behavior was exercised and evidence was captured.
- **UNVERIFIED**: source exists, but the required test or device run has not happened.
- **PARTIAL**: only a subset of the promised behavior passed.
- **MISSING**: no implementation exists.
- **BLOCKED**: a specific dependency or environment prevents the check.

A successful CI/package build does not establish physical-device runtime verification. The debug APK is for testing; release APK/AAB packaging is blocked unless a real release keystore is configured. R8/resource shrinking and physical-device runtime verification remain separate gates.
