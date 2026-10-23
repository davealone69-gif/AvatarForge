# AvatarForge Project Checkpoint

Last updated: 2026-10-23
Repository: https://github.com/davealone69-gif/AvatarForge

## Resume here first

**Canonical working branch:** `work/android-engine-dev`  
**Protected destination:** `main`  
**Current rule:** Reuse existing work. Do not start a duplicate fix or create a new branch unless necessary. If a new branch is genuinely required, identify an obsolete branch to retire in the same workflow. Preserve unique commits before closing/deleting anything.

At the start of every new session:
1. Read this file and check the live branch heads, open PRs, recent commits, and CI results.
2. Verify the exact commit SHA before editing. This file is a checkpoint, not proof that the remote repository has not changed.
3. Compare the reported bug against current code and other branches before implementing anything.
4. Continue from the last incomplete task. Update this checkpoint after meaningful changes and before stopping.
5. Never claim a build is green unless required checks passed for the exact commit. Never claim device verification unless the APK was installed and tested on a physical device.
6. **Keep checking GitHub Actions quality/build results while working.** After each meaningful push, check Actions/checks for that exact head SHA; inspect failed or cancelled jobs/logs, fix failures on the existing working branch, push, and recheck the new SHA. Before merging any PR, confirm its current head SHA and required checks are green. Recheck open PRs and quality workflows when resuming. Do not treat a stale green run on an older SHA as proof for newer code.
7. Record the latest checked SHA, workflow/run URL, result, failures/fixes, and device-test status in this file. If a workflow is queued or running, leave it marked pending and check again before declaring the build done.

## Current objective

Consolidate already-existing AvatarForge fixes into a coherent Android-first renderer/engine line. Do not merge wholesale from diverged branches just because their names sound relevant. Inspect actual diffs, identify what is already present, selectively integrate unique valuable changes, resolve conflicts, and verify.

Priority order:
1. Renderer/frame-loop stability, EGL/Vulkan surface and swapchain recovery.
2. Android build and native regression checks.
3. Avatar geometry/lighting defects and existing app functionality.
4. Branch audit, integration, and safe retirement of redundant work.

## Perfetto tracing setup

- `986ea7b0fe6a8b16941d7d3cd61ef9d49a487c4a`: added Android trace slices around the render loop, camera submission, native renderer call, and telemetry collection in `RenderSurfaceView.kt`.
- `fac5ecbc2eaf0232efcc56cd3f790c30287f8c4a`: enabled shell profiling with `<profileable android:shell="true" />` in the app manifest.
- `7dc93a1b08cb031ad4cc0f4ad81ddee51e513d69`: added `docs/PERFETTO_PROFILING.md` with phone/Android Studio/ADB capture steps and interpretation guidance.
- These commits are on the existing `work/android-engine-dev` branch. Perfetto capture and build validation are not yet confirmed. Trace slices measure CPU-side sections, not proof of GPU completion or display.

## Latest known renderer commits

On `work/android-engine-dev`, prior session reported these commits:
- `1397b4c91391ffdfcf01b7ca206f57e604c11543`: GL render-target rollback logging and minimal-target fallback in `native/src/render/af_gl.cpp`.
- `d6f554533249d26b92247ee68df057631462a98c`: Kotlin render watchdog/recovery in `sdk/src/main/java/com/avatarforge/sdk/render/RenderSurfaceView.kt`.

Re-check the live branch head and source before assuming these commits remain current or sufficient.

## Verification truth and latest quality check

- Previous session reported Android Actions run #185 for `d6f554533249d26b92247ee68df057631462a98c`; its current status and relevance must be rechecked before relying on it.
- Latest live branch/PR inventory was checked on 2026-10-23. The canonical branch head and latest Actions run result still need to be confirmed in the next resume pass.
- **Latest checked commit SHA:** pending recheck.
- **Latest GitHub Actions run URL/status:** pending recheck.
- **Physical Samsung Galaxy S24 runtime verification:** OUTSTANDING unless a newer verified record proves otherwise.
- CI success is not installed-device verification. A successful renderer initialization is not proof of visible frames. Zero FPS/zero triangles is inconclusive/failure evidence, not a pass.
- Do not label the app production-ready without required CI checks and installed-device runtime verification.

