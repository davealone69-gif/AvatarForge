# Build and artifact policy — 2026-10-30

## Production signing: intentionally discontinued

The owner selected removal of the production signing path. There is no production keystore configuration, release-secret lookup, signed-release workflow, or GitHub Release publication path. Do not add signing credentials or create a signed release task unless the owner explicitly changes this policy.

Only the debug APK is intended to be installable. App release variants are guarded and may be packaged only with the explicit `-Paf.allowUnsignedRelease=true` opt-in; that output is compile/package verification only and must remain unsigned. The SDK AAR is likewise a compile/package artifact, not a signed app.

## Compile-only artifact verification

| Check | Result |
|---|---|
| App Gradle has no production signing configuration or credential path | Pass |
| Release packaging fails closed without the explicit opt-in | Pass |
| CI builds debug APK, unsigned release AAB, and SDK AAR without signed-tag publishing | Pass |
| `tools/test_compile_only_artifacts.py` checks policy, tasks, staging paths, and unsigned AAB | Pass |
| Current Gradle build produced debug APK, release AAB, and SDK AAR | Pass |
| Debug APK passed `apksigner verify`; AAB/AAR have no signature entries | Pass |
| Native-linkage guard checked 18 packaged libraries; system Vulkan linked, no loader bundled | Pass |
| Independent install/log/pixel verification | Not verified — a user-supplied SM-S921B report records GLES fallback, critical thermals and 29.3 FPS, but no live ADB session or screenshot was available here |

The dated entries in `docs/audit/VERIFICATION_RESULTS.md` remain historical evidence for older commits. Their references to a planned signed-tag release do not describe the current build policy.
