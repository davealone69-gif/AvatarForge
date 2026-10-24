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
| AF-017 | low | diagnostics | Frame-loop restart hint over-warns on benign reschedules | FIXED + 3 JVM tests, CI-green on run 40107047136; device re-observation pending |
| AF-018 | low | diagnostics | Silent Vulkan→GLES fallback: "Vulkan preferred: OK" beside a gles31 backend, reason invisible | PARTIAL: Kotlin-side fallback row FIXED + 4 JVM tests, CI-green 40107047136; native reason surfacing QUEUED (needs renderer ABI work, see AF-019) |
| AF-019 | low | native/ABI | Renderer C API (`af_render_capi.cpp`) has no public header, manifest or conformance gate | CONFIRMED, deferred; blocks the native half of AF-018 |
| AF-020 | high | renderer/gles | GLES/ANGLE frames show the packed base-color texture atlas full-screen instead of the skinned 3D scene; double-buffered camera UBO also overruns its buffer | CONFIRMED visually; camera UBO overflow FIXED (`1944aee`; prebuilt refreshed in `354cc53`), 439/439 host checks locally, NDK+host+APK CI green on runs `40115970765` and `40116499207` (branch tip), device effect UNVERIFIED; atlas mechanism still PARTIAL — see section |
| AF-009 | medium | release | No AAB; release APK uses debug certificate | BLOCKED (credentials) |
| AF-010 | — (historical) | renderer/vulkan | Mip-chain image-layout spec violation caused surface rippling | FIXED in code; UNVERIFIED visually — Vulkan path not initialised on current builds, and all GLES/ANGLE frames are blocked by AF-020 |
| AF-011 | — (historical) | diagnostics | Counter shadowing, red-card strobe, redundant resizes | FIXED (commits `9b0af6d`/`bac2168`) |
| AF-012 | low | test strategy | No instrumented/device tests exist | CONFIRMED, roadmap |
| AF-013 | low | deps | No version catalog; dependency coordinates hardcoded | CONFIRMED, deferred |
| AF-014 | low | renderer | White head cap in device screenshot | UNVERIFIED hypothesis; only visible on the older Vulkan-labelled build — every newer GLES/ANGLE build is blocked by AF-020 |
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
  at the first green run, 28 as of run `40107047136`). The gate found a real defect on its
  first run — AF-016.
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
  in `bac2168`, prebuilts refreshed in `e9bc49b`, NDK compile green.
- Device evidence 2026-10-24 (`artifacts/audits/device-report-SM-S921B-2026-10-24.txt`): the
  SM-S921B now renders healthily at 58 FPS / 17.2 ms with zero failures, but **via the GLES
  3.1/ANGLE path** — native Vulkan initialisation was declined (see AF-018), so the Vulkan
  blit/mip code path is not exercised by this report. The mip fix itself therefore stays
  UNVERIFIED; a visual check for rippling on this session's build and any future Vulkan-active
  session is still required. The EGL surface-format/geometry half of the renderer recovery is
  positively exercised (context + window surface + first frame succeeded).
- 2026-10-25 update: every subsequent GLES/ANGLE screenshot shows the full-screen atlas defect
  AF-020 rather than the avatar, so the rippling cannot be re-observed on current builds either.
  AF-010 remains UNVERIFIED and is blocked on AF-020 being resolved (or a Vulkan-active session).

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
- 2026-10-24 status: the numeric report cannot see pixels, so the cap is not resolved or
  disproven; still needs a fresh screenshot from a current build.
- 2026-10-25 status: the cap is still visible on the same older Vulkan-labelled frame, but all
  three newer GLES/ANGLE builds display only the texture atlas (AF-020), so no current build can
  confirm or refute the cap. Stays UNVERIFIED, blocked behind AF-020; no code change justified.

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

## AF-017 — Frame-loop restart hint misclassifies benign reschedules (low, fix in flight)

- Detected from: the 2026-10-24 SM-S921B device report (`artifacts/audits/device-report-SM-S921B-
  2026-10-24.txt`): `Frame loop restarts: 4` with surface created 1 / changed 1 / destroyed 0
  and one successful create attempt, yet the report carried the "can look like flashing" hint.
