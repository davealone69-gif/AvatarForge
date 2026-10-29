# Branch triage

Status: **PARTIAL** — GitHub compare metadata and selective source diffs were inspected; no branch was merged, deleted, reset, or force-pushed.

Snapshot: 2026-10-29. Repository has 20 branches, 19 PRs in history, 0 releases/tags. Current work remains on `arena/aaf85469-avatarforge`, draft PR #20.

| Branch / PR | Observed divergence | Disposition |
|---|---|---|
| `main` (`40ef65b`) | Default/base; behind active recovery work | Preserve; promote only through reviewed PR |
| `arena/aaf85469-avatarforge` / PR #20 | Active camera/recovery/audit line | KEEP ACTIVE; current CI/evidence line |
| `arena/985042e3-avatarforge` / PR #19 | 38 ahead / 59 files vs main; diagnostics, settings store, device reports, prebuilt refresh workflow | SELECTIVELY PORT useful diagnostics/settings ideas; do not wholesale merge because it removes current recovery/camera files |
| `work/android-engine-dev` / PR #4 | 138 ahead / 213 files; SDK publication, triage/Perfetto docs | REVIEW; preserve unique docs/tooling, avoid stale source overwrite |
| `integrate/audited-branch-work` / PR #13 | 79 ahead / 185 files; selective integration audit | REVIEW as historical integration record; current source is newer for recovery/camera |
| `fix/vulkan-frame-recovery` | 84 ahead; Vulkan fatal/result state, LOD/culling/compression experiments | SELECTIVELY PORT; current patch implements a smaller frame-error/counter fix, advanced LOD/compression remains unverified |
| `fix/render-loop-visibility-watchdog` | 79 ahead | REVIEW lifecycle overlap before porting |
| `fix/renderer-frame-loop-resize-recovery` | 70 ahead; fence/swapchain recovery | REVIEW; current patch covers fatal propagation, not full fence/semaphore rebuild |
| `fix/diagnostics-invalid-render-telemetry` | 62 ahead | REVIEW; current counters/unknown GPU time address part of this |
| `feat/json-asset-registry`, `feat/avatar-realism-upgrade` | AssetRegistry/schemas/acceptance docs | KEEP UNTIL REVIEWED; registry is not runtime-wired in current app |
| `chore/quality-gates-ai-review`, `chore/restore-code-quality-gates` | Quality workflows/policies | REVIEW; do not add AI code rewriting to the selected no-AI stability path |
| `feat/repo-wide-engine-consolidation` | Fully behind main in compare | STALE CANDIDATE; do not delete without confirming references |

## Rules applied

- Ahead/behind counts are commit-graph divergence, not proof that a change is absent under another SHA.
- Historical device reports/screenshots are user-observed evidence for their build, not current-head proof.
- No binary from an old branch replaces current source artifacts without checksum/build/device verification.
- Unique work is preserved by leaving branches/PRs intact until a selective port is reviewed and tested.
