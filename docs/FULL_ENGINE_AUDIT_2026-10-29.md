# Full AvatarForge engineering audit — 2026-10-29

## Scope and evidence rules

Repository: `davealone69-gif/AvatarForge`
Audit branch: `fix/avatarforge-visual-quality-performance-2026-10-29`
Audited source head before this report: `fd1e34d014e7c42a43190902f985d57052b8b97b`
Integration base: `work/android-engine-dev`
Stable default branch: `main` (must remain untouched by this audit).

This is an active audit record, not a claim that the project is production-ready. Findings below are source-level unless a CI run is explicitly named. Physical Samsung S24 execution remains unverified.

## Confirmed current CI

Workflow: [Android APK run 40862091892](https://github.com/davealone69-gif/AvatarForge/actions/runs/40862091892)
Head SHA: `fd1e34d014e7c42a43190902f985d57052b8b97b`

At last inspection:
- GLB importer ASan/UBSan fuzz job: **PASS**.
- Headless GLES and Vulkan render/pixel/synchronization regression job: **PASS**.
- Android package build/host regression job: **RUNNING**.
- Overall workflow: **PENDING**, not green until all jobs finish.

The headless GLES/Vulkan job verifies the host fixtures and driver paths on the CI runner. It does not verify the Samsung phone, the exact generated 426k-triangle avatar, or its visual quality on Xclipse/ANGLE.

## User's current visual/performance baseline

The 2026-10-29 screenshot shows a visible avatar with 426,170 triangles, 316,358 vertices, 71 joints, 52 ARKit blendshapes, 37.3 MB GLB, and a reported 29 FPS / 34.2 ms frame interval. The backend string is `gles31 · ANGLE (Samsung Xclipse 940) on Vulkan 1.3.304`. The capture shows the region inspector but no selected region.

This is one user-observed session, not a sustained benchmark. It does not tie the screenshot to an APK SHA or prove surface recovery, export, animation stability, thermal stability, or a completed quality downshift.

## Findings confirmed by source inspection

### F-01 — Renderer health is correctly stricter than initialization, but device evidence is still missing
- Kotlin `RenderLoopPolicy` requires valid positive FPS, a progressing accepted-frame sequence, no fatal state, and a non-negative Vulkan result.
- GLES does not publish a new frame counter after failed `eglSwapBuffers`; Vulkan distinguishes attempts, submissions, accepted present requests, and fatal results.
- Host tests cover swap failure and recovery. These are good safeguards but are not equivalent to visible-pixel proof on the target phone.
- Disposition: retain; verify exact-head CI and installed-device behavior.

### F-02 — AUTO changes post-processing costs, not mesh complexity
- The native `BudgetRenderer` tier ladder changes MSAA, shadow-map resolution, and bloom levels.
- A separate culler/LOD system attempts to select generated index-only LODs and has a 50,000-triangle mobile target.
- If LOD generation fails or no generated level reaches the target, the culler deliberately chooses the coarsest valid level and can report that the target was exceeded. AUTO quality cannot compensate for an unavailable/coarse LOD stream.
- The screenshot's build summary alone does not reveal the actual selected LOD or visible triangle count. Verify the actual runtime counters and log.
- Disposition: investigate with the exact generated avatar and add observable LOD outcome evidence before claiming the 30-FPS target is achievable.

### F-03 — Vulkan currently serializes frame progress to protect shared mapped buffers
- In `native/src/render/af_vk.cpp`, each frame calls `vkWaitForFences` with every in-flight fence and `VK_TRUE` before acquiring the next swapchain image.
- The source comment explains why: camera data is per-frame, while geometry, material, post-process, and Forward+ buffers are shared and updated by the CPU.
- This avoids CPU writes racing GPU reads, but it also removes most benefit from multiple frames in flight and can force the CPU to wait for GPU completion every frame.
- Disposition: **confirmed performance concern, not yet repaired**. A safe fix requires per-frame/ring-buffer ownership for every mapped resource that can be written while previous frames read it, descriptor/range updates, resize/mesh-rebuild handling, and synchronization regression coverage. Do not simply delete the global fence wait; that would risk data races and corrupted rendering.

### F-04 — LOD construction happens during renderer engine attachment
- Both Vulkan and GLES renderer paths own an `AvatarLodSet` and build index-only LOD streams during engine attachment, using rest vertices where available.
- This preserves the original skinned vertex stream and avoids changing rig weights, but the CPU time and temporary memory peak of LOD generation are not currently proven for the user's exact avatar.
- Disposition: profile attach duration, peak memory, generated index counts per level, and whether the 50k triangle target is reached. Preserve fallback safety if LOD generation cannot complete.

### F-05 — Surface and renderer recovery are bounded
- The Kotlin render thread retries renderer creation while the Surface is valid, destroys the renderer before Surface invalidation, and performs one automatic restart after a detected stall per Surface lifetime.
- Failed resize discards the renderer and retries at a lower cadence rather than permanently removing the frame callback.
- A repeated stall after that one restart remains an error requiring lifecycle/device evidence. Do not call recovery fully verified based on source or headless tests alone.

### F-06 — Visual surface defects are not yet attributable
- The screenshot includes visibly uneven/dark surface patches, but the image alone cannot distinguish authored/generated texture content, UV/atlas projection, normals/tangents, material parameters, skinning, or lighting.
- No shader/material changes should be made until the exact GLB is inspected and repeatable screenshots are captured at fixed camera/light settings.
- Disposition: request/locate the exact GLB and capture its material/texture inventory, UVs, normals/tangents and submesh/region mapping. Do not label the defects a shader bug without that evidence.

## Repair order

1. Finish and record the exact-head Android workflow for `fd1e34d014e7c42a43190902f985d57052b8b97b`.
2. Audit every open renderer-recovery/diagnostics PR and relevant branch against `work/android-engine-dev`; port only unique fixes after diff review.
3. Add runtime-observable selected LOD / source-to-selected triangle counts and LOD build failure reasons, with tests and no fake defaults.
4. Profile the exact avatar's LOD build time/peak memory and selected LOD; optimize only from measured bottlenecks.
5. Design and test Vulkan per-frame resource ownership to remove global frame serialization safely. Do not remove synchronization without resource lifetime proof.
6. Reproduce and trace the screenshot's visual artifacts from the exact GLB before changing material or shader code.
7. Exercise the full lifecycle matrix on device: open, model attach/re-attach, pose/expression, resize/rotation, background/foreground, low-memory pressure, quality transitions, repeated stalls, export/share, and sustained thermal run.
8. Build a source-linked APK; record SHA, workflow, artifact URL, package/signature/ABI. Install and test that exact APK on the Samsung phone.

## Current status

- Audit branch and source changes: **REAL + VERIFIED** on GitHub.
- Importer sanitizer and headless GLES pixel jobs: passed on earlier branch revisions; the exact final head must pass again.
- Android package build/full workflow: **PENDING/UNVERIFIED** for the final head.
- Native Vulkan backend: removed from this branch; ANGLE may still use Vulkan internally.
- Exact avatar LOD selection/50,000-triangle target attainment: **UNVERIFIED**; new telemetry now reports the selected level and whether the coarsest level still exceeds budget.
- Visual artifact root cause: **UNVERIFIED**; no material/shader change is accepted without repeatable captures.
- Installed Samsung S24 runtime on this branch: **UNVERIFIED**.
- Release signing: **UNVERIFIED** until artifact and signature are inspected.

## Guardrails

Never merge this branch or claim a green build until the exact head SHA has completed required checks. Never claim the app is fixed or production-ready until the exact produced APK has been installed and exercised on the physical Samsung phone. Preserve `main`; keep this work isolated until it earns promotion.

## Superseding decision — native Vulkan removed (2026-10-29)

The earlier F-03 finding about Vulkan serializing in-flight fences is retained as historical evidence but is no longer a planned repair for this branch. The user explicitly requested removing Vulkan. The branch now removes the native Vulkan implementation, its SPIR-V table, Vulkan-only host test/script, CMake link dependency, Kotlin preference UI, JNI/C ABI option, Rust option, and CI validation steps. The active renderer is GLES 3.1 only; camera and shadow projection math now uses OpenGL depth range only.

See [GLES-only migration record](GLES_ONLY_MIGRATION_2026-10-29.md). Exact-head build, final GLES pixel regression, artifact dependency inspection, and physical Samsung runtime remain **UNVERIFIED/PENDING**. Android may still route GLES through a system-selected ANGLE build that uses Vulkan internally; ordinary app code cannot reliably force a different system graphics driver. Do not claim zero Vulkan anywhere in the Android graphics stack until the actual device driver path is verified.