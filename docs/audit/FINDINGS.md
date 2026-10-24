# Findings register

Severity: **critical / high / medium / low**. Status uses the directive's labels. Every entry
states how it was detected and what evidence supports it; hypotheses are marked as such.
Fixes land as focused commits referenced from this register.

| ID | Sev | Area | Title | Status |
|---|---|---|---|---|
| AF-001 | high | CI/test | No workflow executes any test on push/PR | FIXED; CI-gated (runs 40097344731, 40098617172) |
| AF-002 | medium | app/intents | Manifest GLB VIEW intent has no handler (dead external contract) | FIXED; CI package build green, device "open with" check UNVERIFIED |
| AF-003 | medium | app/jobs | Image-analysis failure is silently swallowed | FIXED in code; 4 JVM tests CI-green; Compose rendering UNVERIFIED |
| AF-004 | medium | app/persistence | No state persistence; settings and prompt lost on process death | PARTIAL → renderer settings FIXED (6 JVM tests CI-green; device restore UNVERIFIED); prompt/photos deferred |
| AF-005 | low | build | `engine/` directory is a dead, unregistered module | CONFIRMED, deferred |
| AF-006 | low | build/rust | Rust workspace is outside the shipped build and all CI | CONFIRMED, documented |
| AF-007 | low | CI hygiene | Three "active" GitHub workflows have no YAML in any branch | BLOCKED (repo-admin) |
| AF-008 | medium | renderer | Adaptive quality changes are not explainable; no ADPF | PARTIAL: tier-transition telemetry FIXED (4 JVM tests CI-green; device display UNVERIFIED); ADPF queued |
| AF-016 | medium | native/test | Shader-table harness expected 12 stages; forward_plus compute stage shipped unvalidated | FIXED (`af09371`); gate proved its worth on first run |
| AF-009 | medium | release | No AAB; release APK uses debug certificate | BLOCKED (credentials) |
| AF-010 | — (historical) | renderer/vulkan | Mip-chain image-layout spec violation caused surface rippling | FIXED (commit `bac2168`+`e9bc49b`), device retest UNVERIFIED |
| AF-011 | — (historical) | diagnostics | Counter shadowing, red-card strobe, redundant resizes | FIXED (commits `9b0af6d`/`bac2168`) |
| AF-012 | low | test strategy | No instrumented/device tests exist | CONFIRMED, roadmap |
| AF-013 | low | deps | No version catalog; dependency coordinates hardcoded | CONFIRMED, deferred |
| AF-014 | low | renderer | White head cap in device screenshot | UNVERIFIED hypothesis |
| AF-015 | medium | feature | GLB import/open is not wired into the app | MISSING (filter removed; feature planned) |

---

## AF-001 — No workflow executes any test (high)

- Evidence: `.github/workflows/android-apk.yml` (before this session) invokes only
  `:app:assembleDebug :app:assembleRelease :sdk:assembleRelease`. No `test*`, no host harness,
  no `tools/abi_*.py`, no `tools/validate_glb.py`. The 7/7 JVM and 405/405 host numbers in
  `STATUS.md`/`artifacts/BUILD-0.10.1.txt` are a one-off record from 2026-10-18, not a gate.
- Root cause: CI was added around packaging; test execution was never wired into a workflow.
- Impact: regressions in pure-Kotlin safety logic, the kernel ABI and GLB validity merge
  unobserved; "CI green" overstates verification.
- Fix: run `:sdk:testDebugUnitTest` in the APK job; add a `host-tests` job that installs
  zlib, runs `tools/build_host.sh test`, regenerates the ABI manifest and fails on drift, and
  re-validates the sample GLB.
- Regression test: the workflow itself; this session's new JVM tests execute there (21 tests
  total as of this audit). The gate found a real defect on its first run — AF-016.
- Evidence: run `40097344731` (APK + NDK jobs green; host-tests red on AF-016) and run
  `40098617172` after the harness fix (see VERIFICATION_RESULTS.md for conclusions).

## AF-002 — Dead GLB VIEW intent filter (medium)

- Evidence: `app/src/main/AndroidManifest.xml` registers an `ACTION_VIEW` filter for
  `model/gltf-binary`; `grep` of `MainActivity.kt` shows no `intent`/`ACTION_VIEW` handling in
  `onCreate` or anywhere else. `af_glb_read.cpp` exists and is host-harness-tested but
  `grep -rn "glbRead|readGlb|importGlb|loadGlb" sdk/src app/src` returns no Kotlin caller.
