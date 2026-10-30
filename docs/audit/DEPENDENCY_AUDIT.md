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

Custom C++17 kernel/renderer. zlib is used for PNG; Android links the platform zlib. The default native Vulkan renderer uses NDK Vulkan headers and Android's **system** `libvulkan.so`; GLES 3.1 remains the startup/recovery fallback and uses Khronos EGL/GLES headers (no bundled Vulkan loader). Shader-table generation uses NDK `glslc`/`spirv-val`. No Filament/bgfx/Lugdunum runtime dependency is present.

## Rust

Workspace is dependency-free by manifest, version 0.10.1, rust-version 1.70, `panic=abort`, with the real GitHub repository URL. The default workspace tests passed locally against the explicit C++ `build-host/libafcore.so` preload; the optional renderer feature has separate compile/doc-test coverage because its native GPU symbols are not part of the default host kernel build.

## Vulkan target distinction

The requested target label is Vulkan `1.3.304`; `VkApplicationInfo` uses the standards-compliant `VK_API_VERSION_1_3` encoding (patch zero, ignored by Vulkan), and the source gates the loader and selected physical device at API 1.3. Vulkan SPIR-V is compiled/validated for `vulkan1.3`. The pinned NDK 26.1.10909125 headers identify `VK_HEADER_VERSION 237` (header revision 1.3.237); this is not the target label or a runtime version. The supplied SM-S921B checker report remains separate capability evidence, not proof of AvatarForge initialization. Local ARM64 compilation/link and source/shader tests pass; runtime loader/device values are unobserved until the exact app runs on a phone. See [the target audit](VULKAN_TARGET_AUDIT.md).

## Supply-chain risks

1. No repository LICENSE.
2. Tracked `prebuilt/` and `artifacts/` are convenient but can become stale/unprovenanced binaries.
3. CI actions are not SHA-pinned.
4. Gradle wrapper authenticity is not verified against a published checksum in CI.
5. Release signing credentials are absent; production release is BLOCKED, not debug-signed.
