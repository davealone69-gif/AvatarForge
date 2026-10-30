# Renderer audit — current branch 2026-10-30

Status: **PARTIAL** — native Vulkan is the default, GLES remains the startup and bounded safe-recovery
fallback, and the selector is host-tested. Android app Vulkan commands, visible pixels and Samsung
thermal/clip sharpness remain **UNVERIFIED** until exact-head build and device gates pass. See
[the restoration notes](../VULKAN_RESTORE_2026-10-30.md) for the earlier restoration checkpoint.

## Pipeline trace

`RenderSurfaceView.frameLoop` → `AvatarRenderer.frame` → JNI `rendererFrame` →
`BudgetRenderer.renderFrame` → selected `GlesRenderer` or `VulkanRenderer.renderFrame`.
The app's default `RenderSettings.backend` is Vulkan; the C++ factory tries native Vulkan first,
then falls back to GLES on Vulkan startup error. Backend changes recreate the native renderer on its owner thread;
preferences cannot silently override the default. `actualBackend` reports what initialized,
not what was requested. A GLES driver reporting ANGLE-on-Vulkan is **still** `backend=gles31`.

GLES: EGL context/surface → shadow pass → forward pass → bloom → composite → `glGetError` →
`eglSwapBuffers`. Vulkan: device/queue → swapchain/depth/MSAA targets → command buffer →
`vkQueueSubmit` → `vkQueuePresentKHR`. The Vulkan path uses the system loader, a separate
13-module checked-in SPIR-V table, and independent shader source compilation in CI.

## Zero-render / zero-FPS defects and fences

1. Historically Vulkan frame-result failures could count blank frames or retry indefinitely.
   Now fatal acquire/command-record/submit/present/target-rebuild errors are latched as a
   `frameError()`, rejected by JNI and passed through bounded safe-GLES recovery. Submission
   and accepted-present counters are separate. Neither counter proves compositor-visible pixels.
2. GLES swap/GL frame failures similarly stop the backend. Host mocks exercise its startup,
   cleanup, error propagation and C ABI telemetry. A factory-only Vulkan double tests selection
   and fallback, **not GPU command execution**.
3. CPU submit wall time is measured by the budget wrapper. Vulkan now writes a timestamp pair only
   when the selected queue has valid timestamp bits and the device supports timestamp queries; it
   reads results after the in-flight fence, masks counter wrap, and leaves `gpuMs = -1` on
   unsupported or invalid results. The existing C ABI/JNI stats surface GPU time plus draw calls,
   triangles and texture bytes. Host tests cover the conversion and unknown sentinel, but this
   sandbox has no Vulkan headers or GPU, so the Vulkan translation unit and device path remain
   **UNVERIFIED**. GLES GPU timer queries are not implemented, and its GPU timing remains unknown.
4. Shader tools require NDK `glslc` for compilation/freshness checks, and cannot silently emit an
   empty Vulkan table. CI checks both shader dialects separately. Source/host checks alone do
   not establish a runnable APK or sharp image.
5. The UV-atlas packing and per-clip CPU skinning fixes target visual blur and frame budget
   *regardless of backend*. They still need matched paused/live GPU screenshots on the SM-S921B.

## Verification

The full host kernel suite passed **437 checks, 0 failures**, including the AF-038 1024/2048
atlas checks and AF-043 handle regressions. Renderer selection/GLES fault-injection passed **87/0**;
diagnostic metadata and timestamp-conversion tests passed in the renderer host script. The Vulkan
translation unit was not syntax-checked because Vulkan/NDK headers were unavailable, and hardware
tests are **none on this branch**. Check exact-head Android CI status in
[PROJECT_STATUS.md](../../PROJECT_STATUS.md), then follow [DEVICE.md](../../DEVICE.md) for
both-backend comparison and fallback exercises.

## Diagnostic scene ladder

**PARTIAL — metadata only.** The eight rungs (clear → triangle → indexed triangle → textured quad
→ lit sphere → skinned mesh → blendshape mesh → production avatar) now have names and human-visible
pass criteria, and a host contract test covers their range and metadata. There is still no selectable
scene implementation or UI that renders those rungs, so backend-by-backend visual verification and
per-rung attempts/submissions/accepted presents, draw calls, triangles and texture-byte reporting
remain **MISSING**.
