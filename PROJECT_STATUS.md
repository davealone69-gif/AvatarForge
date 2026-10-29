# Project status

Updated: 2026-10-29. Branch: `arena/aaf85469-avatarforge`. PR #20 remains draft.

## Active checkpoint

Maximum evidence-backed stability/integrity pass in progress. No AI was added. Source changes are saved on the Arena branch; exact-head CI is pending at this checkpoint.

## Implemented this pass

- Fail-closed release signing; CI unsigned AAB compile artifact + provenance; signed tag-release job gated on credentials.
- Renderer frame-error propagation, fatal Vulkan halt, distinct attempt/submission/accepted-present counters, honest CPU/GPU timing labels.
- GLES host swap-failure regression: 53 checks, 0 failures. Kernel host harness: 406 checks, 1 pre-existing shader-table-count failure.
- Thermal quality/pacing policy + API-29 monitor; settings persistence; progress overcount fix; atomic job IDs; serial generation dispatcher; safer release-after-cancellation.
- Shader embedder no longer silently empties SPIR-V; CI shader compile/diff gate.
- Native GLB/PNG import limits; removed dead GLB VIEW advertisement.
- Audit docs under `docs/audit/`.

## Still not proven

- Current-head Android CI, device camera movement, Vulkan device recovery, thermal cap effect, settings persistence on Android, moiré pixel improvement, sustained performance/memory.
- Production signed AAB/APK: BLOCKED, no credentials.
- Full engine/render thread ownership fence and diagnostic scene ladder: MISSING.

## Next exact steps

1. Commit/push; watch exact-head CI; update docs with run/artifact evidence.
2. Device-retest camera + diagnostics on SM-S921B using the CI debug APK.
3. Implement P0 ownership fence and scene ladder from `REMEDIATION_PLAN.md`.
