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
- [ ] CI builds debug APK + explicitly unsigned AAB.
- [ ] Artifact labelled UNSIGNED and not published to GitHub Release.
- [ ] Provenance ties files to commit/run/attempt.
