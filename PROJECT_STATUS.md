# AvatarForge Project Checkpoint

Last updated: 2026-10-24
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

## Black-frame repair checkpoint — 2026-10-24

**Read this newer checkpoint before the historical review notes below.** The user selected
black/blank viewer or 0 FPS, then explicitly selected the latest `work/android-engine-dev`, not
`main` and not the earlier `21c1bc2` ancestor. The canonical branch was checked again and remained
at `9ec715723bfa0cccdf31c78e197fd365f2818908`.

- Reproduced the unfixed branch with the new driver harness: GLES initialization emits `0x0502`;
  Vulkan fails shader/descriptor/pipeline validation and attempts a disabled-bloom pipeline with a
  null render pass. The new harness was also run against an isolated archive of the exact baseline.
- Repaired real GLES/Vulkan initialization, correctly rounded UBO slices, matching depth samplers,
  Vulkan 1.0 shader generation, valid descriptors/dynamic offsets, single-sample depth clearing,
  transfer/render-pass synchronization and safe shared-buffer ownership. Disabled shadow targets
  are cleared/transitioned rather than sampled in UNDEFINED layout.
- Fallback/1x1 textures are non-anisotropic, and disabled material maps no longer use arithmetic
  `mix(..., possiblyNaN, 0)`. Reattaching a textured mesh followed by an untextured one remains visible.
- Inspected PR #16 at `0e5f772d7608a7323f33f7143d56d07c22c9eb59`. Selectively adapted its host-intent /
  effective-visibility separation instead of merging the diverged branch wholesale. Added attach/
  detach scheduling guards, deduplicated retries, recovery after creation/resize failure, and
  fresh native sequence-based health checks so stale FPS cannot clear a recovery error.
- Existing LOD/culling, GPU profiles, codecs, engine leases, thermal/performance hints and Vulkan
  surface/swapchain recovery were retained. No branches/PRs closed or deleted. No third-party
  rendering engine was added. Checked-in historical APKs/prebuilt libraries are NOT refreshed;
  the new APK is built from current native code and delivered separately.
- **Verified locally on the repaired sources:** full native harness **468/0**; real GLES driver
  **270/0**; real Vulkan driver **88/0 with zero validation errors**; all **13** baked SPIR-V modules
  validate for Vulkan 1.0; SDK JVM **26/0** including **8** new lifecycle/health policy tests; ARM64
  NDK libraries + debug APK + release SDK AAR build; C ABI **47/47**; APK signature, offline permission
  and native ELF Build-ID checks. Headless pixels are fixture geometry, not an installed-avatar test.
- Added a renderer-pixel CI job and made Android packaging depend on it. The repair was committed as
  `1e6d647321b64836e5196259278dc681fab703f7` and pushed to this existing canonical branch after the
  user authorized GitHub access. Its tree `d18b345d71b7b9da99df575b169415b40031a1bc` matches the locally
  verified source exactly. No merge into `main` occurred.