## Known open PRs at last checkpoint

Re-check state and head SHAs on resume:
- [#4 Android engine/rendering update](https://github.com/davealone69-gif/AvatarForge/pull/4), `work/android-engine-dev` → `main`.
- [#8 latest Dropbox ZIP fixes](https://github.com/davealone69-gif/AvatarForge/pull/8), targeting `work/android-engine-dev`.
- [#10 avatar head/hands geometry pass](https://github.com/davealone69-gif/AvatarForge/pull/10).
- [#11 invalid zero-frame diagnostics](https://github.com/davealone69-gif/AvatarForge/pull/11).
- [#12 renderer frame-loop/resize recovery](https://github.com/davealone69-gif/AvatarForge/pull/12).
- [#13 audited branch integration](https://github.com/davealone69-gif/AvatarForge/pull/13).
- [#14 Vulkan frame/swapchain recovery](https://github.com/davealone69-gif/AvatarForge/pull/14).
- [#15 Android/static-analysis quality gates](https://github.com/davealone69-gif/AvatarForge/pull/15).

Prior session closed PRs #7 and #9 without merging; that did **not** delete their branches. Verify their current state. PR #1 closure was not confirmed. Do not infer branch deletion from PR closure.

## Branch audit inventory

Last observed branch list:
- `chore/apply-dropbox-update-2026-10-21`
- `chore/quality-gates-ai-review`
- `feat/avatar-realism-upgrade`
- `feat/json-asset-registry`
- `feat/repo-wide-engine-consolidation`
- `fix/avatar-head-hands-geometry-pass1`
- `fix/avatarforge-overexposed-render`
- `fix/diagnostics-invalid-render-telemetry`
- `fix/renderer-frame-loop-resize-recovery`
- `fix/renderer-recovery-2026-10-21`
- `fix/restore-self-test-menu-phase4`
- `fix/vulkan-frame-recovery`
- `integrate/audited-branch-work`
- `main`
- `work/android-engine-dev`

Earlier comparisons showed significant divergence and unique work on several branches, especially `fix/vulkan-frame-recovery`, `fix/renderer-frame-loop-resize-recovery`, `fix/diagnostics-invalid-render-telemetry`, `fix/avatar-head-hands-geometry-pass1`, `chore/apply-dropbox-update-2026-10-21`, and `integrate/audited-branch-work`. Do **not** delete these blindly. Compare each against the canonical branch and inspect per-file patches/commit history. A branch may contain a fix already applied in another form, plus other unique fixes.

Branch deletion capability was not available through the GitHub connector in the prior session. Do not claim a branch was deleted unless the remote branch is confirmed absent.

## Work log

- 2026-10-23: Created this persistent checkpoint so a new chat can resume from repository state instead of relying on conversation memory.
- Previous work: renderer GL rollback logging/minimal-target fallback and Kotlin zero-frame watchdog were committed to `work/android-engine-dev` (SHAs above).
- Previous work: PR #7 and #9 were closed without merging; branches were preserved. Branch audit/consolidation remains incomplete.
- 2026-10-23: Added an explicit ongoing GitHub Actions quality/build monitoring rule: check exact-SHA results after pushes, inspect failures, fix and rerun, recheck PR heads before merge, and log the last verified SHA/run/device status. Current live build status has not yet been confirmed in this update.
- 2026-10-23: Set up Perfetto source instrumentation and profiling instructions on `work/android-engine-dev`. Exact commits are listed under Perfetto tracing setup. Build and device trace remain pending; check Actions for the latest branch head before treating this as build-verified.

## Next exact task

1. Check the current `work/android-engine-dev` HEAD and live Actions runs/checks for that exact SHA. Also inspect the relevant open PR heads, especially renderer/Vulkan PRs.
2. If a build is running, wait/check again; if failed, inspect job logs, fix on the existing branch, push, and verify the new SHA.
3. Resume the branch audit. For each branch, compare against `work/android-engine-dev`, identify changes already present vs unique changes, and record a disposition: **already integrated**, **selectively port**, **keep for later**, or **safe to retire**.
4. Do not close/delete a branch with unique unpreserved work. Run checks on the exact resulting commit and update this file.
