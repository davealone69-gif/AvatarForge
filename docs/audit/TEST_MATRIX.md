# Test matrix

| Layer | Test | Defect tied | Current result |
|---|---|---|---|
| Native GLES host mock | `tools/test_renderer_host.sh` | AF-002/003 frame error/counters/cleanup/C ABI parity | REAL + VERIFIED: 57 checks, 0 failures |
| Native model detail host | `tools/test_model_host.sh` | AF-017 palm frame, garment layer/poke-through | REAL + VERIFIED: 19 checks, 0 failures |
| Native kernel host | `build-host/host_tests` | kernel regressions, import limits, GLES shader count | REAL + VERIFIED: 406 checks, 0 failures; previous SPIR-V count failure is obsolete |
| SDK JVM | `:sdk:testDebugUnitTest` | AF-004/005/006 thermal, settings, progress; GLES-only recovery | REAL + VERIFIED: CI run 40864270280 passed |
| Android CI | `.github/workflows/android-apk.yml` | AF-001/009/014 signing/AAB/shader staleness/GLES-only linkage | REAL + VERIFIED: run 40864270280 passed on `973f70f`; model-detail CI gate added afterwards |
| Device camera | SM-S921B drag/pinch/chips/sliders | AF-010 | PARTIAL: user confirms camera works; exact build SHA/counters not supplied |
| Device renderer | GLES startup, resize, background, safe recovery | AF-002/016 | UNVERIFIED for new GLES-only build |
| Device visual | hands, garment poke-through, header occlusion, eyes/hair/moiré | AF-011/017/018 | UNVERIFIED for new build |
| Release | signed AAB/APK, package/version/signature/ABI/checksums | AF-001 | BLOCKED: no production credentials |

Hardware-only tests are not required on hosted runners. Host mocks prove API sequencing and cleanup, not pixels.
