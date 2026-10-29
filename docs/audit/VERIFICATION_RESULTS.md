# Verification results

Checkpoint: 2026-10-29, branch `arena/aaf85469-avatarforge`.

## Executed locally

- `bash tools/test_renderer_host.sh`: **57 checks, 0 failures**. GLES/EGL are stateful host doubles; this proves API sequencing, error propagation, cleanup and C ABI telemetry layout, not pixels.
- `bash tools/test_model_host.sh`: **19 checks, 0 failures**. Reproduces the old world-axis palm defect and garment-layer/poke-through defects, then verifies the fixed analytic field.
- `bash tools/build_host.sh test`: **406 checks, 0 failures**. The old SPIR-V table-count failure is obsolete because the Vulkan/SPIR-V path was removed; the GLES shader table now has exactly the required 12 stages.
- `git diff --check`: clean.

## Not executed locally

- Gradle/JVM tests: no JDK/Gradle/Android SDK in this environment.
- Android native NDK build: no NDK/CMake/Ninja/Clang.
- Rust: no rustc/cargo.
- Device/adb: no device.
- Shader compilation: no glslc; CI performs the compiler gate.

## CI

**REAL + VERIFIED:** run https://github.com/davealone69-gif/AvatarForge/actions/runs/40864270280 succeeded on head `973f70f042132679228dd50e54853a6b815508f1`.

- GLES shader compile/diff gate passed; there is no SPIR-V warning because the SPIR-V table was deleted.
- Renderer host tests, ARM64 native build, SDK JVM tests, debug APK, unsigned release AAB, SDK AAR, GLES-only linkage verification, artifact verification and upload passed.
- Artifact: `AvatarForge-Android-973f70f042132679228dd50e54853a6b815508f1`, id `12892579593`, 17,609,630 bytes. It contains a debug APK, an explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS and PROVENANCE.json.
- The subsequent geometry/header patch adds model-detail and full kernel-host CI gates; its exact-head run is pending at the time of this record.

## Device evidence

- Camera: **user-confirmed working**. Exact build SHA/counters were not supplied, so this is functional confirmation, not a repeatable regression artifact.
- Earlier device reports (build `6883243`, SM-S921B API37 arm64, thermal severe): gles31 ANGLE/Xclipse 940, 90.7 MiB textures, 27.8–49.2 FPS, 300k–417k submitted triangles. These are historical GLES reports, not proof of the new GLES-only/model-fixed build.
- New hand/garment/header fixes: **UNVERIFIED on device** until the next CI APK is installed and captured.

Production signed release remains **BLOCKED**: the signed-release job requires production secrets on a tag.
