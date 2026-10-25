# Baseline — real project state at audit start

- Date: 2026-10-24 (Australia/Perth)
- Auditor session: zero-trust engineering audit, branch `arena/985042e3-avatarforge`
- Inspected commit: `1aa73de` (branch tip at audit start; PR #19 open against `main`)
- Working directory: `/home/user/AvatarForge`
- Remote: `origin https://github.com/davealone69-gif/AvatarForge.git`
- Tags: none. Releases: none (CI only publishes on `v*` tags, which do not exist).

## 1. Environment actually present in the audit sandbox

Evidence commands and outcomes:

| Check | Command | Result |
|---|---|---|
| JDK / Kotlin / Gradle | `command -v java javac kotlin gradle` | all ABSENT |
| Android SDK / NDK / adb | `command -v adb sdkmanager`; `echo $ANDROID_HOME` | ABSENT / unset |
| CMake / Ninja | `command -v cmake ninja` | ABSENT |
| Host C++ compiler | `command -v g++` | present (`/usr/bin/g++`) |
| zlib headers (host harness dependency) | `ls /usr/include/zlib.h` | ABSENT — host harness BLOCKED locally |
| Rust toolchain | `command -v rustc cargo` | ABSENT |
| CPUs / RAM | `nproc`, `free -h` | 2 cores, 3.8 GiB |

Consequence: in-sandbox verification is limited to pure-JVM/Python logic and static
inspection. All Kotlin/C++ compilation is delegated to GitHub Actions; all device behaviour
remains UNVERIFIED in this session. No device or emulator is available.

## 2. Build identity (from source, not assumptions)

- Gradle wrapper: `gradle-8.9-bin.zip` (`gradle/wrapper/gradle-wrapper.properties`)
- AGP `8.5.2`, Kotlin Android plugin `1.9.24`, Compose compiler extension `1.5.14`
  (root `build.gradle.kts`, `app/build.gradle.kts`)
- NDK pinned `26.1.10909125`; CMake `3.22.1`; Compose BOM `2024.06.00`
- Application ID `com.avatarforge`; version `0.10.1 (versionCode 2)`
- `compileSdk = 34`, `minSdk = 26`, `targetSdk = 34`; ABIs built in CI: `arm64-v8a` only
- Modules: exactly `:sdk` and `:app` (`settings.gradle.kts`). The top-level `engine/`
  directory is **not** a Gradle module and has zero references (see FINDINGS AF-005).
- Dependency strategy: hardcoded coordinates in each module's `build.gradle.kts`; there is no
  `gradle/libs.versions.toml` version catalog.
- R8/minify: disabled for both debug and release (documented JNI-name rationale; keep rules in
  `sdk/consumer-rules.pro`). Release signing falls back to the debug keystore when
  `keystore.properties` is absent — no keystore or secret is committed.
- Primary deliverable gap: no App Bundle (`.aab`) task exists anywhere; CI produces two APKs and
  one AAR. Signed-store delivery is BLOCKED on credentials.

## 3. Evidence collected this session

- `python3 tools/abi_manifest.py` regenerated `abi/afcore.abi.json` → **no diff**, so the
  machine-readable ABI contract (47 functions, 11 structs, 12 enums, ABI version 1) is in sync
  with `native/include/afcore.h`. Status REAL + VERIFIED for manifest/header drift.
- `python3 tools/validate_glb.py artifacts/avatarforge-sample.glb` →
  `OK: all structural checks passed` (70,148 B JSON / 35,118,564 B BIN; 271,155 vertices,
  369,108 triangles, 52 morph targets, 71 joints, 15 animations, 13 materials). The sample is a
  prior export and is **not** proof of the current source (it predates recent renderer fixes).
- GitHub Actions on the branch:
  - run `40092525181` — both jobs SUCCESS (`Build and verify Android packages`,
    `Compile native libraries from source (NDK)`).
  - The NDK job rebuilt all three `.so` files from source; commit `e9bc49b` refreshed
    `prebuilt/arm64-v8a/` (e.g. `libafrender.so` 1,852,968 → 1,876,176 bytes), so the APK
    packages the Vulkan mip-chain fix.
- Device evidence on file (user-supplied, not reproducible here): SM-S921B (Galaxy S24,
  Exynos 2400, Xclipse 940), Android 17 API 37, Vulkan 1.3.304, driver 104869891; one
  diagnostics report showing 58 FPS / 17.3 ms / tier 2 / 14 draw calls / 426,170 triangles,
  plus a screenshot of rippling surface artifacts. Device runtime remains UNVERIFIED for every
  fix made after that report.

## 4. Pre-existing engineering record (read, not re-derived)

- `STATUS.md` (2026-10-18): claims host harness 405/405, focused ABI 116/116, JVM 7/7 — but
  these results come from a now-unavailable pinned toolchain, and no current workflow runs them
  (see AF-001). The document is stale regarding the renderer (it predates Vulkan device
  evidence and the 2026-10-24 fixes). When the harness was finally re-run in CI it was in fact
  404/405: the `forward_plus` compute stage had shipped without updating the table checks
  (AF-016, fixed this audit; 405/405 restored locally and in CI).
- `docs/ENGINE_CONSOLIDATION_AUDIT.md`: cross-repository consolidation decision keeping
  AvatarForge canonical; still useful, narrower scope than this audit.
- `docs/RENDERER_RECOVERY_2026-10-21.md`, `docs/RENDERER_FIX_2026-10-24.md`,
  `docs/VULKAN_MIP_FIX_2026-10-24.md`, `docs/DIY_FAULT_FINDING.md`: renderer work history.

## 5. Honest verification state at baseline

| Capability | Status | Basis / limitation |
|---|---|---|
| Kotlin compiles, APK/AAR assembled | REAL + VERIFIED (CI) | green Actions run on branch tip |
| Native C++ compiles for arm64 | REAL + VERIFIED (CI) | NDK source-build job green |
| ABI manifest matches header | REAL + VERIFIED (local) | regeneration produced no diff |
| Sample GLB structurally valid | REAL + VERIFIED (local) | validator output above |
| JVM/host/ABI tests on current source | UNVERIFIED | not executed by any current workflow; local host run blocked on zlib |
| Vulkan actually renders the avatar | PARTIAL | one device report shows RUNNING + counters; visual correctness after the mip fix UNVERIFIED |
| GLES 3.1 path on target device | UNVERIFIED | no device run observed |
| Long-duration stability, thermal, process death | UNVERIFIED | no device/automation |
| Signed release / AAB | BLOCKED | no keystore; no bundle task |
| `engine/` and `rust/` capabilities | PARTIAL | code exists, neither is built by the shipped pipeline |

## 6. Scope and order of this audit

1. Static, evidence-backed audit of every module (ARCHITECTURE_AUDIT, FINDINGS,
   FEATURE_TRACEABILITY, DEPENDENCY_AUDIT).
2. Competitive research limited to authoritative sources (COMPETITIVE_ANALYSIS).
3. Renderer/security/performance/test/release deep dives against the code as written.
4. Remediations restricted to changes verifiable in CI or by local pure-JVM/Python checks,
   each with a regression test where one is possible. Device-only outcomes stay explicitly
   UNVERIFIED. See REMEDIATION_PLAN and VERIFICATION_RESULTS.
