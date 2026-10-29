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

Pending push. The workflow now compiles shaders, diffs generated headers, builds ARM64 native libraries, runs SDK JVM tests, builds a debug APK and explicitly unsigned release AAB, verifies permissions/ABI/signature status, and writes SHA-256/provenance files.