- Source trace: the counter increments in exactly three places in `RenderSurfaceView` —
  `setFrameLoopEnabled(true)` (view attach visibility + the viewer's explicit enable),
  successful `attemptCreate()`, and a real `surfaceChanged` resize. A clean viewer open fires
  each once (attach + explicit = 2, create = 1, resize = 1 → 4). None of these rebuild a
  swapchain except the resize, which happened exactly once and was expected.
- Defect: the old `> 3` rule warned about flashing without checking whether any surface was
  actually recreated — an alarming but false signal on the healthiest possible open.
- Fix: pure-Kotlin `RendererDiagnosticNotes.frameLoopRestartHint` distinguishes a stable
  surface (0 destroys, ≤1 create, ≤1 attempt → benign explanation) from real churn (flashing
  warning retained). Wording locked by 3 JVM tests in `RendererDiagnosticNotesTest`.
- Verification: 3 JVM tests green in CI on run `40107047136` (a compile-only smart-cast error
  first failed run `40106628640`, fixed in `d5e397a`); device re-observation of the new wording
  on a clean open remains UNVERIFIED.

## AF-018 — Silent Vulkan→GLES fallback makes "Vulkan preferred: OK" misleading (low, PARTIAL)

- Detected from: the same device report pairs `Attempt 1 (Vulkan preferred): OK in 264 ms`
  with backend `gles31 · ANGLE ... on Vulkan 1.3.304`.
- Source trace: `Renderer::create` (`native/src/render/af_render.cpp:166-176`) attempts
  `createVulkanRenderer` when `preferVulkan` is set, and on null silently retries GLES; the
  `vulkanError` string is only surfaced if BOTH backends fail. The Kotlin "attempt" records the
  whole sequence as one successful call, so no layer records *why* Vulkan was declined.
- Why it matters: readers reasonably infer Vulkan is driving the GPU; in fact the system GLES
  driver (ANGLE-on-Vulkan) is. It also means the Vulkan mip fix (AF-010) can sit unexercised
  while reports look Vulkan-green.
- Fix this session (Kotlin side): `RendererDiagnosticNotes.backendFallbackItem` adds an INFO
  "Backend selection" row whenever a Vulkan-preferred attempt coexists with a non-Vulkan active
  backend, stating the observed fallback and that the rejection reason is not exposed — no
  fabricated reason. Covered by 4 JVM tests.
- Queued native follow-up: retain `vulkanError` in `RendererInfo` through the renderer C ABI
  and JNI; that surface currently has no ABI gate (AF-019), so do it together with one.

## AF-019 — Renderer C API lacks manifest/conformance coverage (low)

- Evidence: `tools/abi_manifest.py` reads only `native/include/afcore.h`;
  `abi/afcore.abi.json` contains 0 renderer functions (verified: `render` name filter is
  empty across 47 entries). `af_render_capi.cpp` defines its own structs (e.g.
  `af_render_info`) inside the .cpp with no public header, and is reached only via JNI. The
  host harness covers shader tables but not the renderer C ABI layout/symbols.
- Consequence: the native half of AF-018 (adding a fallback-reason field) would change an
  unguarded struct; a layout mismatch between native and JNI would fail only on a device.
- Recommendation: extract a public `native/include/afrender.h`, extend the manifest generator
  (or add a sibling manifest + conformance job) before extending the renderer info struct.

## AF-020 — Full-screen texture atlas on GLES/ANGLE instead of the skinned scene (high, CONFIRMED visually; mechanism PARTIAL)

- Detected from: four 1080×2340 device screenshots taken 2026-10-24/25 on the SM-S921B
  (Xclipse 940, ANGLE 256.4 / `9fe6623ec0db` on Vulkan 1.3.304), archived under
  `artifacts/audits/screenshots/` with a per-file ledger in
  `artifacts/audits/device-evidence-atlas-2026-10-25.md`.
- What is on screen (confirmed by pixel analysis, not eyeballing): on the three newer GLES
  builds (`230452`, `000636`, `000843`) the render area is the generated avatar's packed
  base-color atlas stretched over the whole surface. In `000843`, 65.1 % of render-area pixels
  are the atlas packer's neutral gray (mean RGB ≈ 124,123,123) while only 1.4 % match the
  configured clear colour (14,19,28) — a rasterised mesh on a cleared background cannot produce
  that. The five baked textures were extracted from `artifacts/avatarforge-sample.glb`; the
  baseColor PNG (2048², gray packer background, hair-dot band, armour/cloth islands) matches the
  screen content structurally. The frame stats still report 15 draw calls / ~417–426 k tris, so
  geometry is *submitted*; the displayed image is texture content regardless.
- Scope: present at both adaptive quality tiers 2 and 1, so the `targetsDirty_` rebuild cycle is
  not the trigger. The older `191611` build (renderer card labelled "vulkan", 18 s generation)
  shows the proper 3D body instead — the defect is specific to the GLES/ANGLE execution path
  (all rendering on this device is in fact GLES31/ANGLE; the card label is recorded verbatim).
  Commit `8dab4d1` made the GLES path initialise on ANGLE for the first time, so a long-standing
  latent GLES defect only became reachable then; the commit itself does not touch the draw, UBO
  or texture paths.
- Concrete code defect found and fixed this session — double-buffered camera UBO overrun
  (confirmed by arithmetic; its causal link to the atlas image is **unproven**, do not call it
  the fix for the visual defect):
  - `CameraUbo` is 448 bytes (`af_render_common.h:135` static_assert). The GLES camera buffer was
    allocated `uboAlignment_ * 2` (512 bytes at the common 256-byte alignment) while each frame
    slot used `frame * uboAlignment_` as its stride. Frame 1 therefore uploaded/bound the range
    [256, 704) inside a 512-byte buffer — 192 bytes out of range at both `glBufferSubData`
    (`af_gl.cpp`, `updateScene`) and `glBindBufferRange` (`drawForwardPass`). Strict GLES raises
    GL errors and an invalid binding rather than corrupting adjacent memory.
  - The Vulkan backend had the same latent pattern (`camSize = uboAlignment_ * kFramesInFlight`,
    memcpy and seven dynamic-offset sites); it rendered acceptably only because the camera
    matrices are the block's first members and stay inside slot 0.
  - Fix: new `af::afRoundUp()` helper in the dependency-free `native/src/render/af_align.h`;
    each backend computes `cameraSlotSize_ = afRoundUp(sizeof(CameraUbo), uboAlignment_)`,
    allocates two padded slots (1024 bytes at 256 alignment) and uses the slot stride everywhere
    a frame offset is consumed. Host harness section [17] (34 checks) pins the helper, the
    alignment table 16..4096 and the exact old-overrun bound; local host run green. CI and an
    on-device re-observation are still required (host cannot compile EGL/Vulkan TUs).
- Ruled out by source trace this/previous sessions (do not re-pursue without new evidence):
  intentional atlas/debug screen (no such path; overlays are skeleton/flat/highlight only);
  vertex-attribute or UV swap (locations, offsets and the 88-byte stride match across
  `forward.vert`, `af_gl.cpp`, `af_render_common.h`, `afcore.h` and `af_vk.cpp`); GLES embedded
  shader location stripping (qualifiers retained in `af_shaders_gl.h`); sampler-unit collision
  (five atlas textures use units 0–4, post uses 6/7); composite wrong-texture binding
  (`drawComposite` binds `hdrTexture_`); FBO/texture name aliasing in
  `makeFramebufferWithTexture`; tier-change target rebuild; VBO upload (straight
  `af_engine_vertices` copy, same source the Vulkan path renders correctly).
- Remaining hypotheses, ranked, for the next instrumented device session: (H1) the failed
  camera-buffer binding on odd frames puts ANGLE into an error state that changes later draw
  semantics — the fixed UBO bug is the only current candidate, hence the fix; (H2) the MSAA
  resolve blit or HDR target format behaves differently under ANGLE, leaving the composite
  fullscreen triangle sampling undefined/atlas-named storage; (H3) an ANGLE-specific descriptor
  or program state issue outside the code paths traced so far. No GL error log is obtainable
  from the current build, so H1–H3 cannot be discriminated from source alone.
- Next-device deterministic ladder (uses only shipped controls; details and expected
  discriminations in `artifacts/audits/device-evidence-atlas-2026-10-25.md`): move the Exposure
  slider, toggle Ground plane / Shadows / Bloom / SSS, move the Sun sliders, toggle Prefer
  Vulkan, and screenshot each state. Whether the atlas image responds to composite-only vs
  forward-only controls localises the fault without any new build.
- Latent neighbour risk noted, not changed: post-process slices (`PostUbo`, 32 bytes) use the
  raw `uboAlignment_` stride in both backends; a driver ever reporting an alignment below 32
  would overlap slices. Android devices in the field report 64–256 today; revisit together with
  any post-process defect rather than speculatively.

## AF-015 — GLB import feature missing (medium)

- The native reader (`af_glb_read.cpp`, ~981 lines, bounds-checked, host-harness coverage for
  malformed files) and validator exist, but no JNI/Kotlin path or UI opens an external GLB.
  Implementing import is a feature project (scene attachment, materials, lifecycle), not a
  bug fix; the misleading intent filter (AF-002) is removed so the platform no longer
  advertises it.
