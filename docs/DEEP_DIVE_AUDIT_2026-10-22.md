# AvatarForge deep-dive audit — 2026-10-22

Scope: `work/android-engine-dev`, current feature/fix branches, renderer telemetry report, Android UI flow, generation/GLB path, and the Samsung Galaxy S24/Xclipse 940 target. This is a source/CI audit, not a claim of physical-device verification.

## Executive status

- **CI:** PR #11 diagnostics commit passed Android package compilation, host regression/shader/GLB/ABI checks, JVM tests, APK permission/ABI audit, and artifact upload. This verifies CI only.
- **Phone rendering:** UNVERIFIED. No current-source APK has been installed and exercised on the physical Samsung S24 in this audit.
- **Main renderer hypothesis:** the supplied all-zero frame/FPS/CPU-call telemetry is stronger evidence that the Kotlin frame loop is not reaching or publishing a frame than it is evidence of a blendshape shader failure.
- **Immediate source defect found:** `RenderSurfaceView.surfaceChanged()` removed the only scheduled frame callback when `renderer.resize()` failed. That can permanently stop frames until another surface event. PR #12 changes this to schedule a delayed retry while the surface is still valid and retain the visible error until a successful frame clears it.
- **Additional native risk found:** Vulkan frame submission/presentation errors are not all handled. In particular, a failed `vkQueueSubmit` occurs after resetting the in-flight fence; returning without restoring/retiring that fence can leave the next frame waiting indefinitely. This needs a separate native recovery change and device/validation-layer testing.
- **No-server architecture:** the app explicitly targets offline/on-device generation and has no INTERNET permission. A server start button or server dependency would be incorrect unless the product requirement changes. The current server/bridge surface is therefore intentionally absent, not a missing backend that should be faked.

## Renderer and zero telemetry

### Confirmed from source

1. `RenderSurfaceView.frameLoop` calls `AvatarRenderer.frame()`, then publishes `renderer.stats()`. If the renderer is null it retries after a short delay; frame exceptions are also retried.
2. `surfaceChanged()` previously stopped the frame callback on a failed resize. This is a direct path to a persistent zero-statistics state and is addressed in PR #12.
3. The native `BudgetRenderer::renderFrame()` wraps the backend call with a steady-clock timer and copies backend stats. If Kotlin reaches this method, CPU-call wall time should normally be nonzero even when the backend fails to draw.
4. The GL backend increments frame count and updates frame interval/FPS before its draw passes. The Vulkan backend does likewise before image acquisition. Thus all-zero frame interval, FPS, and renderer-call time point upstream of actual mesh rasterization: frame scheduling, renderer invocation, stats publication, or a renderer handle/lifecycle failure.
5. Native shader compilation/linking in GLES captures `glGetShaderInfoLog`/`glGetProgramInfoLog` and returns the reason through renderer creation. The current diagnostics UI must display that error, not replace it with a generic black-screen claim.
6. Vulkan visible-triangle/draw counters are assigned during command recording. They are not independent GPU-capture evidence. A zero counter means no counted draw in that backend frame, not proof that the driver received no commands in all cases.

### Blendshape / skinning hypothesis

- The engine deforms the vertex stream on the CPU and the renderer uploads the current `af_engine_vertices()` stream. The render shaders are not expected to process all 52 ARKit channels or 71 joints as a large per-vertex shader uniform array.
- Therefore the quoted GLES error about `out` requiring `flat` interpolation is **not established for this code path**. It should only be treated as the cause if the actual current shader compiler log contains it.
- Software skinning is already the architecture's deformation path, not a new toggle to enable.
- A blendshape-free GLB remains a useful controlled experiment, but it should follow proof that the frame loop is alive and that a simple unlit mesh can be submitted. Do not permanently delete the 52 channels based on the current telemetry.

### Vulkan / ANGLE

- The native renderer probes direct Vulkan first and falls back to EGL/OpenGL ES 3.1. Chrome's `chrome://flags/#use-angle` is not a control for this app's native Vulkan renderer and must not be presented as an app-level fix.
- A reported ANGLE string is meaningful only after identifying which backend the app actually initialized. Record the exact `RendererInfo.backend`, device, API and driver from the same running build.
- Swapchain recovery still needs explicit handling and real-device testing. The current Vulkan path marks OUT_OF_DATE/SUBOPTIMAL as resize conditions, but other acquire/submit/present failures are insufficiently surfaced. The in-flight fence failure case described above is a high-priority native follow-up.

## Avatar geometry, deformation, and GLB

- The supplied avatar metrics (about 417k triangles, 305k vertices, 71 joints, 52 ARKit channels) describe asset data, not proof of a successful draw.
- Existing host tests documented in STATUS cover exact REST restoration, repeatable clip playback, and non-empty deformation across all 52 channels. Those are engine tests, not Android GPU tests.
- The checked-in GLB validation/round-trip harness covers mesh topology, morph targets, textures and hostile inputs. It does not prove that the exact generated GLB renders on the S24.
- Requested stature is a specification value, not a measured mesh height. Device acceptance must measure actual exported bounds.
- PR #10's geometry changes narrow the head/jaw and reduce finger/thumb bulk. They are an incremental procedural geometry pass, not a replacement photorealistic model. CI passes, but visual acceptance remains open.

