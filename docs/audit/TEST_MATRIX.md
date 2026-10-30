# Test matrix

## Current exact working-tree pass — 2026-10-30

| Gate | Result | Limit |
|---|---|---|
| Kernel / UV / animation / ABI host harness | **423 checks / 0 failures** | CPU geometry and ABI invariants; not Samsung pixels |
| Renderer fault injection | **87 / 0** | GLES/EGL mock and factory-only Vulkan double; no Vulkan commands or GPU presentation |
| Vulkan API-version helper | **7 / 0** | Version-selection helper only; no device query |
| Model-detail regressions | **33 / 0** | Host geometry checks; no visual device approval |
| Release-workflow contract | **2 static checks pass; exact-head CI run 41002485082 succeeded** | Signed-tag production release still requires credentials |
| GLES shaders | **12 stages compiled; table matches** | Compiler proof, not runtime shader/pixel proof |
| Vulkan shaders | **13 sources compiled and 13 modules pass `spirv-val` for `vulkan1.3`; 6 regression tests / 0 failures** | No GPU or driver execution |
| ABI / Rust parity | Manifest **47 symbols / 11 structs / 12 enums**; C++ **47/47**; Rust **15/47**; prompt parity **72/72** | 32 Rust ABI functions remain unimplemented |
| Rust default workspace | **37 passed / 0 failed; 2 end-to-end tests ignored** | Ignored tests require an explicit full avatar build |
| Rust optional renderer feature | **7/7 unit tests; E0505 compile-fail doctest 1/1** | Rust type/lifetime proof only; `libafrender.so` was not linked or executed |
| Android / SDK | **3 app + 38 SDK JVM tests pass; ARM64 native build and compile-only APK/AAB/AAR succeed; exact-head CI passed** | No install or physical-device evidence |
| Local repository artifact scan | **SKIP; CI stage/verify/upload passed** | No APK/AAB/AAR staged in local `artifacts/`; CI artifact `12954506148` is the uploaded deliverable |
| Rust formatting | **FAIL: workspace `cargo fmt --all -- --check` reports broad formatting diffs** | No mass reformat was applied; formatting cleanup is separate from functional fixes |
| SM-S921B runtime, animation sharpness, fallback and thermal soak | **UNVERIFIED** | Requires the exact-head APK and physical-device evidence |
| Production signed release | **BLOCKED** | Production signing credentials unavailable; unsigned compile artifact is not a release |

The final post-fix unified `tools/verify.sh` completed with exit code 0: all host-side checks passed, including the kernel, renderer and Vulkan-version gates, model regressions, workflow contract, shader validation, ABI/Rust checks and GLB export validation. Its repository artifact scan skipped `artifacts/` because no APK/AAB/AAR was staged; exact-head Android APK workflow run 41002485082 later staged, verified and uploaded its artifact on commit `401a005`. No device pass is claimed. See `VERIFICATION_RESULTS.md` for commands, source-version distinctions and evidence boundaries.

The successful CI run emitted non-blocking runner warnings about Node.js 20 being forced to Node.js 24 and `actions/setup-java@v4` being deprecated; these did not fail the workflow.

## Historical matrix — preceding GLES-only branch

| Layer | Test | Defect tied | Current result at that historical checkpoint |
|---|---|---|---|
| Native GLES host mock | `tools/test_renderer_host.sh` | AF-002/003 frame error/counters/cleanup/C ABI parity; AF-025 option sanitisation | REAL + VERIFIED: **64 checks, 0 failures** (2026-10-29, up from 57) |
| Native model detail host | `tools/test_model_host.sh` | AF-017 palm/garment; AF-019 eye labels/extraction symmetry, chest shell, hair eye-line | REAL + VERIFIED: 33 checks, 0 failures |
| Pose cost (host, measured not asserted) | `/home/user/fault-review/probe/pose_time.cpp` (throwaway) | AF-035 skinning matrix hoist, cofactor normals, fused vertex write | REAL + VERIFIED on host: `af_engine_apply_clip` **10.14 ms → 4.65 ms** per frame for 266 212 vertices / 367 698 triangles. Device frame rate still **UNVERIFIED** |
| Native kernel host | `build-host/host_tests` | kernel regressions, import limits, GLES shader count, AF-034 atlas overflow | REAL + VERIFIED: **411 checks, 0 failures** (2026-10-30; superseded by the current 423-check run) |
| SDK JVM | `:sdk:testDebugUnitTest` | AF-004/005/006 thermal, settings, progress; earlier GLES-only recovery; AF-025 codec clamping | REAL + VERIFIED by CI at that earlier checkpoint; current working-tree JVM tests are recorded above |
| Android CI | `.github/workflows/android-apk.yml` | AF-001/009/014 signing/AAB/shader staleness/GLES-only linkage | REAL + VERIFIED: run 40895347516 passed on `39c8cf6`; historical, not current Vulkan-restoration CI |
| Device camera | SM-S921B drag/pinch/chips/sliders | AF-010 | PARTIAL: user confirms camera works; exact build SHA/counters not supplied |
| Device renderer | GLES startup, resize, background, safe recovery | AF-002/016 | UNVERIFIED at that GLES-only checkpoint |
| Concurrency (source only) | `KernelAccess` fence: UI/render/generation ownership | AF-020 | PARTIAL: source and compile proof; race-free device soak required |
| Device visual | eyes, hair, chest plate, hands, garment poke-through, header occlusion, moiré | AF-011/017/018/019 | UNVERIFIED at that checkpoint |
| Release | signed AAB/APK, package/version/signature/ABI/checksums | AF-001 | BLOCKED: no production credentials |

Hardware-only tests are not required on hosted runners. Host mocks prove API sequencing and cleanup, not pixels.
