# Remediation plan

Ordered by the directive's priority (security/data-loss → crashes → renderer correctness →
memory/ABI → performance → UX → release → capability). "This session" items are delivered with
the branch; the rest are queued with explicit unblock conditions.

## Delivered this session

| Order | Finding | Change | Verification |
|---|---|---|---|
| 1 | AF-001 tests not gated | APK job runs `:sdk:testDebugUnitTest`; new `host-tests` CI job (zlib + host harness + ABI drift + conformance + GLB validation + customization contracts) | run 40097344731 APK/NDK green and host-tests red on real defect AF-016; run 40098617172 all three jobs green |
| 2 | AF-002 dead GLB intent | remove unhandled VIEW filter from app manifest | manifest inspection + XML parse; package build green; device "open with" absence stays in the device backlog |
| 3 | AF-003 silent analysis failure | honest `analysisNote` on null/error path; pure-Kotlin note mapping + JUnit test | `ImageAnalysisNotesTest` (4 tests), CI-green |
| 4 | AF-004 settings lost on process death | `SettingsStore` (SharedPreferences) + pure codec, wired to viewer load/save | `RenderSettingsCodecTest` round-trip + corruption cases (6 tests), CI-green |
| 5 | AF-008 invisible quality changes | tier transitions captured into `RendererDiagnostics` from stats (no ABI change), shown in report/panel | compile + `DiagnosticsReportTest` (4 tests), CI-green |
| 6 | — report drift | structured report test locks worst-status and plain-text behaviour | `DiagnosticsReportTest` |
| 7 | AF-016 shader-table harness drift | add the shipped `forward_plus` compute stage to the harness `want[]` so all 13 table entries are validated | local 405/405 pass; host-tests job on run 40098617172 |

## Queued — next sessions (priority order)

1. **AF-009 release integrity (medium, BLOCKED on credentials).** Add `:app:bundleRelease` to
   CI once a release keystore is supplied via encrypted secret; document AAB verification
   (apksigner, bundletool). Unblock: maintainer adds `keystore.properties` secrets.
2. **SEC-1/SEC-2 workflow hardening (low).** Move `contents: write` into the refresh-only
   workflow; SHA-pin third-party actions.
3. **AF-008 ADPF (medium).** `PerformanceHintManager` session on the render thread (API 31+),
   `getThermalHeadroom` ≤1 poll/10 s with NaN handling, feeding `DeviceResourceGovernor`;
   diagnostics must distinguish unsupported from safe.
4. **AF-015 GLB import (medium feature).** JNI wrapper over the existing `af_glb_read`,
   SAF-open contract, scene attachment into the renderer; restore the VIEW filter only when the
   handler exists; add malformed-file JNI tests and the device open-flow test (TEST_MATRIX #10).
5. **Renderer diagnostic stages (renderer audit §5).** C ABI flag for the eight deterministic
   scenes; device-gated verification; this replaces guesswork in any future "zero render" bug.
6. **PERF-1 GPU timing.** VK timestamp queries / GL timer queries into `RendererStats.cpuMs`
   counterpart; then benchmark per PERFORMANCE_BASELINE §5.
7. **AF-012 instrumented tests.** `androidTest` smoke + export round trip on an emulator runner.
8. **SEC-3 fuzzing.** Expand the GLB/PNG corpus; run under ASan in a scheduled (non-gating) job.
9. **AF-004 remainder.** Persist prompt/spec and reference thumbnails (scoped, auto-purged).
10. **AF-005/AF-006/AF-013 hygiene.** Decide `engine/` fate; add cargo test job or mark Rust
    reference-only; introduce `libs.versions.toml`.
11. **AF-007 phantom workflows.** Maintainer deletes the three orphan registrations (token-blocked
    here).
12. **AF-014 white head cap.** Re-screenshot on a build containing the mip fix; only then decide
    whether a material/bake defect exists.

## Standing constraints on all future work

- No status upgrades without new evidence; CI-green ≠ device-verified.
- Every native ABI change updates `afcore.abi.json` via the generator and passes
  `abi_conformance.py`; renderer ABI changes follow the same rule for `af_render_capi`.
- No new permission, network API or dependency without a finding-level justification.
- One logical change per commit; finding ID in the message where one exists.