- Impact: a user opening a `.glb` with AvatarForge lands on the input screen with no import and
  no message — a fake capability visible to the whole platform (launcher "open with" entries).
- Fix: remove the filter until import exists (this session); track the feature as AF-015.
- Verification: manifest inspection; CI package audit continues to pass.

## AF-003 — Silent image-analysis failure (medium)

- Evidence: `MainActivity.onGenerateImages` used
  `runCatching { engine.analyseImages(...) }.getOrNull()` and on null fell back to the text-only
  `spec` with no `analysisNote`. The user's reference photos could be ignored without any signal.
- Root cause: exception-as-control-flow with no user-visible branch.
- Fix: distinguish "no analysis returned" from "analysis threw"; write an honest note into the
  existing `analysisNote` card in both cases, and keep generation available (the text path still
  works). No fabricated analysis values.
- Regression test: JVM test for the note-mapping logic (pure Kotlin).

## AF-004 — No persistence across process death (medium)

- Evidence: `grep -rn "SharedPreferences|DataStore|Room|SQLite" app sdk` finds nothing. All
  state is Compose `remember { mutableStateOf(...) }`. `allowBackup="false"` and
  `configChanges` hide the problem during rotation, but Android killing the process (memory
  pressure while backgrounded) resets prompt, reference images, spec and renderer settings.
- Fix scope this session: persist `RenderSettings` (16 typed fields) via SharedPreferences with a
  pure-Kotlin codec that is JVM-unit-tested; load on viewer entry, save on every settings
  change. Prompt/photo persistence is deferred (bitmaps need a store design) and recorded here.
- Regression test: `RenderSettingsCodecTest` — encode/decode round trip, unknown-key tolerance,
  malformed-value tolerance, defaults when no prefs exist.

## AF-005 — Dead `engine/` module (low)

- Evidence: `engine/src/main/kotlin/com/avatarforge/engine/{math/Math3D,Noise}.kt` exists;
  `settings.gradle.kts` includes only `:sdk,:app`; no `com.avatarforge.engine` import anywhere
  in `app/` or `sdk/`. Nothing compiles it.
- Recommendation: do not delete (possible intended math donor); either register and test it or
  move it under `tools/`/docs as reference. Deferred to avoid scope churn.

## AF-006 — Rust workspace outside the build (low)

- Evidence: `rust/{af,af-core,af-sys}` with `Cargo.toml`; no reference in `settings.gradle.kts`,
  `native/CMakeLists.txt`, or any workflow. `tools/abi_conformance.py` documents it as an
  alternative ABI implementation, but nothing currently proves it conforms.
- Recommendation: add a cargo test job when a Rust toolchain is acceptable to CI; until then
  label it an alternate implementation, never a shipped one.

## AF-007 — Orphaned workflow registrations (low, BLOCKED)

- Evidence: `gh workflow list` shows `android-quality.yml`, `code-quality.yml`, `semgrep.yml`
  as active; the Contents API returns 404 for all three on the default branch and `git log --all`
  has no history of those paths. They cannot run.
- Blocker: removing server-side workflow registrations needs repo administration the current
  token lacks (dispatch already returned 403). Exact unblock: a maintainer deletes them in the
  Actions UI or re-adds real workflow files.

## AF-008 — Quality adaptation not explainable; no ADPF (medium)

- Evidence: `BudgetRenderer::updateBudget` (`native/src/render/af_render.cpp`) steps `tier_` on
  smoothed frame time with hysteresis (30 slow / 240 fast frames) but emits no event; only the
  current tier is readable in stats. No `PerformanceHintManager`, `getThermalHeadroom` or
  Game Mode API usage exists (`grep` empty); thermal input is a single
  `PowerManager.currentThermalStatus` poll in `DeviceResourceGovernor`.
- Consequence: users cannot see *why* quality changed; the OS gets no performance hints;
  headroom-based early adaptation (recommended by Google ADPF guidance) is unavailable.
- Fix this session: record tier transitions (tier, frame count) in `RendererDiagnostics` from
  the already-collected stats — no ABI change — and show them in the report. ADPF integration
  (API 31+, hint sessions, headroom polling ≤1 per 10 s) is a planned native follow-up.

