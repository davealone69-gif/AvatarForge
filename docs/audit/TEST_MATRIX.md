# Test matrix

| Layer | Test | Defect tied | Current result |
|---|---|---|---|
| Native GLES host mock | `tools/test_renderer_host.sh` | AF-002/003 frame error/counters/cleanup/C ABI parity | REAL + VERIFIED: 57 checks, 0 failures |
| Native model detail host | `tools/test_model_host.sh` | AF-017 palm/garment; AF-019 eye labels/extraction symmetry, chest shell, hair eye-line | REAL + VERIFIED: 33 checks, 0 failures |
| Native kernel host | `build-host/host_tests` | kernel regressions, import limits, GLES shader count | REAL + VERIFIED: 406 checks, 0 failures; previous SPIR-V count failure is obsolete |
| SDK JVM | `:sdk:testDebugUnitTest` | AF-004/005/006 thermal, settings, progress; GLES-only recovery | REAL + VERIFIED: CI run 40867172914 passed |
| Android CI | `.github/workflows/android-apk.yml` | AF-001/009/014 signing/AAB/shader staleness/GLES-only linkage | REAL + VERIFIED: run 40867172914 passed on `1fcbe6c`, including model-detail and full kernel-host gates |
| Device camera | SM-S921B drag/pinch/chips/sliders | AF-010 | PARTIAL: user confirms camera works; exact build SHA/counters not supplied |
| Device renderer | GLES startup, resize, background, safe recovery | AF-002/016 | UNVERIFIED for new GLES-only build |
| Device visual | eyes, hair, chest plate, hands, garment poke-through, header occlusion, moiré | AF-011/017/018/019 | UNVERIFIED for the fixed build; current device capture is pre-fix |
| Release | signed AAB/APK, package/version/signature/ABI/checksums | AF-001 | BLOCKED: no production credentials |

Hardware-only tests are not required on hosted runners. Host mocks prove API sequencing and cleanup, not pixels.
