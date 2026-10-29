# Verification results

Checkpoint: 2026-10-29, branch `arena/aaf85469-avatarforge`.

## Executed locally

- `bash tools/test_renderer_host.sh`: **57 checks, 0 failures**. GLES/EGL are stateful host doubles; this proves API sequencing, error propagation, cleanup and C ABI telemetry layout, not pixels.
- `bash tools/test_model_host.sh`: **33 checks, 0 failures**. Covers the world-axis palm and garment-layer/poke-through regressions plus eye-label side mapping, exposed-sclera symmetry, extracted eye-patch symmetry, the chest-plate shell and hair clearance.
- Host GLB export of the current device spec (`athletic woman, ponytail, full plate armour`, 1.75 m, outfit 5, ponytail): **302,545 verts / 412,266 tris**, `validate_glb.py` OK. Eye patches are **2,359 / 3,148** triangles; gear front is **−0.139 m** instead of the pre-fix −0.222 m; hair top is **crown + 0.024 m** instead of +0.077 m. The afro style still clears the skull by **0.085 m**.
- `bash tools/build_host.sh test`: **406 checks, 0 failures**. The old SPIR-V table-count failure is obsolete because the Vulkan/SPIR-V path was removed; the GLES shader table now has exactly the required 12 stages.
- `git diff --check`: clean.

## Not executed locally

- Gradle/JVM tests: no JDK/Gradle/Android SDK in this environment.
- Android native NDK build: no NDK/CMake/Ninja/Clang.
- Rust: no rustc/cargo.
- Device/adb: no device.
- Shader compilation: no glslc; CI performs the compiler gate.

## CI

**REAL + VERIFIED:** run https://github.com/davealone69-gif/AvatarForge/actions/runs/40895347516 succeeded on head `39c8cf68663e6b22419d392780095c74fb08bd3f` in 7m52s.

- GLES shader compile/diff gate passed; there is no SPIR-V warning because the SPIR-V table was deleted.
- Model-detail regressions, full kernel host harness, renderer host tests, ARM64 native build, SDK JVM tests, debug APK, unsigned release AAB, SDK AAR, GLES-only linkage verification, artifact verification and upload passed.
- Artifact: `AvatarForge-Android-39c8cf68663e6b22419d392780095c74fb08bd3f`, id `12905983840`, 17,610,882 bytes. It contains a debug APK, an explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS and PROVENANCE.json.
- Earlier runs 40867172914 (`1fcbe6c`) and 40864270280 (`973f70f`) are superseded by this exact-head eye/hair/chest-fixed run.

## Device evidence

- Camera: **user-confirmed working**. Exact build SHA/counters were not supplied, so this is functional confirmation, not a repeatable regression artifact.
- Earlier device reports (build `6883243`, SM-S921B API37 arm64, thermal severe): gles31 ANGLE/Xclipse 940, 90.7 MiB textures, 27.8–49.2 FPS, 300k–417k submitted triangles. These are historical GLES reports, not proof of the new GLES-only/model-fixed build.
- Current evidence supplied 2026-10-29 21:25 (SM-S921B, GLES 3.1, 416,396 tris): screenshot still shows the pre-fix metal-ball cuirass and hair over the brow. Its GLB measures **1,637 left-eye vs 156 right-eye triangles**, gear front **−0.222 m**, hair top **crown + 0.077 m**. Recovery report shows severe thermal, requested quality MEDIUM but effective LOW, 29.2 FPS, no renderer fault. This is the defect evidence, not proof of the fixes.
- New eye/hair/chest/hand/garment/header fixes: **UNVERIFIED on device** until the next CI APK is installed and captured.

Production signed release remains **BLOCKED**: the signed-release job requires production secrets on a tag.
