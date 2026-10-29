# Test matrix

| Layer | Test | Defect tied | Current result |
|---|---|---|---|
| Native GLES host mock | `tools/test_renderer_host.sh` | AF-002/003 frame error/counters/cleanup | REAL + VERIFIED: 53 checks, 0 failures |
| Native kernel host | `build-host/host_tests` | kernel regressions, import limits compile | REAL + VERIFIED: 406 checks / 1 pre-existing SPIR-V table-count failure |
| SDK JVM | `:sdk:testDebugUnitTest` | AF-004/005/006 thermal, settings, progress | REAL + VERIFIED: CI run 40858597544 passed |
| Android CI | `.github/workflows/android-apk.yml` | AF-001/009 signing/AAB/shader staleness | REAL + VERIFIED: run 40858597544 passed; SPIR-V drift warning recorded |
| Device camera | SM-S921B drag/pinch/chips/sliders | AF-010 | UNVERIFIED |
| Device renderer | Vulkan/GLES startup, resize, background, safe recovery | AF-002/016 | UNVERIFIED |
| Device visual | moiré, hands, eyes, hair, garment poke-through | AF-011 + quality defects | UNVERIFIED |
| Release | signed AAB/APK, package/version/signature/ABI/checksums | AF-001 | BLOCKED: no production credentials |

Hardware-only tests are not required on hosted runners. Host mocks prove API sequencing and cleanup, not pixels.