## AF-009 — Release integrity blocked (medium)

- Evidence: `app/build.gradle.kts` signs `release` with `~/.android/debug.keystore` when
  `keystore.properties` is absent; no bundle task; no tags/releases exist. CI uploads APKs as
  build artifacts only.
- Status: BLOCKED — no signing material is available, and none may be committed. Exact unblock
  in RELEASE_CHECKLIST. No unsigned artifact is ever labelled signed.

## AF-010 — Vulkan mip-chain layout violation (historical, fixed)

- Evidence and fix: see `docs/VULKAN_MIP_FIX_2026-10-24.md`. Same-layout
  `TRANSFER_DST→TRANSFER_DST` barriers around `vkCmdBlitImage` violated the spec; strict
  AMD/Xclipse drivers could return undefined blit data, matching the observed rippling. Fixed
  in `bac2168`, prebuilts refreshed in `e9bc49b`, NDK compile green. Visual device retest
  remains UNVERIFIED.

## AF-011 — Diagnostics/telemetry defects (historical, fixed)

- Fixed in `9b0af6d`/`bac2168`: `withCounters()` extension-receiver shadowing zeroed every
  surface counter; the red failure card strobed during automatic fallbacks; every
  `surfaceChanged` rebuilt the swapchain even for duplicate/zero sizes.

## AF-012 — No instrumented device tests (low)

- Evidence: `find . -name "androidTest" -type d` is empty. All device behaviour (lifecycle,
  surface recreation, long-duration rendering, export round trip) is manual.
- Recommendation: add an `androidTest` module with a smoke Activity test and a GLB
  export/reimport test once a device farm or emulator runner is available; keep it out of the
  required PR gate until then, per the directive's hardware-test rule.

## AF-013 — No version catalog (low)

- Evidence: no `gradle/libs.versions.toml`; coordinates and Compose BOM versions are duplicated
  across `app/` and `sdk/` build files. Centralising is a mechanical, low-risk follow-up.

## AF-014 — White head cap on device (low, UNVERIFIED)

- Evidence: user screenshot `Screenshot_20261024_191611_Grok Faces.jpg` shows a white cap-like
  surface above the face. Hypotheses, in order to test after the mip fix: corrupted mip
  sampling (may already be resolved), hair-cap material/bake issue, or depth fallback. No code
  change is justified before the next device report.

## AF-016 — Shader-table harness drift: forward_plus compute stage unvalidated (medium, fixed)

- Detected by: the new `host-tests` CI job on run `40097344731` (the first run ever to execute
  the kernel harness in CI), step "Build and run the kernel host harness", exit 1.
- Evidence, reproduced locally (g++ 12.2 with a local zlib header/lib workaround):
  `FAIL host_tests.cpp:1245 every pipeline stage is baked into the SPIR-V table (13 of 12)`
  — 405 checks, exactly 1 failure. The `want[]` table listed 12 (name, stage) pairs while
  `afShaderCount()` returns 13.
- Root cause: the clustered forward+ light-culling compute shader
  (`{"forward_plus", AF_STAGE_COMP}`, required by `af_vk.cpp:1464`; present in both generated
  tables since base commit `40ef65b`) was added without updating the harness. Because no
  workflow ran the harness (AF-001), the drift survived until the gate existed.
- Why it matters: the harness comment is explicit that a missing shader is a hard renderer
  initialisation failure — a black viewport on a phone is the only other place to notice. An
  unvalidated table entry could silently corrupt or vanish.
- Fix (`af09371`): add the pair to `want[]`; the existing SPIR-V-magic and GLSL ES version
  assertions now cover all 13 entries. This extends coverage; no assertion was weakened.
- Verification: local full run `All 405 checks complete: 0 failure(s)`; CI run
  `40098617172` host-tests job **success** (all three jobs green, zero failure annotations).

## AF-015 — GLB import feature missing (medium)

- The native reader (`af_glb_read.cpp`, ~981 lines, bounds-checked, host-harness coverage for
  malformed files) and validator exist, but no JNI/Kotlin path or UI opens an external GLB.
  Implementing import is a feature project (scene attachment, materials, lifecycle), not a
  bug fix; the misleading intent filter (AF-002) is removed so the platform no longer
  advertises it.
