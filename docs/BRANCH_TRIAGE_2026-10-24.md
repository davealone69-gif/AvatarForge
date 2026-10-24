# AvatarForge branch triage

**Snapshot date:** 2026-10-24  
**Repository:** [davealone69-gif/AvatarForge](https://github.com/davealone69-gif/AvatarForge)  
**Canonical integration branch:** `work/android-engine-dev`  
**Default/stable branch:** `main`

## Rules

- Do not bulk-merge divergent branches or force-update `main`.
- Never delete a branch until unique work is preserved and the branch's PR/release references are checked.
- Ahead/behind counts indicate commit-graph divergence, not whether the code change is already present under a different commit.
- Only claim **REAL + VERIFIED** after exact-head CI/build evidence and installed-device runtime evidence. A user screenshot is valid user-observed evidence, but does not identify the APK/source SHA.

## User report and current working baseline

The user-provided viewer screenshot displays Vulkan, Samsung Xclipse 940, 58 FPS / 17.2 ms, 417,150 triangles on screen, 304,797 vertices, 71 joints, 52 ARKit blendshapes, a 37.5 MB GLB, and an 18-second build. Treat this as **REAL + USER-OBSERVED** evidence for that session, not a verified current-source build or sustained soak. Visible surface rippling/striping and questionable hand/head/neck appearance remain quality issues to isolate without destabilizing the returned image.

The user reports the DIY fault-finding and manufacturer-diagnosis flow is built into the app. PR #19's fetched diff contains an in-app Diagnostics dialog/panel, live renderer diagnostics, actionable hints, and a copy-report action. Code presence is observed in that PR diff; installed interaction is **UNVERIFIED**. Keep `docs/DIY_FAULT_FINDING.md` as the long-form guide and route future troubleshooting through in-app Diagnostics first.

## Branch inventory and disposition

| Branch | Current disposition | Next action |
|---|---|---|
| `arena/985042e3-avatarforge` | **KEEP ACTIVE**; PR #19 head | Review full diff and exact-head Actions; candidate for renderer-startup/Diagnostics integration. |
| `fix/vulkan-frame-recovery` | **REVIEW / SELECTIVELY PORT** | Compare Vulkan recovery changes against PR #19 and canonical branch; preserve unique synchronization fixes. |
| `fix/renderer-frame-loop-resize-recovery` | **REVIEW / SELECTIVELY PORT** | Confirm callback/resize recovery is present and tested. |
| `fix/render-loop-visibility-watchdog` | **REVIEW / SELECTIVELY PORT** | Confirm lifecycle visibility fix and diagnostics are not duplicated. |
| `fix/renderer-recovery-2026-10-21` | **REVIEW / SELECTIVELY PORT** | Diff against the candidate; preserve unique recovery behavior. |
| `fix/diagnostics-invalid-render-telemetry` | **REVIEW / SELECTIVELY PORT** | Retain INCONCLUSIVE semantics for zero/invalid samples. |
| `fix/restore-self-test-menu-phase4` | **REVIEW / SELECTIVELY PORT** | Ensure self-test/Diagnostics menu is actually available. |
| `fix/avatar-head-hands-geometry-pass1` | **KEEP UNTIL REVIEWED** | Incremental geometry changes only; inspect rendered mesh before claims. |
| `fix/avatarforge-overexposed-render` | **KEEP UNTIL REVIEWED** | Inspect exposure/material patch and its relationship to current visual artifacts. |
| `feat/avatar-realism-upgrade` | **KEEP UNTIL REVIEWED** | Preserve any unique realism work. |
| `feat/json-asset-registry` | **SELECTIVE INTEGRATION** | Check schema/parameter changes and confirm real UI/engine wiring. |
| `chore/restore-code-quality-gates` | **REVIEW** | Consolidate with the other quality-gate branch without adding disallowed tools. |
| `chore/quality-gates-ai-review` | **REVIEW** | Deduplicate workflow/policy changes against canonical branch. |
| `chore/apply-dropbox-update-2026-10-21` | **REVIEW** | Inspect unique Dropbox-sourced fixes before porting. |
| `integrate/audited-branch-work` | **REVIEW** | Avoid reapplying already integrated patches; inspect unique file changes. |
| `feat/repo-wide-engine-consolidation` | **STALE CANDIDATE, NOT DELETED** | Current compare returned no commits ahead of canonical branch; recheck head/PR references before retirement. |
| `main` | **PRESERVE AS DEFAULT/STABLE** | No force push/reset. Promote via reviewed PR after CI. |
| `work/android-engine-dev` | **CANONICAL INTEGRATION BRANCH** | Keep new fixes and verified notes here; exact current build/runtime status still needs evidence. |

## Sequence to finish sorting

1. Snapshot exact HEAD SHAs and CI checks for the canonical branch and every open PR.
2. Inspect PR #19's full diff and exact-head Actions. Do not merge solely because GitHub says mergeable.
3. Compare branch changes file-by-file and record each as **already integrated**, **selectively port**, **keep active**, **superseded**, or **safe to retire after confirmation**.
4. Port/merge only reviewed changes with successful exact-head CI. Do not bulk-merge divergent branches.
5. Rerun repository verification and Android APK/AAB builds. Tie artifacts to exact source SHAs.
6. Device-check the installed APK, including visible geometry, live counters, diagnostics/report copy, resize/background recovery, animation, generation, export/import, and sustained performance.
7. Only close/delete superseded PRs/branches after unique work and references are accounted for.

## Snapshot caveat

This document is a triage checkpoint, not a claim that branches have been merged or deleted. The live branch list had 18 refs. Compare results show substantial divergence for most branches; ahead/behind numbers must not be treated as missing-patch counts. No branch mutations were made as part of this checkpoint.
