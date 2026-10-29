# Project status

Updated: 2026-10-29. Branch: `arena/aaf85469-avatarforge`, head `5e2f990`. PR #20 remains draft.

## Active checkpoint

Maximum evidence-backed stability/integrity pass delivered. No AI was added. Exact-head CI is green: https://github.com/davealone69-gif/AvatarForge/actions/runs/40858597544.

## Implemented this pass

- Fail-closed release signing; CI unsigned AAB compile artifact + provenance; signed tag-release job gated on credentials.
- Renderer frame-error propagation, fatal Vulkan halt, distinct attempt/submission/accepted-present counters, honest CPU/GPU timing labels.
- GLES host swap-failure regression: 53 checks, 0 failures. Kernel host harness: 406 checks, 1 pre-existing shader-table-count failure.
- Thermal quality/pacing policy + API-29 monitor; settings persistence; progress overcount fix; atomic job IDs; serial generation dispatcher; safer release-after-cancellation.
- Shader embedder no longer silently empties SPIR-V; CI shader compile/diff gate.
- Native GLB/PNG import limits; removed dead GLB VIEW advertisement.
- Audit docs under `docs/audit/`.

## CI artifact

`AvatarForge-Android-5e2f990b5435cfdfa5ee709b905e675b0478a2cb` (id 12890980765, 18,226,242 bytes): debug APK, explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS, PROVENANCE.json. The unsigned AAB is compile proof only, not installable production release.

## Still not proven

- Device camera movement, Vulkan device recovery, thermal cap effect, settings persistence on Android, moiré pixel improvement, sustained performance/memory.
- Production signed AAB/APK: BLOCKED, no credentials.
- Full engine/render thread ownership fence and diagnostic scene ladder: MISSING.

## Next exact steps

1. Device-retest camera + diagnostics on SM-S921B using the CI debug APK from run 40858597544.
2. Implement P0 ownership fence and diagnostic scene ladder from `REMEDIATION_PLAN.md`.
3. Supply production signing credentials through GitHub secrets to unblock signed AAB/APK release.