## Generation and image inputs

- Text generation is an on-device deterministic prompt-to-spec plus procedural geometry/UV/material/rig pipeline. It is not a remote diffusion model or a neural photorealistic identity reconstruction system.
- Image analysis samples pixels and extracts coarse framing/colour/material cues; it does not preserve face identity, reconstruct a true 3D face from photographs, project source pixels onto a likeness, or fuse calibrated multi-view images.
- The older 2026-10-18 audit described a 22/26/28-float image-analysis packing defect. The current `work/android-engine-dev` source has since aligned the SDK and native bridge at **28 floats**, uses the native stride check, and has sentinel packing tests. This older finding should not be repeated as a current confirmed defect.
- Generation acceptance still requires testing prompt input, one-image input, two-image input, cancellation/failure, export, reload, and a second generation in the same process. CI cannot replace these tests.

## UI and controls

- Current `work/android-engine-dev` menu has Builder, 3D Viewer, Render settings and System check. The self-test/diagnostics menu item exists on the separate `fix/restore-self-test-menu-phase4` branch; it is included in PR #12 so the integration branch carries it.
- The menu trigger is still text `☰ MENU`, not the requested gear/settings icon. This is a real UI follow-up, not a renderer root cause.
- Viewer panels expose camera, render settings, pose/clip, facial blendshape sliders, region inspection, and export/share/save-to. Their presence is source-confirmed; each callback still needs installed-device interaction tests.
- Export UI's “Save to app storage” action reports the already-created app-private file; “Save to…” invokes the Storage Access Framework; “Share” uses a FileProvider content URI. Verify each action and GLB byte count on-device.
- The app has no server/bridge menu because it is explicitly offline-first. Do not add dummy server buttons or hidden network dependencies.

## Branch audit and integration

The following changes have been integrated into the open PR #12 branch `fix/renderer-frame-loop-resize-recovery` on top of `work/android-engine-dev`:
- PR #11 diagnostics: invalid/zero telemetry is INCONCLUSIVE, not a successful recorded adaptation test.
- PR #9 self-test & diagnostics menu route.
- PR #10 head/hands procedural geometry pass.
- New SurfaceView resize-failure frame-loop retry.

Other branches need selective review rather than wholesale merging:
- `feat/avatar-realism-upgrade` and `feat/json-asset-registry` are heavily diverged from the active engine branch. The JSON schema/asset registry work is useful candidate material, but it must be rebased and tested against the current SDK before integration.
- `fix/avatarforge-overexposed-render` has no effective `AvatarRenderer.kt` content difference from the active branch in the inspected file, despite the branch name.
- `fix/renderer-recovery-2026-10-21` includes a recovery note and historical APK/AAR checksums, explicitly marked not device-verified. Those packages predate the current fixes and must not be represented as current builds.
- `main` is not the active engine branch: comparison shows it is divergent and does not contain the latest engine source changes. Do not merge a stale documentation-only change as if it integrates the engine.

## Required S24 acceptance run

Run the same test build on the Samsung Galaxy S24 with logcat/native diagnostics captured. Record pass/fail for each:

1. Launch, navigate to Builder, generate from text, and reach the viewer.
2. Verify surface create/change/destroy and rotation/re-entry; ensure frame-loop heartbeat continues after a forced/observed resize failure.
3. Confirm non-zero frame count, FPS, frame interval, and renderer-call duration. Confirm a visible unlit diagnostic mesh before the full avatar.
4. Confirm avatar draw counts, vertex/index buffer sizes, and successful Vulkan acquire/submit/present. If Vulkan fails, verify the GLES fallback reports its actual backend and shader logs.
5. Run the full avatar, then a no-morph GLB diagnostic, then add morphs back. Keep all 52 channels unless evidence isolates a specific incompatibility.
6. Exercise REST, T-pose, A-pose, all clips, each facial channel, reset-all, orbit/pinch, region picking/highlight, export, save-to, share, and re-import.
7. Generate from text, one photo, and two photos; verify honest UI wording, no crash, cancellation handling, repeated jobs, and valid exported GLB.
8. Confirm APK signature, package ID, arm64 native library, no INTERNET permission, and APK/AAR version/build SHA.
9. Capture a screenshot of the rendered model and record real FPS/frame time, process memory, free storage, and thermal state. Do not call the build green until installed-device runtime tests pass.

## Current gate

**CI build:** pass for PR #11; new combined PR #12 CI is pending.  
**Source-level frame-loop fix:** committed in PR #12.  
**Physical S24 rendering, swapchain recovery, image generation, and visual quality:** unverified.  
**No claim of complete recovery is made.**
