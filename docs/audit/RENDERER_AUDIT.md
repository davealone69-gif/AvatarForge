# Renderer audit — current branch 2026-10-30

Status: **PARTIAL** — GLES is the default, a distinct native Vulkan backend is opt-in, and
startup/frame failure has a bounded safe-GLES recovery. Android build, real Vulkan commands,
visible pixels and Samsung thermal/clip sharpness remain **UNVERIFIED** until the exact-head
CI and device gates pass. See [the restoration notes](../VULKAN_RESTORE_2026-10-30.md).

## Pipeline trace

`RenderSurfaceView.frameLoop` → `AvatarRenderer.frame` → JNI `rendererFrame` →
`BudgetRenderer.renderFrame` → selected `GlesRenderer` or `VulkanRenderer.renderFrame`.
The C++ factory tries native Vulkan only if `RenderSettings.backend == VK`. It requests Vulkan
**1.3 core**, rejects physical devices below 1.3, then falls back to GLES on startup error. The
reported device patch (such as `.304`) is not a requested feature. Backend changes recreate the
native renderer on its owner thread;
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
3. `gpuMs = -1` means unknown; CPU submit wall time is measured by the budget wrapper. Real GPU
   timestamp instrumentation and a diagnostic scene ladder remain **MISSING**.
4. Shader tools require NDK `glslc` for compilation/freshness checks, and cannot silently emit an
   empty Vulkan table. CI checks both shader dialects separately. Source/host checks alone do
   not establish a runnable APK or sharp image.
5. The UV-atlas packing and per-clip CPU skinning fixes target visual blur and frame budget
   *regardless of backend*. They still need matched paused/live GPU screenshots on the SM-S921B.

## Verification

Host kernel **411 checks / 0 failures**, renderer selection/GLES/version gate **87/0**,
artifact guard **3/0**, Vulkan translation units `-fsyntax-only` with fetched headers.
Hardware tests: **none on this branch**. Check exact-head Android CI status in
[PROJECT_STATUS.md](../../PROJECT_STATUS.md), then follow [DEVICE.md](../../DEVICE.md) for
both-backend comparison and fallback exercises.

## Diagnostic scene ladder

**MISSING** — clear → triangle → indexed triangle → textured quad → lit sphere → skinned mesh →
blendshape mesh → production avatar. Each rung needs a human-visible pass criterion plus backend,
surface size, attempts/submissions/accepted presents, draw calls, triangles and texture bytes.