- GitHub started [Android APK run 40055587416](https://github.com/davealone69-gif/AvatarForge/actions/runs/40055587416)
  for code commit `1e6d647`; it was **in progress**, not green, when checked. This checkpoint follow-up
  changes only documentation and will trigger a newer run. Check the exact latest remote HEAD and
  its jobs before claiming CI success; the earlier run may be superseded by branch concurrency.
- Physical S24 runtime verification remains **UNVERIFIED**. Do not claim production readiness or
  hardware FPS. GitHub sign-in credentials are not part of the repository or delivery.

See [`docs/RENDERER_BLACK_FRAME_FIX.md`](docs/RENDERER_BLACK_FRAME_FIX.md) for root causes, test
coverage, reproducible commands, scope limits and the safety-first shared-buffer fence tradeoff.
The session delivery contains a source patch, an ARM64 debug test APK, an SDK AAR, checksums and
before/after logs. Neither the existing tracked `prebuilt/` nor the old APKs in `artifacts/` should be
used as this repair's binary evidence.

**Next steps for this repair:** verify the pixel and Android jobs for the exact latest branch SHA
(including this documentation follow-up). If a job fails, fix it on this same branch, push and recheck
the new SHA. Then install the newly produced APK and complete the physical-device matrix in the repair note.
The broader branch audit below remains separate unfinished work.

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
- **Latest checked remote baseline SHA:** `9ec715723bfa0cccdf31c78e197fd365f2818908` on `work/android-engine-dev`. The renderer repair code commit `1e6d647321b64836e5196259278dc681fab703f7` has now been pushed; baseline CI does not cover that repair or this documentation follow-up.
- **Latest checked baseline GitHub Actions run:** [39998014453](https://github.com/davealone69-gif/AvatarForge/actions/runs/39998014453), completed/success for unmodified `9ec7157`. **Modified-source CI: STARTED for `1e6d647`, [run 40055587416](https://github.com/davealone69-gif/AvatarForge/actions/runs/40055587416), in progress when checked. Recheck the newer documentation-follow-up HEAD/run; no green claim yet.**
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

## AvatarForge renderer review notes — 2026-10-24

### User-visible symptoms and primary diagnosis target

- The most important unresolved symptom is the renderer self-test reporting **0 FPS, 0 draw calls, and 0 triangles/vertices** during a 15-second sample, despite the GPU surface/backend appearing initialized.
- Treat this as **invalid/inconclusive rendering evidence**, not as a performance result and not as proof the adaptive-quality controller works.
- A second reported symptom is the avatar disappearing after sustained low frame rates and returning after leaving/re-entering the app.
- The key investigation is to trace the complete frame path: model loaded and retained → visible scene/geometry selected → vertex/index data available → draw commands recorded → native submission attempted → queue submission accepted → presentation accepted → valid frame and geometry telemetry. Log each stage distinctly; do not equate accepted presentation with GPU completion or proof of visible pixels.
- Check the Kotlin render-loop callback lifecycle, resize/surface failure handling, native renderer return values, buffer/target creation, shader/pipeline failures, and Vulkan fence/semaphore/swapchain recovery. Ensure failure cannot silently cancel the only frame callback forever.
- The self-test must return `INCONCLUSIVE` when no valid frame/timing sample exists. An initialized GPU backend is not equivalent to valid telemetry or a visible avatar.

### Relevant pull requests observed during this review

These PRs were returned by the live GitHub PR search on 2026-10-24. This is a search snapshot, not a fresh verification of their current head SHAs or CI status. Recheck each PR and Actions before relying on it.

- [PR #12 — Recover renderer frame loop after transient surface resize failure](https://github.com/davealone69-gif/AvatarForge/pull/12): recorded changes keep the visible frame loop alive after transient resize failure, retain errors until a successful frame, restore self-test/diagnostics menu, and improve Vulkan failure recovery. Its description explicitly says latest native commits require CI rechecking and physical S24 rendering remains unverified.
- [PR #11 — Reject invalid zero-frame renderer evidence](https://github.com/davealone69-gif/AvatarForge/pull/11): changes zero-only test samples to `INCONCLUSIVE` and distinguishes initialized GPU backend from valid frame telemetry. This improves diagnostic honesty but does not repair the underlying renderer.
- [PR #14 — Fix Vulkan frame and swapchain recovery](https://github.com/davealone69-gif/AvatarForge/pull/14): records checks for fence waits, image acquisition, command-buffer operations, queue submission, presentation, surface recreation and swapchain recovery; adds attempt/submission/present counters, last VkResult and fatal-state logging. The PR description reported its Android workflow pending at the time observed. No physical-device verification was claimed.
- [PR #13 — Integrate audited branch work before renderer verification](https://github.com/davealone69-gif/AvatarForge/pull/13): selectively integrates asset registry/schema and renderer recovery documentation; does not claim all assets are wired into UI or S24 verification.
- [PR #10 — Fix avatar head silhouette and hand proportions (pass 1)](https://github.com/davealone69-gif/AvatarForge/pull/10): incremental procedural geometry edits only; not a full replacement model and not visually device-verified.
- [PR #15 — Add Android and static-analysis quality gates](https://github.com/davealone69-gif/AvatarForge/pull/15): proposed Android unit tests, lint, debug APK artifact and scheduled/change-triggered Semgrep checks. Its description said workflows had not run yet at the time observed; do not call the gate green without checking.
- [PR #4 — Apply AvatarForge Android engine and rendering update](https://github.com/davealone69-gif/AvatarForge/pull/4): proposed promotion of the engine branch to main, including Android lifecycle, renderer/GPU profiles, culling/LOD, texture compression and tests. CI must pass before merge; S24 runtime remained outstanding in its description.
- [PR #8 — Apply latest Dropbox ZIP fixes to Android engine branch](https://github.com/davealone69-gif/AvatarForge/pull/8): earlier integration target; inspect actual current diff and CI before deciding whether it is redundant or still has unique work.

### Repository status document and verification caveats

The live `main` branch `STATUS.md` and `README.md` fetched on 2026-10-24 describe an earlier 0.10.1 build/test pass. They report host harness checks, Kotlin/JVM tests, Rust checks, Android native builds, debug/release APK builds, shader validation, and static APK checks. Those are historical repository claims and do **not** establish that the latest renderer PR heads build or that the renderer works on the S24. The status document itself says device/GPU execution remains outstanding. It also reports a Rust end-to-end test assertion still red and that the release APK uses a fallback debug certificate if no release keystore is supplied.

The README links to a [renderer recovery note](https://github.com/davealone69-gif/AvatarForge/blob/main/docs/RENDERER_RECOVERY_2026-10-21.md). It describes a retest candidate, not a verified fix. It documents checksums for the APK/AAR from a supplied bundle; verify actual downloaded files against those checksums before installation. It states that GitHub Release binary assets were missing at the time that note was written.

### Other outstanding functional/quality items

- The incremental head/hands geometry changes are not a complete realistic replacement. Inspect actual mesh generation and the viewer/export path before claiming the model has been replaced.
- The previously reported thermal postponement uses threshold level 3 (Severe) or higher in the remembered design: 0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency, 6 shutdown. Verify the actual current source and avoid changing the safety threshold without reviewing thermal behavior and generation reliability.
- Verify that the self-test/diagnostics menu is present and usable in the current app; PR #12 says it restores it, but source/installed-device verification is still required.
- Verify that asset registry/schema entries are connected to real UI and engine paths before describing them as implemented features.
- The project’s desired rendering backends are custom Vulkan primary and OpenGL ES 3.1 fallback. Rust and Kotlin are intended to interface with the same native C ABI. Do not introduce a third-party engine or assert both backends work on hardware without evidence.

### Status labels at this checkpoint

- **PARTIAL:** code-level renderer recovery changes exist in PRs and prior branch commits.
- **UNVERIFIED:** current PR heads and their latest Actions results have not been rechecked in this note; latest build status for exact SHAs is not established.
- **UNVERIFIED:** installed-device rendering, live non-zero frame telemetry, actual avatar visibility, generation, export, resize/background recovery and thermal soak on the Samsung Galaxy S24.
- **MISSING from this review:** a current exact-head Actions/run URL + result and a physical-device test report proving visible avatar geometry and valid frame telemetry.

### Next actions, in order

1. Fetch current `work/android-engine-dev` HEAD SHA, current PR #11/#12/#14 head SHAs, and the latest Actions/check runs for those exact SHAs. Inspect failed/cancelled job logs; do not rely on stale runs.
2. Inspect the current source around `RenderSurfaceView`, `AvatarRenderer.resize()`, native frame recording/submission/presentation and telemetry counters. Confirm whether the proposed fixes are actually present on the canonical branch or only on PR branches.
3. Run the relevant host/native tests and Android build against the exact chosen commit. Record exact command, SHA, workflow URL and outcome.
4. Publish a direct, verified APK artifact link from a successful workflow/release. Do not invent a link or claim a binary is the latest build if its commit cannot be tied to the source.
5. On the S24, install that exact APK and test visible geometry/non-zero frames, initial surface creation, resize/rotation, background/foreground, renderer restart, real generation, GLB export/import, airplane-mode behavior and a sustained thermal/performance soak. Record measured results and logs.
6. Only then classify the renderer fix as **REAL + VERIFIED**. Until then, keep the device status **UNVERIFIED** and do not merge or announce production readiness prematurely.

## 2026-10-24 audit remediation on `work/android-engine-dev`

- Latest upstream base integrated before this remediation: `fff28e3f6f99b4774a8b9103445a2fa01d95094d` (includes GLB fuzzing, hardened PNG limits/CRC validation, and removal of the incomplete `.glb` open intent filter). Local fixes are currently uncommitted pending commit/push; no CI result is claimed for these edits yet.
- Fixed the Rust binding's `af_texture::Default` by using a null pointer, added a regression test, repaired the renderer feature's duplicate `Camera::default` implementation and missing engine handle accessor, and tied an attached Rust renderer's lifetime to its borrowed engine. CI now installs Rust and `tools/verify.sh` tests the full workspace and checks every feature/target.
- Fixed animation duration metadata reads so the Compose animation picker can query immutable clip data while the renderer holds its engine lease.
- Hardened GLB accessor range arithmetic and sparse buffer ranges; GLB index accessors now decode unsigned integers directly rather than round-tripping through `float`. Added regression cases for overflowing accessor stride, `UINT32` out-of-range indices, and sparse-only unsigned indices. Live import now clears stale texture slots on a successful model swap and stages several large allocations before mutating the engine.
- Preserved upstream PNG chunk/zlib validation and hardening, including the compressed-data and peak-memory budgets; restored packed-row decoding, grayscale scaling, and standard grayscale/RGB `tRNS` keys with regression fixtures. Kept the upstream removal of the unsupported `.glb` open intent filter rather than advertising an import path the app does not implement.
- Local verification on the current tree after integrating upstream `fff28e3`: `bash tools/verify.sh` passed all 536 native host checks, shader freshness, GLB structural validation, C++ ABI conformance (47/47), Rust workspace tests, all-feature/all-target Rust compilation, Rust-core ABI parity for the 15 implemented symbols, and prompt parity (72/72). Rust workspace tests reported 38 passed, 2 intentionally ignored. `aapt2` was unavailable, so APK manifest/static checks were skipped.
- Android Gradle/JVM verification did not complete locally: dependency resolution failed before compilation due Maven Central TLS handshake failure. Current APK/AAR files in `artifacts/` predate these edits. Re-run Android CI against the pushed exact SHA before claiming Android package success.
- Exact-head GitHub CI for these local changes: **PENDING push/run**. The repository now has an ASan/UBSan libFuzzer workflow, but local fuzz execution was unavailable because `clang` is not installed in this environment. Physical Galaxy S24 runtime, visible-frame, FPS, thermal soak, and device import verification remain **UNVERIFIED**.


## 2026-10-24 user-device report and branch triage checkpoint

### Latest user-provided on-device evidence

- The user supplied a screenshot of the AvatarForge viewer showing: **Vulkan**, Samsung Xclipse 940 label, **58 FPS**, **17.2 ms**, **417,150 triangles on screen**, **304,797 vertices**, **71 joints**, **52 ARKit blendshapes**, **37.5 MB GLB**, and **18 s to build**. The overlay says "on target".
- Classification: **USER-REPORTED DEVICE EVIDENCE**. The screenshot is evidence that the pictured session displayed the avatar and non-zero geometry/FPS counters. It is not a sustained performance test, a source/build-to-SHA match, or proof of export/animation stability. Do not turn it into a general "green build" claim.
- Visual quality is still visibly imperfect in the supplied capture: strong rippling/striping across torso and legs, blocky-looking hands, and an unclear head/neck transition. Diagnose materials/UVs/normals/tangents and geometry from source/asset evidence; do not guess the cause from the screenshot alone.
- The user reports that an in-app DIY fault-finding and manufacturer-diagnosis flow is now built into AvatarForge. The repository PR #19 diff also shows an in-app Diagnostics dialog/panel, live renderer diagnostics, actionable WARN/FAIL hints, and a copy-report action. **Code presence: observed in PR #19 diff; installed-app interaction: not independently verified in this audit.** Preserve this workflow and make it the first-line troubleshooting path. The existing `docs/DIY_FAULT_FINDING.md` remains the longer reference checklist.
- The user is happy that the picture is back. Do not disturb the working path with speculative renderer changes. Capture the current visual/FPS baseline before changing rendering, material, geometry, or quality logic.

### Branch triage (live GitHub snapshot, 2026-10-24)

Canonical integration/work branch remains `work/android-engine-dev`; `main` remains protected from wholesale promotion until exact-head CI and selective integration are reviewed. The repository default branch is `main`; GitHub currently grants push/admin permission, but permission is not a reason to force-update history.

The live branch list contained 18 refs. Comparisons against `work/android-engine-dev` show substantial divergence for most feature/fix branches. These are comparison counts, not proof that a branch's changes are missing: shared patches may have been cherry-picked or independently reimplemented. Review diffs and tests before integration.

- **Primary review queue:** `arena/985042e3-avatarforge` (PR #19, open, reported mergeable) contains the latest renderer startup/fallback/diagnostics changes. Its PR head is `bac2168a663858cb0e8341c5fce00f3d93dfca96`; base is `main`. Review the full diff and exact-head Actions, then selectively integrate only after tests pass. Do not merge based only on "mergeable".
- **Renderer recovery lineage:** `fix/vulkan-frame-recovery`, `fix/renderer-frame-loop-resize-recovery`, `fix/render-loop-visibility-watchdog`, `fix/renderer-recovery-2026-10-21`, `fix/diagnostics-invalid-render-telemetry`, and `fix/restore-self-test-menu-phase4`. Compare each against the primary branch and mark each fix as present/missing/partial before closing or retiring anything.
- **Avatar quality:** `fix/avatar-head-hands-geometry-pass1`, `fix/avatarforge-overexposed-render`, and `feat/avatar-realism-upgrade`. Preserve unique geometry/material work; screenshot artifacts alone do not identify the root cause.
- **Assets/schema:** `feat/json-asset-registry`. The prior audit indicates selective integration was preferred over wholesale merging. Confirm the actual current branch content and UI/engine wiring.
- **Quality gates:** `chore/restore-code-quality-gates`, `chore/quality-gates-ai-review`. Check exact workflows and avoid duplicate/conflicting quality jobs. User preference remains no Qodana, CodeQL, detekt, or ktlintCheck.
- **Dropbox import:** `chore/apply-dropbox-update-2026-10-21`. Compare actual file-level changes and retain unique fixes.
- **Integration/audit:** `integrate/audited-branch-work`; inspect for patches already included in the canonical branch before taking any changes.
- **Likely stale candidate:** `feat/repo-wide-engine-consolidation` compares as behind with no unique commits relative to the canonical branch in the current compare result. Keep it until its head and references are rechecked; only then consider retirement.
- **Historical baseline:** `main` is significantly behind/diverged from the working branch in the current compare snapshot. Do not force-push or reset it. Keep it as the stable/default branch until a reviewed PR passes.
- **Other ref:** `arena/985042e3-avatarforge` is the PR #19 head and must be treated as active, not a cleanup target.

### Immediate branch-sorting procedure

1. Record exact HEAD SHA and current CI for `work/android-engine-dev` and every open PR before making any history changes.
2. Use PR #19 as the candidate for review because it includes the in-app diagnostics path and renderer startup fixes; inspect the full patch for regressions and check the exact head's workflow status.
3. Compare each branch's unique file patches against the candidate and canonical branch. Label each item **already integrated**, **selectively port**, **keep active**, **superseded**, or **safe to retire after confirmation**.
4. Merge only reviewed PRs with successful exact-head CI. Do not bulk-merge divergent branches, force-update `main`, or delete any branch containing unpreserved work.
5. After integration, rerun repository verification and Android package builds. Installed-device runtime verification remains a separate gate; the user screenshot is a positive runtime observation but is not tied to a verified current Git SHA/APK.
6. Once unique work is preserved and PR references are checked, close duplicate/superseded PRs with a clear explanation. Delete obsolete refs only after explicit confirmation that they contain no unique unpreserved work.

### Status labels

- **REAL + USER-OBSERVED:** avatar visible in the user-provided screenshot with non-zero displayed FPS and geometry counters.
- **PARTIAL:** in-app DIY/Diagnostics source is visible in PR #19 and the user reports it is built into the app.
- **UNVERIFIED:** installed version/SHA match, diagnostics copy/report action, long-duration FPS, renderer recovery under resize/background transitions, animation, export/import, and exact-head CI for the current branch/PRs.
- **NOT DONE:** no branches were merged, force-updated, or deleted during this triage snapshot. Branch sorting must be completed by selective diff review and exact-head CI, not by deleting refs based only on age or ahead/behind counts.


## SDK transplant audit checkpoint — 2026-10-24

Source-level audit recorded in [`docs/SDK_TRANSPLANT_AUDIT_2026-10-24.md`](docs/SDK_TRANSPLANT_AUDIT_2026-10-24.md), commit `49f3c609f09d41a7a008fc973655880a86a73b32`.

- Confirmed `:sdk` Android library, public Kotlin façade, renderer wrapper/SurfaceView, CMake native-library path, JNI keep rules, and app dependency on `:sdk`.
- **Transplant readiness is not yet proven:** no clean external consumer app has consumed a produced AAR in this audit. The current in-repository app uses `implementation(project(":sdk"))`, which is not the same test.
- AAR publication/versioned dependency configuration was not established from the inspected SDK build file.
- `TEST_MATRIX.md` and `RELEASE_CHECKLIST.md` were not found at root or `docs/` paths on this branch; README instead points to `DEVICE.md` and `docs/RENDERER_FIX.md`. Reconcile paths before treating the user's 10-step matrix as available in-repo.
- Device runtime remains **UNVERIFIED**; Xclipse 940 mip retest and white-head-cap hypothesis remain pending/unconfirmed. Signed release remains **BLOCKED** without a valid keystore. Debug stays DEBUG.
- Deferred: prompt/photo persistence, ADPF thermal adaptation, GLB import, phantom workflows/repo-admin, instrumented tests.
- `main` was not modified. No runtime, Android build, or AAR consumer build was performed in this audit.

Next action: implement a separate consumer smoke-test app/module, build and inspect the AAR/native ABI payload, then run the canonical 10-step device matrix when hardware is available.
