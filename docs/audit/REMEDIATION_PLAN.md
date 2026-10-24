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
| 8 | AF-017 restart hint over-warning | `RendererDiagnosticNotes.frameLoopRestartHint`: benign-vs-churn split driven by surface counters | 3 JVM tests (`RendererDiagnosticNotesTest`), CI-green run 40107047136 |
| 9 | AF-018 invisible Vulkan fallback | `RendererDiagnosticNotes.backendFallbackItem`: explicit INFO row when Vulkan preferred but GLES active; reason marked unknown, never guessed | 4 JVM tests, CI-green 40107047136; native reason surfacing queued |
| 10 | — device evidence 2026-10-24 | raw report archived; AF-010/014 visual checks stay open because the Vulkan path did not initialise | `artifacts/audits/device-report-SM-S921B-2026-10-24.txt` |
| 11 | AF-020 camera UBO overrun (GLES confirmed, Vulkan latent) | `af::afRoundUp` slot sizing in new `af_align.h`; `cameraSlotSize_` stride in both backends; post-UBO latent risk documented only | host harness [17], 34 new checks (439 total), 0 failures local; NDK compile + device effect CI/device-UNVERIFIED |
| 12 | AF-020 full-screen atlas (visual) | evidence pack, screenshot ledger and shipped-controls localization ladder; no fix claim — mechanism PARTIAL | `artifacts/audits/device-evidence-atlas-2026-10-25.md` + `screenshots/` |

## Queued — next sessions (priority order)

1. **AF-020 device confirmation (high).** Ship a build containing the camera-UBO slot fix and
   run device backlog step 11 (TEST_MATRIX §3): body visible vs atlas persists; if it persists,
   execute the seven-step shipped-controls ladder in the evidence pack §6 and, if still
   ambiguous, add a `KHR_debug`/`glGetError` surfacing path AFTER the AF-019 renderer C-API gate
   exists. Do not claim the UBO fix resolves the atlas without on-device evidence.
2. **AF-009 release integrity (medium, BLOCKED on credentials).** Add `:app:bundleRelease` to
   CI once a release keystore is supplied via encrypted secret; document AAB verification
   (apksigner, bundletool). Unblock: maintainer adds `keystore.properties` secrets.
3. **SEC-1/SEC-2 workflow hardening (low).** Move `contents: write` into the refresh-only
   workflow; SHA-pin third-party actions.
4. **AF-008 ADPF (medium).** `PerformanceHintManager` session on the render thread (API 31+),
   `getThermalHeadroom` ≤1 poll/10 s with NaN handling, feeding `DeviceResourceGovernor`;
   diagnostics must distinguish unsupported from safe.
5. **AF-019 + AF-018 native half (low/medium).** Extract `native/include/afrender.h`, give the
   renderer C API its own manifest/conformance gate (today 0 of its symbols are in
   `afcore.abi.json`), then extend `af_render_info` with the retained Vulkan rejection reason and
   wire it through JNI so the fallback row can show the actual cause.
6. **AF-015 GLB import (medium feature).** JNI wrapper over the existing `af_glb_read`,
   SAF-open contract, scene attachment into the renderer; restore the VIEW filter only when the
   handler exists; add malformed-file JNI tests and the device open-flow test (TEST_MATRIX #10).
7. **Renderer diagnostic stages (renderer audit §5).** C ABI flag for the eight deterministic
   scenes; device-gated verification; this replaces guesswork in any future "zero render" bug.
8. **PERF-1 GPU timing.** VK timestamp queries / GL timer queries into `RendererStats.cpuMs`
   counterpart; then benchmark per PERFORMANCE_BASELINE §5.
9. **AF-012 instrumented tests.** `androidTest` smoke + export round trip on an emulator runner.
10. **SEC-3 fuzzing.** Expand the GLB/PNG corpus; run under ASan in a scheduled (non-gating) job.
11. **AF-004 remainder.** Persist prompt/spec and reference thumbnails (scoped, auto-purged).
12. **AF-005/AF-006/AF-013 hygiene.** Decide `engine/` fate; add cargo test job or mark Rust
    reference-only; introduce `libs.versions.toml`.
13. **AF-007 phantom workflows.** Maintainer deletes the three orphan registrations (token-blocked
    here).
14. **AF-014 white head cap.** Re-screenshot on a build containing the mip fix; only then decide
    whether a material/bake defect exists.

## Standing constraints on all future work

- No status upgrades without new evidence; CI-green ≠ device-verified.
- Every native ABI change updates `afcore.abi.json` via the generator and passes
  `abi_conformance.py`. The renderer C API (`af_render_capi`) has no equivalent gate today
  (AF-019); do not extend its structs until one exists.
- No new permission, network API or dependency without a finding-level justification.
- One logical change per commit; finding ID in the message where one exists.
