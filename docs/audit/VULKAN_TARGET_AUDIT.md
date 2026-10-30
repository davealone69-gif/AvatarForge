# Vulkan target and capability audit

**Status: API 1.3 target, shader gate and ARM64 compile verified locally; AvatarForge runtime on a physical phone remains unverified.** This record keeps the requested target label, Vulkan API encoding, NDK headers, loader and device API separate.

## What “1.3.304” means here

The requested target label is Vulkan **1.3.304**. AvatarForge declares `VK_API_VERSION_1_3` in `VkApplicationInfo`, which encodes API **1.3.0**. Per the [Khronos `VkApplicationInfo` reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkApplicationInfo.html), the API version communicates the application's highest supported major/minor level; the patch component is ignored. There is no distinct runtime capability obtained by putting `304` in that patch field. The renderer therefore records `1.3.304` as the requested target label and uses the standards-defined API 1.3 encoding for negotiation.

The Gradle and CMake build pins Android NDK `26.1.10909125`. Its `vulkan_core.h` reports `VK_HEADER_VERSION 237` / complete header revision **1.3.237**. Per [Khronos `VK_HEADER_VERSION_COMPLETE`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_HEADER_VERSION_COMPLETE.html), that is a header/tooling revision, not the app's API target or a runtime version. AvatarForge has no separate Vulkan-Headers package pin. Runtime loader support is queried using [`vkEnumerateInstanceVersion`](https://docs.vulkan.org/refpages/latest/refpages/source/vkEnumerateInstanceVersion.html); the selected physical device's `apiVersion` is queried separately. The `AF_VK_CONTEXT` log records the target label, encoded app API, compiled header revision, loader version and selected device version as distinct values.

## Runtime checks implemented

The Vulkan path now fails closed to the existing GLES fallback unless the loader and selected physical device support Vulkan API 1.3. Before enabling them, the renderer enumerates the required `VK_KHR_surface` and `VK_KHR_android_surface` instance extensions and `VK_KHR_swapchain` device extension. Device selection checks a queue family for both graphics and presentation support; failed surface-support queries reject the candidate.

The renderer queries core `VkPhysicalDeviceFeatures` and enables only supported members it uses. It does not chain `VkPhysicalDeviceVulkan13Features`, because the implementation uses no optional Vulkan-1.3-only feature bits; API 1.3 is still the requested instance/device baseline. Surface capability checks reject failed or empty format/mode results, require color-attachment usage and FIFO presentation, and choose a supported composite-alpha mode. HDR formats must support the needed optimal-tiling features and the combined color-attachment/sampled-image usage at the swapchain extent. Depth/shadow selection checks attachment and linearly sampled support and image limits. Device limits used for texture dimensions, framebuffer samples, uniform-buffer alignment and anisotropy are queried before sizing or enabling those paths.

A loader or device below API 1.3 is intended to use the existing GLES 3.1 fallback. The source and host selector tests support that policy; the actual behavior on older and newer Android drivers still requires phone testing.

## Shader and build evidence

`tools/embed_vulkan_shaders.py` compiles all 13 Vulkan GLSL sources for `vulkan1.3` and validates the checked-in 13-module SPIR-V table with `spirv-val --target-env vulkan1.3`. The current source/table pair is SHA-pinned; this NDK's optimizer can emit different bytes, so `--check` validates the pinned snapshot without silently rewriting it. Results: **6/6** shader regression tests; all **13** sources compiled and all **13** embedded modules validated.

`tools/test_renderer_host.sh` passes **87/87** renderer host checks and **7/7** API-version helper checks. These cover GLES behavior, mocked Vulkan selection/fallback and version comparisons; they do **not** execute Vulkan commands or a GPU. `tools/build_android.sh arm64-v8a android-26` passes with NDK `26.1.10909125`. The native artifact guard confirms `libafrender.so` links Android's system `libvulkan.so` and no Vulkan loader is bundled.

At the earlier audit checkpoint, Gradle passed **3 app** and **38 SDK** JVM tests and built the debug APK, unsigned compile-only release AAB and SDK AAR. Package, debug signature, no-INTERNET permission and packaged native libraries were checked. These are compile/package results, not proof of Vulkan initialization or visible pixels.

## User-supplied SM-S921B recovery report and remaining proof

The owner supplied an AvatarForge recovery export from a Samsung **SM-S921B** (API 37, arm64-v8a).
It reports `VULKAN` requested but effective backend `gles31`, with `FALLBACK: Native Vulkan did not
initialize`; a journal event records `backend=GLES` after the Vulkan-init-to-GLES transition. The GPU
string is `ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304)`. This identifies the active app backend as
GLES; the ANGLE/Vulkan driver string does not mean AvatarForge's native Vulkan renderer initialized.

The same export reports `THERMAL_LIMIT` / **critical**, **29.3 FPS** with a **34.11 ms input-delta
EMA** (explicitly not present-completion timing), **20.18 ms** CPU submit, and **25.81 ms** GPU time.
It records 1,740 frame attempts, submissions and accepted presents, 422,600 triangles, 15 draw calls,
90.7 MiB texture memory, quality level 0, MSAA 1×, shadow map 1024, bloom 0, and last error `none`.
The report itself warns that frame submissions/presents do not prove visible pixels.

This is user-provided device evidence, not an independently reproduced test. The export does not
include the APK SHA or source commit, so it cannot be tied to the current working-tree build.
`adb devices -l` is empty in this environment, and no screenshot or validation-layer trace was
supplied. It is useful evidence of a GLES fallback and a critical-thermal, sub-30-FPS session, but
does **not** verify native Vulkan runtime or visible image correctness. An earlier checker report's
`1.3.304` capability label also does not prove AvatarForge selected Vulkan.

**Next device gate:** reproduce on the exact-head ARM64 APK; capture `AF_VK_CONTEXT` and Self diagnostics
during live clips; confirm the actual backend, loader/device API and driver; enable Vulkan validation
layers if available; exercise resize, surface loss, fallback and recovery; then attach matched
visible-frame screenshots and a thermal soak. Until then, native Vulkan runtime and image correctness
remain **UNVERIFIED**.
