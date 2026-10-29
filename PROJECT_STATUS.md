# Project status

Updated: 2026-10-29. Branch: `arena/aaf85469-avatarforge`, head `1fcbe6c`. PR #20 remains draft.

## Active checkpoint

User direction implemented: **remove Vulkan**, preserve the now-working camera, and fix visible model details. The renderer is **GLES 3.1 only**. Vulkan implementation, SPIR-V table, backend preference/fallback, JNI/Rust fields and the Vulkan link dependency are deleted. Android's own GLES driver may still report an internal translation layer; that is not an AvatarForge backend.

## Verified now

- Exact-head GLES-only + model-detail CI: run https://github.com/davealone69-gif/AvatarForge/actions/runs/40867172914 succeeded on `1fcbe6c`.
- Renderer host fault injection: **57 checks, 0 failures**.
- Focused model-detail regressions: **19 checks, 0 failures**.
- Full kernel host harness: **406 checks, 0 failures**.
- Camera: **user-confirmed working**; exact build SHA/counters still missing.

## Model-detail fixes in this checkpoint

- Palms are built in the anatomic hand frame instead of a world-axis box; interdigital blend reduced to avoid mitten fusion.
- Garment minimum layer is two lattice cells, and the lower-garment arm cut no longer removes thigh cloth below the crotch.
- Viewer header is compact by default so it no longer covers the face/hands; full build/driver details remain expandable. Camera gesture code is unchanged.
- Stale Vulkan-era tracked prebuilt/APK/AAR/sample GLB binaries were removed. CI exact-head artifacts are authoritative, and a linker guard rejects Vulkan dependencies.

## CI artifact

Exact-head artifact: `AvatarForge-Android-1fcbe6cc73425ecd369f797b222eb2cc85315652` (id 12893609460, 17,608,790 bytes): debug APK, explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS, PROVENANCE.json. The unsigned AAB is compile proof only, not an installable production release.

## Still not proven

- Device pixel confirmation of the new hand/garment/header fixes; thermal cap effect; settings persistence on Android; sustained performance/memory; moiré pixel comparison.
- Production signed AAB/APK: **BLOCKED**, no credentials.
- Full engine/render thread ownership fence and diagnostic scene ladder: **MISSING**.
- Remaining eye/hair detail quality: targeted only after matched device captures; no superiority claim.

## Next exact steps

1. Install the next exact-head CI debug APK on SM-S921B; capture front/side hand and garment views plus diagnostics.
2. Implement P0 ownership fence and diagnostic scene ladder from `docs/audit/REMEDIATION_PLAN.md`.
3. Supply production signing credentials through GitHub secrets to unblock signed AAB/APK release.
