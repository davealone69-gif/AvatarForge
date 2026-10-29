# Native Vulkan restoration and live-clip quality — 2026-10-30

**Status: source restored, physical-device visibility UNVERIFIED.** This is a new opt-in renderer,
not a claim that Vulkan Checker or Android's ANGLE-on-Vulkan is AvatarForge's native Vulkan path.
GLES remains the default and the bounded recovery target. The affected clip is live `walk`/`run`
playback inside the viewer, not an encoded recording.

## Diagnosis before changes

- The phone's third-party Vulkan Checker report shows Vulkan 1.3.304, Xclipse 940, a graphics
  queue and `VK_KHR_swapchain`; GLView reports OpenGL ES 3.2 implemented through ANGLE on Vulkan.
  These describe **capabilities and another app**, not AvatarForge rendering or texture sharpness.
- The pre-removal backend at `76fdebab` was a real Vulkan implementation, but it had depended on
  code and compiled shader tables later deleted; restoring just a preference flag would not work.
  The audit's AF-002 defect was fatal frame errors counted/retried without notifying recovery.
- The *separate* AF-034/036 UV-atlas fault folded 10,788 vertices onto chart borders and left
  17,271 triangles with zero texel area in the measured pre-fix mesh. The ported fix makes that
  overflow a failing invariant; its original benchmark reports zero clamped vertices and 30
  zero-area triangles after the fix. AF-035 moved joint matrix products out of the per-vertex
  skinning loop and removed a per-frame area-weighted-normal triangle pass. The original
  desktop benchmark fell from 10.14 ms to 4.65 ms per `af_engine_apply_clip` call. **Neither
  number is a measured result on the SM-S921B.** Small UV charts can still be under-resolved.

## Implementation

- Ported the upstream UV packing, atlas-footprint regression and clip-skinning changes to the
  current branch. These change the generated atlas and per-frame CPU work, independent of backend.
- Restored `af_vk.cpp` and its culling/LOD/GPU-profile helpers with its **own** `shaders_vk/`
  source tree and precompiled 13-stage SPIR-V table. Existing GLES shaders and 12-stage table
  remain independent. Removed the old async ASTC/ETC2 encoder from this restoration rather than
  adding a second live texture-owner race; the Vulkan path uploads uncompressed atlases/mips.
- Explicit `RenderSettings.backend` opt-in goes through JNI to the native factory. Startup tries
  Vulkan then GLES when Vulkan initialization fails; the app reports the **actual** backend and
  journals a fallback. Changing API recreates the native renderer on its owning thread, preserving
  camera, engine attachment, clip selection and overlays. Old persisted `preferVulkan` alone
  cannot silently select the experimental backend.
- The Vulkan backend owns an Android window reference across surface recreation. It propagates
  unrecoverable `vkAcquire`/`vkResetCommandBuffer`/record/submit/present and target-rebuild errors
  through `frameError()` to JNI's exception and one safe GLES recovery. Atlas upload, white
  placeholder clear, ground-vertex upload, material-buffer and descriptor failures also stop
  rather than reporting a presented but untextured/undefined avatar. Target rebuilding rejects
  a failed device-idle wait before touching in-flight resources. Submission/accepted-present counters are only advanced after
  their respective successful calls; **none proves visible pixels**. Anisotropy is clamped against
  the actual physical-device limit and capped at 4×.
- Android build links the **system** `libvulkan.so` alongside EGL/GLES. The artifact gate requires
  all three, rejects a bundled Vulkan loader, and runs on APK/AAB/AAR. Shader regeneration is
  fail-closed if NDK `glslc` is missing: it cannot overwrite SPIR-V with an empty table.

## Verification / remaining gates

- `bash tools/build_host.sh test`: **411 checks, 0 failures** (host kernel, incl. UV/rig).
  This environment needed a compatible local zlib header (`/usr/local/include/node`) and symlink
  to the system libz; neither is committed or shipped.
- `bash tools/test_renderer_host.sh`: **81 checks, 0 failures** (real GLES/EGL mock and a
  factory-only Vulkan double). It does not execute Vulkan commands.
- `python3 tools/test_renderer_artifacts.py`: **3 tests pass**; package/linkage logic only.
- `python3 tools/test_embed_vulkan_shaders.py`: **6 tests pass**. Sources and the actual checked-in
  SPIR-V table match the byte-identical pre-removal snapshot; mutations to either are rejected.
  Validator calls are mocked in this local test, **not** a local SPIR-V compilation.
- `g++ -fsyntax-only` against temporarily fetched Vulkan headers: `af_vk.cpp` and its helper
  translation units compile on the host. **Not an NDK build or GPU test.** `python3
  tools/embed_vulkan_shaders.py --check` requires NDK `glslc`, unavailable locally; CI runs it.
- Android APK workflow compiles both shader dialects, validates the SHA-pinned historical Vulkan
  SPIR-V modules with NDK `spirv-val`, and gates ARM64 native build, JVM tests and APK packaging.
  **Check the run for the current commit before treating its APK as built**; a green build still
  cannot approve pixels. No `adb`, local Android SDK/NDK, or SM-S921B is attached here, so
  neither backend's live image has been visually approved.

**Device gate:** install the exact-head debug APK, regenerate an avatar, compare paused `REST`
with live `walk`/`run` at matched camera/quality/thermal state on both GLES and explicit native
Vulkan, export two diagnostics reports, capture screenshots and app `logcat` failures, exercise
resize/background/foreground and safe fallback. See [DEVICE.md](../DEVICE.md) for the ordered
checklist. If native Vulkan falls back to GLES or produces blank frames, do not label it fixed.
