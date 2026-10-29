# Viewer startup fault — 2026-10-29

> **Historical record.** Written when the viewer still offered a Vulkan backend. Vulkan was removed
> from the project afterwards (see `docs/audit/FINDINGS.md` AF-002): the renderer is GLES 3.1 only,
> and there is no "Try Vulkan" action in the current viewer. Read the diagnosis below as the incident
> history, not as instructions for the current build.

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

## Follow-up screenshot (13:27)

The next phone capture showed the viewer running on `gles31 · ANGLE (Samsung Xclipse 940)` at 58 FPS,
with 417,150 triangles submitted, but no visible avatar. A second source audit found:

- The camera UBO is 448 bytes, but both backends allocated/advanced two frame slices by the raw
  `minUniformBufferOffsetAlignment` (256 on the reported device). The second slice therefore ran past
  the allocation. GLES now uses a 512-byte aligned camera stride and allocates 1024 bytes; Vulkan uses
  the same aligned stride for allocation, copy, and dynamic descriptor offsets.
- The GLES ground quad was back-face culled from above. It is now drawn with culling disabled, matching
  the Vulkan ground pipeline and the shader comment.
- A missing material index could produce a zero alpha fallback and discard every fragment. The fallback
  is now opaque white with sane PBR defaults.
- The viewer header now says “tris submitted” rather than “tris on screen”, because the counter measures
  draw submission, not visibility.
- The MENU again includes **Self diagnostics**, with a copyable offline report covering device/engine
  health, backend/GPU/API/driver, FPS, submitted triangles/draw calls, quality, settings, and last error.

These changes are still **unverified on the phone**. The triangle counter alone is not proof of visible
geometry.

## Verification performed here

- `bash tools/test_renderer_host.sh`: **43 checks, 0 failures** against the real GLES/backend-selector
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

## Stability follow-up

The viewer now has a deterministic no-AI stability path: startup/frame/resize faults are classified and
journaled privately, a bounded policy tries safe settings and the alternate backend, and a frame probe
checks sustained non-empty submission. `MENU -> Self diagnostics` can copy/export the report, run Safe
recovery, or clear the journal. This is a regression-evidence loop, not proof of visible pixels. See
[SELF_IMPROVEMENT_PATH.md](SELF_IMPROVEMENT_PATH.md).


## Visual-quality follow-up (exported `avatar-1337.glb`, 14:44)

The phone now renders visibly at ~58 FPS on `gles31` (ANGLE / Xclipse 940), with 417,150 submitted
triangles and no renderer fault. The exported GLB passes `tools/validate_glb.py`, but its atlas shows
the remaining visual defect class:

- 17,520 UV charts at 2048², coverage 0.65, ~445 texels/m;
- many cloth/gear/skin triangles cover less than one atlas texel;
- the baked normal/base-colour maps contain single-texel confetti, consistent with the on-screen
  moiré/striping on cloth and gear;
- `textureMiB` was always zero because the native backends never wrote `RendererStats::textureBytes`;
- `preferVulkan=true` could silently fall back to GLES without a journal event.

Changes made for this evidence:

1. The texture bake now measures each triangle's local atlas footprint (metres/texel) and fades
   procedural micro-detail whose wavelength is below the atlas Nyquist limit. This targets the
   confetti/moiré without raising atlas memory or changing the mesh.
2. GLES and Vulkan now report uploaded atlas texture memory (including the GLES mip chain / Vulkan
   image allocation), and diagnostics includes a `Textures` line.
3. A Vulkan-preferred startup that lands on GLES records `BACKEND_FALLBACK` in the local journal.
4. The default orbit distance moved from 3.0 m to 3.5 m so a ~1.85 m avatar is less likely to be
   cropped head/feet at the default 32° vertical FOV.
5. Host regressions: the GLES mock now uploads a fixture atlas and asserts non-zero texture memory;
   the kernel bake harness asserts the normal atlas is band-limited rather than single-texel noise;
   the SDK journal tests round-trip `BACKEND_FALLBACK`.

Still not claimed fixed: blocky hands, eye-shell asymmetry, garment/skin interpenetration, and any
remaining framing overlap with the info card. Those need separate geometry/layout evidence. The
moire fix is a bake-time band-limit; it still needs a device screenshot/export comparison.

## Camera-control follow-up (16:35–16:37)

The next device reports confirm the visual-quality build is running: `Textures: 90.7 MiB`, no last
renderer error, and `BACKEND_FALLBACK` events are present when a Vulkan-preferred startup lands on
GLES. The screenshots nevertheless show the **camera** panel selected with no camera controls, and
the user reports the camera still cannot be moved.

Source audit found two UI defects:

1. The model gesture layer used `pointerInput(Unit)` but closed over the first composition's
   `CameraSettings`. Each drag/pinch event therefore recomputed from the initial camera instead of
   accumulating from the live camera, so orbit/zoom appeared dead or snapped back.
2. `ViewerPanel.CAMERA` rendered `Unit`, so the selected camera panel had no sliders, buttons, or
   reset action.

Changes made for this evidence:

- Gesture callbacks now read the current camera and callbacks through `rememberUpdatedState`, while
  the pointer-input coroutine remains alive for the whole gesture.
- The exact orbit/zoom transform moved into `CameraGesture` in the SDK and is covered by JVM
  regression tests (accumulating deltas, pinch direction, clamping, yaw wrap, invalid input).
- The camera panel now exposes orbit left/right, up/down, zoom in/out, reset, and sliders for yaw,
  pitch, distance, target height, and field of view. These drive the same `CameraSettings` path as
  gestures.
- Diagnostics now includes a `Camera` line so a device report can prove whether the UI camera state
  changed after a drag or slider move.

This is still **unverified on the phone**. Retest by dragging the model, pinching, and moving the
camera sliders; then export `MENU -> Self diagnostics` before and after. The `Camera` line should
change, and the rendered view should orbit/zoom with it.
