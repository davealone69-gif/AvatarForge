# Test matrix

What is actually verified, where, and with what evidence. Hardware-dependent layers are marked
UNVERIFIED rather than implied by CI.

## 1. Layered strategy

| Layer | Tests | Runner | Status at audit start | Status after this session |
|---|---|---|---|---|
| Static: ABI contract | `tools/abi_manifest.py` regeneration vs `abi/afcore.abi.json` | local Python + new CI `host-tests` job | manual only | CI-gated; local run showed no diff |
| Static: GLB validity | `tools/validate_glb.py` against sample export | local Python + CI `host-tests` | manual only | CI-gated; local run passes |
| Static: customization contracts | `tools/validate_custom_inputs.py` | manual | manual | added to CI `host-tests` |
| Native kernel (host) | `native/tests/host_tests.cpp` (439 checks: anatomy, SDF, isosurface, rig, blendshapes, GLB round trip, malformed GLB, shaders incl. all 13 baked table entries, and section [17] camera-UBO slot layout for AF-020) | `tools/build_host.sh test` (g++, zlib) | one-off record 2026-10-18; not in CI | CI-gated via `host-tests`; first gated run exposed AF-016, now fixed; 405 checks at run 40098617172, 439/439 local after the AF-020 addition |
| Native ABI conformance | `tools/abi_conformance.py` (symbols/layout/behaviour vs manifest) | Python+ctypes against a built lib | manual | wired into `host-tests` against the host build |
| Kotlin/JVM SDK | JUnit 4, 28 tests in 6 classes under `sdk/src/test` (adult safety, analysis packing, analysis notes, settings codec, diagnostics report, diagnostic-note rules) | `:sdk:testDebugUnitTest` | not run by any workflow | added to APK job; 21-test set green on 40097344731, full 28-test set green on 40107047136 |
| Kotlin/Native integration | engine lifecycle through JNI | requires device/emulator | MISSING | MISSING (AF-012) |
| Renderer | device frame/swapchain behaviour | requires device | UNVERIFIED except one manual report | unchanged; manual checklist in RENDERER_AUDIT |
| UI/flows | Compose screens, intents, SAF export | requires device | MISSING | MISSING (AF-012) |
| Long-run/thermal/lifecycle | 30-min render, backgrounding, process death | requires device | MISSING | MISSING |
| Security | Semgrep workflow registered server-side | phantom workflow, file absent (AF-007) | not running | BLOCKED on maintainer |

## 2. Regression tests added this session

| Test | Defect it locks | Type |
|---|---|---|
| `RenderSettingsCodecTest` | AF-004 persistence round trip, corrupt-value tolerance, unknown-key forward compatibility | pure JVM JUnit |
| `ImageAnalysisNotesTest` (note mapping logic) | AF-003 silent analysis failure | pure JVM JUnit |
| `DiagnosticsReportTest` (structure/worst-status/plain-text) | diagnostics drift between panel and clipboard | pure JVM JUnit |
| CI `host-tests` job | AF-001 tests never executing | workflow gate |
| CI `:sdk:testDebugUnitTest` | AF-001 | workflow gate |
| `testCameraUboSlotLayout` — harness section [17], 34 checks over `af::afRoundUp`, the 16–4096 alignment table and the frame-1 containment bound | AF-020 camera UBO double-buffer overrun (GLES + latent Vulkan) | host C++ (pure layout maths; renderer TUs themselves remain NDK/CI-only) |

## 3. Device test backlog (exact procedures; SM-S921B evidence dated 2026-10-24)

Evidence key: **[device 2026-10-24]** = observed in `artifacts/audits/device-report-SM-S921B-2026-10-24.txt`;
**[device 2026-10-25]** = the two reports in `artifacts/audits/device-report-SM-S921B-2026-10-25.txt`
(both still app 0.10.1 (2), pre-AF-020-fix). Everything else remains pending. The reporting
build's source commit is not recorded by the reports, so even observed rows describe the
device, not a specific commit.

1. Fresh install of the CI artifact; verify version row 0.10.1 (2), no INTERNET permission.
   **[device 2026-10-24]** version row matches; no-INTERNET still relies on CI aapt gate.
2. Generate from text; wait to viewer; confirm first frame within 5 s and no red card.
   **[device 2026-10-24]** RUNNING, 1 attempt / 264 ms, no FAIL/WARN rows, 58 FPS — first-frame
   wall-clock was not measured by the user, so the "within 5 s" clause stays open.
3. Generate from two reference photos; force an analysis failure (e.g. corrupt image picker
   output via a file manager) and confirm the new visible note appears and generation continues.
4. Armor avatar, orbit 60 s at each zoom extreme; capture `diag` report — ripples must be
   absent (AF-010 retest), surface counters non-zero, depth format recorded.
   **[device 2026-10-24]** counters non-zero and healthy at 58 FPS via GLES/ANGLE; ripple check
   is visual and still open, and the Vulkan mip path was not initialised (AF-018).
   **[device 2026-10-25]** blocked: all newer GLES/ANGLE frames show the full-screen atlas
   defect AF-020 instead of the body, so ripples cannot be re-observed on current builds.
5. Kill the app in background with the viewer open; relaunch — render settings must be restored
   (AF-004); prompt/photos resetting is the documented remaining gap.
6. Rotate/background/foreground 10×; expect zero or explained surface recreate counts and no
   crash. **[device 2026-10-24]** single clean open: created 1 / changed 1 / destroyed 0; the
   four frame-loop restarts are source-traced as benign (AF-017).
7. 30-minute continuous render with clips playing; record thermal row, tier transitions, FPS
   distribution, memory. **[device 2026-10-24]** spot sample only: thermal light, tier held 2
   (no transitions to observe), 1236 MB available; the 30-minute run remains pending.
8. Export via SAF (clean install, no grants), share via FileProvider to another app, re-open the
   GLB with `validate_glb.py` after transfer.
9. Toggle Vulkan off and repeat 2–4 to exercise the GLES path.
   **[device 2026-10-24]** GLES/ANGLE path exercised in practice (via automatic fallback, not
   the toggle); the explicit-toggle flow itself remains untested.
10. Attempt to open a `.glb` with the app from a file manager: the dead entry must no longer
    appear (AF-002); when import ships (AF-015), this becomes a positive test.
11. AF-020 regression + localization: install a build containing the camera-UBO slot fix,
    regenerate, and confirm a 3D body is visible instead of the full-screen atlas. If the atlas
    persists, run the seven-step shipped-controls ladder (exposure, ground plane, shadows, sun,
    bloom, SSS, Vulkan retry) in `artifacts/audits/device-evidence-atlas-2026-10-25.md` §6,
    screenshotting each state; only then re-score AF-010 and AF-014.
12. AF-020 cross-GPU isolation (from the 2026-10-25 external review, accepted): repeat step 11
    on a non-ANGLE device — a Snapdragon/Adreno Android device with native GLES and, if
    possible, a successful native Vulkan init — plus any desktop/Vulkan validation environment.
    Correct rendering elsewhere separates an ANGLE/Xclipse translation fault from a generic
    GL-path bug. Two suggested shortcuts are deliberately NOT available: GLES 3.0 is hard-
    rejected by the ES 3.1 gate (`af_gl.cpp:278`), and external `.glb` loading does not exist
    (AF-015); use a simpler app-generated avatar instead.

## 4. Rules for future tests

- Every regression test names its defect ID (the table above is the index).
- Hardware-only tests never block hosted CI; they live here and in `tools/device_check.sh`.
- A test is never weakened to go green; disablement requires a documented reason and linked
  finding.
