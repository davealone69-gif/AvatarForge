# Verification results

Checkpoint: 2026-10-29, branch `arena/aaf85469-avatarforge`.

## Executed locally

- `bash tools/test_renderer_host.sh` with host zlib env: **53 checks, 0 failures**.
- `tools/build_host.sh`: rebuilt `libafcore.so` and `host_tests` successfully (warnings only in output).
- `build-host/host_tests`: **406 checks, 1 failure** — the same pre-existing embedded-shader table-count assertion at `native/tests/host_tests.cpp:1255` (`13 of 12`). GLB round-trip/hostile-input and live-engine ABI import sections passed.
- `git diff --check`: clean before checkpoint.

## Not executed locally

- Gradle/JVM tests: no JDK/Gradle/Android SDK in this environment.
- Android native NDK build: no NDK/CMake/Ninja/Clang.
- Rust: no rustc/cargo.
- Device/adb: no device.
- Shader compilation: no glslc.

## CI

**REAL + VERIFIED:** run https://github.com/davealone69-gif/AvatarForge/actions/runs/40858597544 succeeded on head `5e2f990b5435cfdfa5ee709b905e675b0478a2cb` in 2m20s.

- Shader source compile check passed; GLES table matched. CI warned that regenerated SPIR-V differs from the checked-in table and used the regenerated table for that build.
- Renderer host tests, ARM64 native build, SDK JVM tests, debug APK, unsigned release AAB, SDK AAR, artifact verification and upload all passed.
- Artifact: `AvatarForge-Android-5e2f990b5435cfdfa5ee709b905e675b0478a2cb`, id `12890980765`, 18,226,242 bytes. It contains a debug APK, an explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS and PROVENANCE.json.
- Earlier red runs were fixed, not hidden: `40857157931` failed on strict SPIR-V diff; `40857486764` failed on Gradle signing-variable scope. Both are superseded by the green run.

Production signed release remains **BLOCKED**: the signed-release job is skipped on PRs and requires production secrets on a tag.
