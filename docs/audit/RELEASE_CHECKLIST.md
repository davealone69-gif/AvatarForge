# Release checklist

## Production release — BLOCKED without credentials

- [ ] Production keystore supplied through ignored `keystore.properties` or `AF_RELEASE_*` secrets.
- [ ] `:app:bundleRelease` produces signed AAB (primary).
- [ ] `:app:assembleRelease` produces signed APK (secondary).
- [ ] `apksigner verify --print-certs` passes for APK.
- [ ] `jarsigner -verify` passes for AAB.
- [ ] Package `com.avatarforge`, versionCode/versionName verified with aapt.
- [ ] ABI list verified (`arm64-v8a` current CI; add x86_64 only with build proof).
- [ ] Manifest permissions verified: no INTERNET/storage/camera.
- [ ] SHA256SUMS and provenance JSON attached.
- [ ] Device install/update smoke test on SM-S921B.
- [ ] No debug/unsigned fallback permitted.

## CI compile artifact — not a production release

- [x] Source-defined fail-closed signing policy.
- [x] CI builds debug APK + explicitly unsigned AAB (run 40858597544).
- [x] Artifact labelled UNSIGNED and not published to GitHub Release.
- [x] Provenance ties files to PR head SHA/run/attempt.


## Current branch — 2026-10-30 workflow audit

- [x] Signed-tag verifier inputs now match the signed APK/AAB and SDK AAR staged by that job.
- [x] `python3 tools/test_release_workflow.py` statically guards staging/verifier path consistency and current `fix/**` CI triggers.
- [ ] Run the signed-release job on an authorized version tag and verify signed outputs; credentials are still required.
- [ ] Install the exact signed build on the SM-S921B and complete the device checklist.

## Local working-tree compile evidence — 2026-10-30

- [x] NDK 26.1.10909125 built ARM64 native libraries for `android-26`; linker guard found system `libvulkan.so` and no bundled loader.
- [x] App and SDK JVM tests passed (3 app tests, 38 SDK tests); debug APK, unsigned compile-only release AAB, and SDK AAR built.
- [x] APK metadata reports `com.avatarforge`, versionCode `2`, versionName `0.10.1`, min SDK 26 and target SDK 34; `apksigner verify` passes for the debug APK, which uses the Android debug certificate.
- [x] APK has no INTERNET permission; the release AAB has no signature entries as expected for compile-only verification; all nine packaged native libraries passed the system-Vulkan/no-bundled-loader guard.
- [ ] Production-signed APK/AAB, release certificate identity, and signed-tag workflow execution remain **BLOCKED** without release credentials.
- [ ] Install and smoke-test the exact working-tree APK on the SM-S921B; device installation and Vulkan runtime are not proven here.
