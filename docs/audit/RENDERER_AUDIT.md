# Renderer audit

Status: **PARTIAL** — GLES-only host fault injection and CI packaging are REAL + VERIFIED; Android device pixel/thermal correctness remains UNVERIFIED. Vulkan support has been removed by user direction.

## Pipeline trace

`RenderSurfaceView.frameLoop` → `AvatarRenderer.frame` → JNI `rendererFrame` → `BudgetRenderer.renderFrame` → backend `renderFrame`.

GLES: EGL context/surface → shadow pass → forward pass → bloom → composite → `glGetError` → `eglSwapBuffers`.
There is no Vulkan path. Android may report that its own GLES driver uses a translation layer; that is driver text, not an AvatarForge backend choice.

## Zero-render / zero-FPS root causes found in source

1. Historical Vulkan frame-result defects were eliminated by deleting the backend rather than continuing to maintain an unverified second API.
2. `stats_.frames` incremented before present, so recovery probes and FPS could count work that never reached the compositor.
3. GLES logged frame errors but continued; swap failure only set a flag and did not surface a JNI exception.
4. `frameMs` was derived from caller delta but commented as GPU-side time; `cpuMs` was not measured; no GPU timestamp query exists.
5. Shader embedding could silently produce an empty SPIR-V table when `glslc` was absent.

## Fixes in this patch

- `Renderer::frameError()` propagates backend frame failure through JNI into the existing bounded recovery policy.
- Counters are distinct: `frameAttempts`, `frameSubmissions`, `presentRequestsAccepted`; `frames` now means accepted presents.
- `gpuMs = -1` means unknown. CPU submit wall time is measured by the budget wrapper.
- Recovery is one bounded safe-GLES retry; it never switches graphics API. Old BACKEND_FALLBACK journal records are decode-only history.
- GLES swap/GL frame failure becomes a frame error and stops the backend until owner recovery.
- Host mock suite injects swap failure and verifies counters/error/cleanup and C ABI telemetry parity: **57 checks, 0 failures**.

## Diagnostic scene ladder

Status: **MISSING** — required next implementation:
clear → triangle → indexed triangle → textured quad → lit sphere → skinned mesh → blendshape mesh → production avatar.
Each rung must report backend, surface size, attempts/submissions/accepted presents, draw calls, submitted triangles, texture bytes, and a human-visible pass criterion. Submission alone must never be labelled visible success.
