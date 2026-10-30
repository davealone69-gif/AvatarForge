# Test matrix

## Current branch — 2026-10-30

| Gate | Result | Limit |
|---|---|---|
| Host kernel/UV/clip | **411 checks / 0 failures** | CPU and atlas invariants; not Samsung pixels |
| GLES mocks + Vulkan selector/version gate | **87 / 0** | 1.0/1.2 rejected; 1.3+ accepted in host policy. No native Vulkan commands or GPU presentation |
| Dual-API package guard unit tests | **3 / 0** | APK linkage still requires exact-head CI |
| Vulkan SPIR-V provenance regressions | **6 / 0** | No local NDK `glslc` or `spirv-val`; CI compiles/validates |
| Vulkan translation-unit syntax | **pass** with fetched headers | Not an NDK build or real GPU driver |
| Android CI and SDK JVM | **check current commit's Android APK workflow run** | Shader-toolchain mismatch is handled by pinned source/table hashes plus SPIR-V validation; still must pass before APK delivery |
| Live animation sharpness, startup, recovery | **UNVERIFIED** | Requires exact-head APK on SM-S921B |

## Historical matrix — preceding GLES-only branch

| Layer | Test | Defect tied | Current result |
|---|---|---|---|
| Native GLES host mock | `tools/test_renderer_host.sh` | AF-002/003 frame error/counters/cleanup/C ABI parity; AF-025 option sanitisation | REAL + VERIFIED: **64 checks, 0 failures** (2026-10-29, up from 57) |
| Native model detail host | `tools/test_model_host.sh` | AF-017 palm/garment; AF-019 eye labels/extraction symmetry, chest shell, hair eye-line | REAL + VERIFIED: 33 checks, 0 failures |
| Pose cost (host, measured not asserted) | `/home/user/fault-review/probe/pose_time.cpp` (throwaway) | AF-035 skinning matrix hoist, cofactor normals, fused vertex write | REAL + VERIFIED on host: `af_engine_apply_clip` **10.14 ms → 4.65 ms** per frame for 266 212 vertices / 367 698 triangles. Device frame rate still **UNVERIFIED** |
| Native kernel host | `build-host/host_tests` | kernel regressions, import limits, GLES shader count, **AF-034 atlas overflow (UV density percentiles + zero-texel footprint) added 2026-10-30** | REAL + VERIFIED: **411 checks, 0 failures** (2026-10-30; was 406) |
| SDK JVM | `:sdk:testDebugUnitTest` | AF-004/005/006 thermal, settings, progress; GLES-only recovery; AF-025 codec clamping; stale Vulkan key ignored | REAL + VERIFIED by CI on the exact head of this pass (two new codec cases); local JVM execution unavailable (no JDK) |
| Android CI | `.github/workflows/android-apk.yml` | AF-001/009/014 signing/AAB/shader staleness/GLES-only linkage | REAL + VERIFIED: run 40895347516 passed on `39c8cf6`, including model-detail and full kernel-host gates |
| Device camera | SM-S921B drag/pinch/chips/sliders | AF-010 | PARTIAL: user confirms camera works; exact build SHA/counters not supplied |
| Device renderer | GLES startup, resize, background, safe recovery | AF-002/016 | UNVERIFIED for new GLES-only build |
| Concurrency (source only) | `KernelAccess` fence: UI/render/generation ownership | AF-020 | PARTIAL: single-lock design reviewed and compiled by CI; a data race is not observable in a JVM test, so it needs a device soak |
| Settings input | `RenderSettingsCodecTest` malformed/out-of-range cases | AF-025 | REAL + VERIFIED by CI (JVM) |
| Device visual | eyes, hair, chest plate, hands, garment poke-through, header occlusion, moiré | AF-011/017/018/019 | UNVERIFIED for the fixed build; current device capture is pre-fix |
| Release | signed AAB/APK, package/version/signature/ABI/checksums | AF-001 | BLOCKED: no production credentials |

Hardware-only tests are not required on hosted runners. Host mocks prove API sequencing and cleanup, not pixels.
