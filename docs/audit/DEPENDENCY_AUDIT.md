# Dependency audit

Status: **PARTIAL** — inventory is source-derived; no full Gradle dependency resolution or SBOM was run locally.

## Build toolchain

- Gradle wrapper 8.9; wrapper JAR SHA-256 recorded in `BASELINE.md`, authenticity not independently verified.
- AGP 8.5.2, Kotlin 1.9.24, Compose compiler 1.5.14, Java 17.
- Android compile/target 34, min 26, NDK 26.1.10909125, CMake 3.22.1.
- CI actions: `actions/checkout@v4`, `actions/setup-java@v4`, `android-actions/setup-android@v3`, `actions/upload-artifact@v4`, `softprops/action-gh-release@v2`. These are tag-pinned, not commit-SHA-pinned.

## App/SDK libraries

Compose BOM 2024.06.00, activity-compose 1.9.0, core-ktx 1.13.1, exifinterface 1.3.7, lifecycle-runtime-compose 2.8.2, kotlinx-coroutines-android 1.8.1, JUnit for SDK tests. No HTTP client, database, WebView, analytics SDK, or AI runtime was found in the app/SDK source.

## Native

Custom C++17 kernel/renderer. zlib is used for PNG; Android links the platform zlib. The default GLES renderer needs Khronos EGL/GLES headers; the opt-in native Vulkan backend additionally needs NDK Vulkan headers and links Android's **system** `libvulkan.so` (no bundled loader). Shader-table generation uses NDK `glslc`/`spirv-val`. No Filament/bgfx/Lugdunum runtime dependency is present.

## Rust

Workspace is dependency-free by manifest, version 0.9.0, rust-version 1.70, `panic=abort`. Repository URL is `https://example.invalid/avatarforge`. Not built in this environment.

## Supply-chain risks

1. No repository LICENSE.
2. Tracked `prebuilt/` and `artifacts/` are convenient but can become stale/unprovenanced binaries.
3. CI actions are not SHA-pinned.
4. Gradle wrapper authenticity is not verified against a published checksum in CI.
5. Release signing credentials are absent; production release is BLOCKED, not debug-signed.
