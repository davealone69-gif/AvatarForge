# Test matrix

What is actually verified, where, and with what evidence. Hardware-dependent layers are marked
UNVERIFIED rather than implied by CI.

## 1. Layered strategy

| Layer | Tests | Runner | Status at audit start | Status after this session |
|---|---|---|---|---|
| Static: ABI contract | `tools/abi_manifest.py` regeneration vs `abi/afcore.abi.json` | local Python + new CI `host-tests` job | manual only | CI-gated; local run showed no diff |
| Static: GLB validity | `tools/validate_glb.py` against sample export | local Python + CI `host-tests` | manual only | CI-gated; local run passes |
| Static: customization contracts | `tools/validate_custom_inputs.py` | manual | manual | added to CI `host-tests` |
| Native kernel (host) | `native/tests/host_tests.cpp` (405 checks: anatomy, SDF, isosurface, rig, blendshapes, GLB round trip, malformed GLB, shaders incl. all 13 baked table entries) | `tools/build_host.sh test` (g++, zlib) | one-off record 2026-10-18; not in CI | CI-gated via `host-tests`; first gated run exposed AF-016, now fixed; local 405/405 and run 40098617172 green |
| Native ABI conformance | `tools/abi_conformance.py` (symbols/layout/behaviour vs manifest) | Python+ctypes against a built lib | manual | wired into `host-tests` against the host build |
| Kotlin/JVM SDK | JUnit 4, 21 tests in 5 classes under `sdk/src/test` (adult safety, analysis packing, analysis notes, settings codec, diagnostics report) | `:sdk:testDebugUnitTest` | not run by any workflow | added to APK job and green on run 40097344731 |
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

## 3. Device test backlog (exact procedures, no device available here)

1. Fresh install of the CI artifact; verify version row 0.10.1 (2), no INTERNET permission.
2. Generate from text; wait to viewer; confirm first frame within 5 s and no red card.
3. Generate from two reference photos; force an analysis failure (e.g. corrupt image picker
   output via a file manager) and confirm the new visible note appears and generation continues.
4. Armor avatar, orbit 60 s at each zoom extreme; capture `diag` report — ripples must be
   absent (AF-010 retest), surface counters non-zero, depth format recorded.
5. Kill the app in background with the viewer open; relaunch — render settings must be restored
   (AF-004); prompt/photos resetting is the documented remaining gap.
6. Rotate/background/foreground 10×; expect zero or explained surface recreate counts and no
   crash.
7. 30-minute continuous render with clips playing; record thermal row, tier transitions, FPS
   distribution, memory.
8. Export via SAF (clean install, no grants), share via FileProvider to another app, re-open the
   GLB with `validate_glb.py` after transfer.
9. Toggle Vulkan off and repeat 2–4 to exercise the GLES path.
10. Attempt to open a `.glb` with the app from a file manager: the dead entry must no longer
    appear (AF-002); when import ships (AF-015), this becomes a positive test.

## 4. Rules for future tests

- Every regression test names its defect ID (the table above is the index).
- Hardware-only tests never block hosted CI; they live here and in `tools/device_check.sh`.
- A test is never weakened to go green; disablement requires a documented reason and linked
  finding.
