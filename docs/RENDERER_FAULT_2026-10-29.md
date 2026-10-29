# Viewer startup fault — 2026-10-29

## Observed on the supplied phone screenshot

- The avatar build completed: 417,150 triangles, 304,797 vertices, 71 joints, 52 ARKit blendshapes.
- The viewer stayed at `initialising GPU…`, `0 FPS`, and `0 tris on screen`.
- The reported cause was: Vulkan switched off in viewer settings; OpenGL ES 3.1 failed during renderer initialization.
- This is a **renderer startup fault**, not evidence that generation failed.

The screenshot does not identify the exact GLES driver call that failed. The source audit below found
reproducible API-sequencing defects, but the fix remains **unverified on the affected phone**.

## Source findings and changes

1. GLES sampler uniforms were written without making their program current. `glUniform1i` now runs
   with the owning program bound, and the previous program is restored.
2. Ground/skeleton VAOs described attributes before their buffers existed; the avatar VAO also lacked
   an element-buffer binding. Buffer names are now allocated before VAO setup, and lazy geometry uploads
   retain the avatar VAO's element binding.
3. Initialization now reports the failing stage and GL/EGL error code. Shader compile/link logs retain
   the stage/program name. Failure cleanup releases partial resources and EGL objects.
4. Unsupported MSAA storage falls back to one sample. Unusable floating-point colour attachments fall
   back to core RGBA8 and report HDR as unavailable. Extension checks match complete tokens.
5. The viewer has **Retry viewer** and **Try Vulkan / Try OpenGL ES** actions. Changing the backend
   preference recreates the native renderer on its owning thread; `setOptions` alone cannot switch APIs.
6. Failed startup/frame paths reset stale renderer info/statistics instead of presenting an initializing
   GPU with a misleading performance warning. A failed frame loop no longer polls every 16 ms forever.
7. The Android APK workflow rebuilds ARM64 native libraries from the checkout before packaging, so a
   source fix cannot be silently shipped with the old checked-in `prebuilt/` binaries.

## Verification performed here

- `bash tools/test_renderer_host.sh`: **41 checks, 0 failures** against the real GLES/backend-selector
  code with a stateful GLES/EGL test double. Covers sampler ownership, VAO storage, indexed draw bindings,
  compile/link/EGL/VAO failures, cleanup, MSAA/HDR fallback, extension parsing, and retry/fallback selection.
- The same regression suite against the pre-fix GLES source reports **19 failures**, including the
  sampler/VAO ownership defects.
- Existing full kernel harness: **405 checks, 1 pre-existing failure** in the embedded shader-table stage
  count (`13 of 12`). This change does not modify the kernel or shader tables; that failure is not claimed fixed.
- No physical Android GPU was available in this workspace. The mock tests are **not** proof of visible
  geometry, driver compatibility, frame rate, or lifecycle recovery on the phone.

## Phone retest

1. Install the APK built from the fix commit (not the older checked-in APK or the 2026-10-21 recovery bundle).
2. Generate an avatar and open the viewer. Record the backend/device line and the actual triangle count.
3. If startup fails, use **Try Vulkan** and **Try OpenGL ES** separately. Capture the full error text and
   `adb logcat -s AvatarForgeRenderer:E` for each attempt.
4. Confirm visible geometry and non-zero FPS; exercise orbit/zoom, resize/rotation, background/foreground,
   and retry after a failure.
5. Record the result and device/GPU/Android version here or in `DEVICE.md`. Do not mark the fault closed
   from a successful build or a successful host mock test alone.