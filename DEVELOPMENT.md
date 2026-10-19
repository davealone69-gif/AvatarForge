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

## Work order

1. Reproduce the current main-branch state and capture the first failing command, if any.
2. Fix the Rust end-to-end test/clip assertion mismatch noted in `STATUS.md`; verify against the current implementation rather than weakening the assertion.
3. Wire the existing scene/culling/LOD layer into the renderer only where the current renderer contracts support it; add host regression coverage first.
4. Run the host verification suite and Android package workflow.
5. Install and exercise the resulting APK on a physical ARM64 Android device before marking runtime work VERIFIED.

## Evidence labels

- **REAL + VERIFIED**: the exact behavior was exercised and evidence was captured.
- **UNVERIFIED**: source exists, but the required test or device run has not happened.
- **PARTIAL**: only a subset of the promised behavior passed.
- **MISSING**: no implementation exists.
- **BLOCKED**: a specific dependency or environment prevents the check.

A successful CI/package build does not establish physical-device runtime verification. Release artifacts signed with the debug certificate are for testing, not production distribution.
