# AvatarForge — project status

_Rollup of the 2026-10 audit. Evidence details live in `docs/audit/`; this page is the
entry point and never carries a claim that lacks a backing document._

## Headline status

- **Build integrity:** REAL + VERIFIED in CI. Run `40097344731` proved the new test gate works
  by failing on a real pre-existing defect (AF-016, unvalidated shader stage); after the fix,
  run `40098617172` is fully green: NDK native compile, host kernel harness (405/405), ABI
  conformance (47/47), asset validators, 21 JVM unit tests, and APK/AAR assembly. Per-job
  evidence: `docs/audit/VERIFICATION_RESULTS.md`.
- **Device runtime:** UNVERIFIED. No device or emulator exists in the build environment.
  Every renderer, UI, lifecycle and thermal claim is backed by a manual procedure in
  `docs/audit/TEST_MATRIX.md` §3, not by assumption.
- **Signed release:** BLOCKED. No release keystore exists; only debug-signed/unsigned
  artifacts are produced. Path to unblock: `docs/audit/RELEASE_CHECKLIST.md`.
- **Security posture:** no critical or high findings; offline-by-construction (no INTERNET
  permission) verified at manifest level. Detail: `docs/audit/SECURITY_AUDIT.md`.

## What the audit changed this session

| Finding | Change | Commit(s) | Verification |
|---|---|---|---|
| AF-001 no test gate in CI | `:sdk:testDebugUnitTest` in the APK job; new `host-tests` job (kernel harness, ABI drift/conformance, GLB + customization validators) | `e84387c` | CI run `40097344731` |
| AF-002 dead GLB "open with" entry | VIEW intent-filter removed until import exists (AF-015) | `2f21f45` | manifest + package build |
| AF-003 silently ignored photo-analysis failure | `ImageAnalysisNotes` maps failure/empty/measured outcomes to an honest on-screen note | `94256cd`, `6a81fc0` | 4 JVM tests, CI |
| AF-004 settings lost on process death | `SettingsStore` + pure `RenderSettingsCodec`, wired into load/save | `749375b`, `6a81fc0` | 6 JVM tests, CI; device restore UNVERIFIED |
| AF-008 invisible quality adaptation | frame-numbered tier transitions in diagnostics report and panel (no ABI change) | `c20af03` | 4 JVM tests, CI; ADPF still queued |
| AF-016 shader-table harness drift (found by the new gate) | `forward_plus` compute stage added to harness coverage; all 13 baked entries now validated | `af09371` | local 405/405; host-tests green on run `40098617172` |

Prompt/photo persistence (AF-004 remainder) and ADPF thermal adaptation (AF-008 remainder) are
deliberately deferred with tracked entries, not silently dropped.

## Audit document index

| Document | Purpose |
|---|---|
| `docs/audit/BASELINE.md` | repository state when the audit started |
| `docs/audit/FINDINGS.md` | full findings register AF-001…AF-015 with evidence and dispositions |
| `docs/audit/ARCHITECTURE_AUDIT.md` | module/native/threading architecture review |
| `docs/audit/RENDERER_AUDIT.md` | renderer correctness, diagnostic scene ladder, zero-render method |
| `docs/audit/SECURITY_AUDIT.md` | attack surface, permissions, FileProvider/SAF, CI hardening (SEC-1…4) |
| `docs/audit/PERFORMANCE_BASELINE.md` | the one real 58 FPS observation, adaptation constants, benchmark protocol |
| `docs/audit/DEPENDENCY_AUDIT.md` | pinned dependencies and review posture |
| `docs/audit/COMPETITIVE_ANALYSIS.md` | Filament/bgfx benchmarking and the keep-custom-renderer decision |
| `docs/audit/FEATURE_TRACEABILITY.md` | advertised features traced to real execution paths |
| `docs/audit/TEST_MATRIX.md` | what each test layer proves, and the 10-step device backlog |
| `docs/audit/REMEDIATION_PLAN.md` | delivered vs queued work in mandated priority order |
| `docs/audit/VERIFICATION_RESULTS.md` | evidence ledger: CI results, local checks, explicit UNVERIFIED/BLOCKED lists |
| `docs/audit/RELEASE_CHECKLIST.md` | gated release process; signing currently BLOCKED |

## Top risks, honestly ranked

1. **Runtime is unverified end-to-end.** Strong host/JVM/ABI coverage cannot prove a frame
   reaches a display. The device backlog (TEST_MATRIX §3) is the gating work before any
   external release claim.
2. **No signed delivery path (AF-009).** Keystore provisioning is a maintainer action;
   debug builds must not be redistributed as releases.
3. **Renderer adaptation explains itself but still ignores thermal headroom (AF-008
   remainder).** ADPF work is specified against Google's polling limits in REMEDIATION_PLAN.
4. **Two open hypotheses need device evidence:** the white head cap (AF-014) and final
   confirmation of the Xclipse mip fix (`docs/VULKAN_MIP_FIX_2026-10-24.md`).
5. **Tech-debt items AF-005/006/013** (dead `engine/` module, Rust workspace outside CI, no
   version catalog) are documented and deferred, not forgotten.

## Standing rules for continued work

- Labels only: REAL + VERIFIED / UNVERIFIED / PARTIAL / MISSING / BLOCKED — no upgrades
  without new evidence; CI green never implies device-verified.
- Every regression test names its defect ID; tests are never weakened to go green.
- No INTERNET permission, no speculative dependency upgrades, no new permission or network
  API without a finding-level justification.
- Native ABI changes require manifest regeneration and conformance; both are now CI gates.
- Diagnostics show measured facts and explicit unknown states — never fabricated zeros.
