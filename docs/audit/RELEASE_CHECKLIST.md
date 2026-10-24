# Release checklist

Gate sequence for shipping AvatarForge. The primary artifact is a **signed AAB** for Play; the
secondary artifact is a **signed APK** for direct distribution. Today both signed paths are
**BLOCKED** because no release keystore exists in this environment or as a repository secret.
A debug-certificate build exists as a documented fallback and must never be mislabeled as a
release.

## 0. Evidence and honesty rules

- No checkbox may be marked done without a linked artifact, command output or CI run ID.
- CI green = build/host verified only; device rows remain UNVERIFIED until §4 is executed.
- Any BLOCKED row stays BLOCKED; it is never skipped silently or worked around with debug
  signing for an external release.

## 1. Signing and credentials — BLOCKED (finding AF-009)

| Step | Status | Notes |
|---|---|---|
| Maintainer creates an upload key and keystore outside the repo | BLOCKED | never commit keystores; document owner + escrow |
| Encrypted secrets added to GitHub (`AF_KEYSTORE_B64`, `AF_KEYSTORE_PASSWORD`, `AF_KEY_ALIAS`, `AF_KEY_PASSWORD`) | BLOCKED | requires repo admin; none exist today |
| CI `bundleRelease`/`assembleRelease` step consumes secrets via `keystore.properties` | not started | add to `android-apk.yml` only after secrets exist; keep the unsigned build path working without them |
| `apksigner verify --print-certs` recorded in CI | not started | certificate digest must match the escrowed upload key |
| Play App Signing enrollment decision documented | not started | Google-managed upgrade key vs managed key |
| AAB validated with `bundletool build-apks` (including device-specific APK install) | not started | |

Fallback that exists today: `:app:assembleDebug` produces a debug-signed APK from CI. It is
labeled debug in `artifacts/BUILD-0.10.1.txt` and is suitable for device testing only.

## 2. Build and test gates (must all be green on the release commit)

| Gate | Where | Status of coverage |
|---|---|---|
| NDK native compile + prebuilt refresh | CI `native-compile` job | implemented; run 40097344731 result recorded in VERIFICATION_RESULTS |
| Kernel host harness (~400 checks) | CI `host-tests` (`build_host.sh test`) | implemented this audit (AF-001) |
| ABI manifest drift + conformance | CI `host-tests` | implemented |
| GLB + customization contract validators | CI `host-tests` | implemented |
| JVM unit tests (`:sdk:testDebugUnitTest`) | CI APK job | implemented this audit (AF-001) |
| APK/AAR assembly (debug + unsigned release) | CI APK job | pre-existing, retained |
| Signed AAB/APK | absent | BLOCKED per §1 |
| Instrumented/emulator tests | absent | MISSING (AF-012); release risk accepted only with the §4 manual pass |

## 3. Pre-release static review

- [ ] `docs/audit/FINDINGS.md` dispositions match reality; no "FIXED" label without a green CI
      run and (for runtime findings) a device result.
- [ ] No new permissions: `AndroidManifest.xml` contains no `INTERNET`; FileProvider paths still
      limited to `filesDir/avatars` and `cacheDir/exports` (SECURITY_AUDIT §2).
- [ ] No secrets, tokens or personal identifiers in the tree (`git grep -iE "key|token|password"`
      reviewed; build config carries no credentials).
- [ ] Dependency audit refreshed: pinned versions reviewed, no new transitive dependency without
      a finding-level justification (DEPENDENCY_AUDIT).
- [ ] Version code/name bumped; `BuildConfig`/manifest version row matches the changelog.
- [ ] ABI manifest committed and green (`abi/afcore.abi.json`); renderer C-API untouched or
      re-manifested under the same rule.
- [ ] Changelog/release notes state UNVERIFIED and BLOCKED items honestly, including the AF-014
      white-head-cap hypothesis and the AF-015 import absence.

## 4. Device sign-off (UNVERIFIED until executed; TEST_MATRIX §3 is the full procedure)

Minimum release block:

1. Fresh install of the release-candidate artifact; no `INTERNET` permission shown by the OS.
2. Text generation reaches the viewer; first frame within 5 s; no red diagnostic card.
3. Two-photo generation, including a forced analysis failure: visible AF-003 note, generation
   still completes.
4. Viewer orbit 60 s per zoom extreme on at least one Vulkan and one GLES device; `diag` report
   shows non-zero counters and (if adaptation fires) tier-transition rows.
5. Process-death/restore: render settings persist (AF-004); prompt/photo reset is documented.
6. 30-minute continuous render: FPS distribution, thermal row, memory, no surface-recreate storm.
7. SAF export + FileProvider share + re-validated GLB round trip.
8. Xclipse 940 / ARM Mali retest of the mip fix per `docs/VULKAN_MIP_FIX_2026-10-24.md`;
   white head cap separately classified (AF-014).

## 5. Release execution and post-release

- [ ] Tag on the exact green commit; GitHub Release links AAB + APK + build log + SBOM-style
      dependency list; source archive matches the tag.
- [ ] AAB uploaded to Play (internal testing first); retain the upload key separately.
- [ ] Post-release: monitor crash reports for the renderer recovery paths and the new
      persistence/analysis-note code; any new finding gets an AF/SEC/PERF ID and enters
      FINDINGS.md rather than being fixed ad hoc.
- [ ] Old artifacts in `artifacts/` remain as historical records, never overwritten
      (see `artifacts/BUILD-0.10.1.txt` convention).
