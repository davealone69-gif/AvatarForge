# AvatarForge visual quality and performance pass — 2026-10-29

## Branch and safety

- Working branch: `fix/avatarforge-visual-quality-performance-2026-10-29`
- Base: `work/android-engine-dev`
- `main` is unchanged.
- This is an isolated investigation. Do not merge or promote based on this note alone.
- Do not change renderer backend selection or shader/material defaults until a repeatable baseline exists.

## User-provided device baseline

The screenshot supplied on 2026-10-29 shows:

- Avatar label: female, 1.78 m, armor plate sabaton.
- Mesh counters: 426,170 triangles, 316,358 vertices, 71 joints, 52 ARKit blendshapes.
- GLB size: 37.3 MB; build time: 67 seconds.
- Backend label: `gles31 · ANGLE (Samsung Xclipse 940) on Vulkan 1.3.304`.
- Displayed cadence: 29 FPS / 34.2 ms, with AUTO quality warning.
- Region inspector is visible; no region was selected at capture time.

Evidence classification: **REAL + USER-OBSERVED** for this displayed session only. The screenshot does not identify the installed APK's source SHA, prove sustained FPS, prove a completed AUTO downshift, or establish export/animation/recovery stability.

## Source-level findings

- The renderer has Vulkan-first selection with a GLES 3.1 fallback. The user's device label indicates the GLES/ANGLE path was active for this captured session; it is not evidence that native Vulkan was the active renderer.
- AUTO quality is a four-tier controller. It starts at the MEDIUM tier, measures frame cadence, steps down after sustained slow frames, and upgrades only after a much longer headroom window. Device capability profiles clamp the available tier and settings.
- The tier ladder primarily changes MSAA, shadow-map size, and bloom levels. The avatar model and its geometry are not silently changed by the controller.
- The UI exposes the effective renderer statistics and a real region-picking flow.
- Existing renderer tests exercise native/software-driver correctness, but those tests are not a sustained Samsung-device performance benchmark.

## Priority order

1. Preserve the currently visible avatar and capture a source/build-linked baseline before renderer changes.
2. Confirm whether the AUTO controller actually downshifts on the target phone and record the resulting tier, frame cadence, CPU renderer-call time, thermal state, and texture-atlas estimate.
3. Reproduce visual surface artifacts from the same GLB at fixed camera angles and lighting. Inspect generated mesh normals/tangents, UVs, material/normal maps, and atlas contents before changing shaders.
4. Verify the region inspector against face, torso, limbs, hands, feet, eyes, and overlapping surfaces. Record both the picked region ID and visible highlight.
5. Run lifecycle checks: open viewer, rotate/resize, background/foreground, recreate surface, change pose/expression, and reattach a newly generated model.
6. Run a sustained performance/thermal observation only after correctness is stable.

## Acceptance gates

- **Build:** Android package and native regression workflows pass for the exact branch head; record the SHA and workflow URL.
- **Renderer correctness:** no black frame, fatal Vulkan state, stale positive FPS after failed presentation, or invalid frame-sequence progression.
- **Performance:** report measured values and test duration. Do not claim a 30 FPS floor from a short screenshot.
- **Visual quality:** compare repeatable captures. No shader/material fix is accepted solely because it sounds plausible.
- **Device runtime:** install and test the exact produced APK on the Samsung phone. Until that happens, runtime remains **UNVERIFIED**.
- **Release signing:** do not claim a signed release unless the produced artifact's signature is checked with the intended release key.

## Current status

- Branch creation: **REAL + VERIFIED** by GitHub branch creation response.
- User screenshot baseline: **REAL + USER-OBSERVED**.
- Source-level controller/renderer review: **PARTIAL**.
- Changes to rendering algorithms: **NOT YET MADE**. Avoid speculative edits until the visual artifact and AUTO-tier transition are reproducible.
- Build on this new branch: **UNVERIFIED**.
- Installed-device test of a build from this branch: **UNVERIFIED**.
