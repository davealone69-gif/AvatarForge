# Verification results

Evidence ledger for the audit remediation. Nothing in this document is inferred from source
reading: every entry names the command or CI run that produced it. Layers that cannot be
exercised in this environment are listed as UNVERIFIED with the exact procedure that would
verify them.

Status vocabulary (mandated): **REAL + VERIFIED**, **UNVERIFIED**, **PARTIAL**, **MISSING**,
**BLOCKED**.

## 1. Environment and tool limits

| Capability | Available in this sandbox? | Consequence |
|---|---|---|
| JDK / Android SDK / NDK / cmake / ninja | No | No local Gradle compile, unit-test run or APK build; all Kotlin verification happens in CI |
| g++ 12.2 | Yes | Kernel harness can build locally with a zlib workaround (below) |
| zlib dev headers / `libz.so` symlink | Header absent from standard paths; `zlib.h`/`zconf.h` exist under `/usr/local/include/node`; only `libz.so.1` shipped | Local harness build used a throwaway compiler wrapper adding that include path and a symlinked `libz.so`; no repository file changed for this. CI installs `zlib1g-dev` normally. |
| Python 3 | Yes | Manifest/GLB/custom-input validators run locally |
| Android device / emulator | No | Renderer, UI, lifecycle and thermal layers remain UNVERIFIED here; see §5 |
| Release keystore | No | Signed AAB/APK delivery stays BLOCKED (RELEASE_CHECKLIST §1) |
| CI log/artifact download | No (results-receiver host blocked from sandbox, EOF) | Results are taken from `gh run view` job/step conclusions and check-run annotations; the one failure below was reproduced and root-caused locally instead |

## 2. CI verification

### 2.1 Run `40097344731` (head `c20af03`) — first run with test gates

| Job | Conclusion | Evidence |
|---|---|---|
| Compile native libraries from source (NDK) | **success** | `gh run view` job conclusion |
| Build and verify Android packages (incl. `:sdk:testDebugUnitTest`) | **success** | job conclusion; Gradle fails the build on any unit-test failure, so the 21 tests below executed and passed |
| Host kernel tests and ABI/GLB validation | **failure** at step "Build and run the kernel host harness" | step-level conclusion via jobs API; check annotation `exit code 1` |

The failure was reproduced locally (see §3) and root-caused as pre-existing shader-table
drift, now registered as **AF-016** and fixed in commit `af09371`. The gate earned its place
on its first run.

### 2.2 Run `40098617172` (head `af09371`) — after the AF-016 harness fix

Overall: **completed / success**; all three check-runs success; zero failure-level
annotations (verified via the check-runs API).

| Job | Conclusion | What it proves |
|---|---|---|
| Compile native libraries from source (NDK) | **success** | all native sources compile under the NDK; no prebuilt refresh commit was produced, i.e. built libraries match the committed `prebuilt/` (no drift) |
| Host kernel tests and ABI/GLB validation | **success** | `build_host.sh test` exits 0 (405 checks, 0 failures); ABI manifest drift gate clean; `abi_conformance.py` 47/47; sample GLB and both customization JSONs valid |
| Build and verify Android packages | **success** | SDK + app compile; `:sdk:testDebugUnitTest` passes (21 tests); debug/release APK and release AAR assemble |

### 2.3 JVM unit tests covered by `:sdk:testDebugUnitTest` (21 total)

| Test class | Tests | Defect covered |
|---|---|---|
| `AdultSafetyTest` | 5 (pre-existing) | adult-content guard rails |
| `ImageAnalysisPackingTest` | 2 (pre-existing) | 28-float analysis packing contract |
| `ImageAnalysisNotesTest` | 4 new | AF-003: failure / empty / measured / null-pending wording |
| `RenderSettingsCodecTest` | 6 new | AF-004: round trip, corrupt ints/floats/enums, unknown-key tolerance |
| `DiagnosticsReportTest` | 4 new | diagnostics structure, worst-status roll-up, tier line in plain text |

Status: REAL + VERIFIED in CI — the APK job is success on both runs and the five classes are
identical at `af09371`.

## 3. Local verification actually performed

| Check | Command / method | Result |
|---|---|---|
| Kernel host harness, pre-fix reproduction | `CXX=<wrapper> bash tools/build_host.sh test` | REAL + VERIFIED failure: `405 checks, 1 failure`, `SPIR-V table (13 of 12)` at `host_tests.cpp:1245` → AF-016 |
| Kernel host harness, post-fix | same command after adding the `forward_plus`/`AF_STAGE_COMP` pair | REAL + VERIFIED: `All 405 checks complete: 0 failure(s)`, exit 0 |
| ABI manifest drift gate | `python3 tools/abi_manifest.py` then `git diff --exit-code abi/afcore.abi.json` | REAL + VERIFIED: no diff (47 functions, 11 structs, ABI v1) |
| ABI conformance of the host library | `python3 tools/abi_conformance.py build-host/libafcore.so` | REAL + VERIFIED: `CONFORMANCE PASSED: 47/47 symbols, invariants and reference parity` |
| Sample GLB structural validation | `python3 tools/validate_glb.py artifacts/avatarforge-sample.glb` | REAL + VERIFIED: 271,155 verts / 369,108 tris / 52 morphs / 71 joints / 15 anims / 13 materials |
| Customization contracts | `python3 tools/validate_custom_inputs.py config/customization/*.json` | REAL + VERIFIED: 2 files passed |
| Workflow YAML parses, three jobs present | `yaml.safe_load` | REAL + VERIFIED |
| Kotlin brace/paren balance over `sdk/src`, `app/src` | scripted count | REAL + VERIFIED (depth sanity only; not a compile) |
| Manifest XML well-formed after VIEW filter removal | `xml.dom.minidom.parse` | REAL + VERIFIED |
| CI commands map to existing tools/inputs | inspected `build_host.sh`, validator `main()`s, referenced paths | REAL + VERIFIED |
| Field-existence claims (`qualityLevel`, `Quality.of`, `ImageRole` shape) | grep against `AvatarRenderer.kt`, `AvatarSpec.kt` | source-level REAL + VERIFIED; type correctness confirmed by the CI APK compile |

## 4. What CI green will and will not prove

CI green proves: native sources compile under NDK; the kernel harness passes its full 405-check
suite on a Linux x86_64 host; the ABI manifest matches the header and the host library conforms
(47/47); the sample GLB and customization contracts validate; the SDK compiles against Android
stubs and the 21 JVM tests pass; debug/release APK and release AAR assemble from prebuilt
libraries.

CI green does **not** prove: any frame is rendered on a device; Vulkan/GLES swapchain behaviour;
the Xclipse 940 mip fix (AF-010 retest); settings round-tripping through real SharedPreferences
on Android (the codec is JVM-tested; the wrapper is thin but device-bound); analysis-note
rendering in Compose; export/share flows; thermal/ADPF behaviour; signed-artifact integrity.
Each stays UNVERIFIED with a procedure in §5 or BLOCKED with an owner action.

## 5. UNVERIFIED device backlog (authoritative copy in TEST_MATRIX §3)

The ten-step device procedure remains the only path to REAL + VERIFIED for runtime claims.
Highest-value items for this change set:

- step 3: force an analysis failure and confirm the new note while generation continues (AF-003);
- step 5: process death with the viewer open, relaunch, confirm render settings restored (AF-004);
- step 4/7: capture a `diag` report containing the new "Quality tier changes" row (AF-008);
- step 10: file manager no longer offers AvatarForge for `.glb` (AF-002).

## 6. Known unverified/blocked dispositions (unchanged by this work)

| Item | Status | Unblock condition |
|---|---|---|
| AF-005 dead `engine/` module, AF-006 Rust workspace, AF-013 no version catalog | deferred tech debt | maintainer decisions, REMEDIATION_PLAN item 10 |
| AF-007 three phantom workflow registrations | BLOCKED | repo admin deletes server-side registrations |
| AF-009 signed AAB/APK | BLOCKED | release keystore + secrets, see RELEASE_CHECKLIST |
| AF-012 instrumented tests | MISSING | emulator runner in CI, plan item 7 |
| AF-014 white head cap | hypothesis | device re-screenshot on a build containing the mip fix |
| AF-015 GLB import | feature-tracked; VIEW filter intentionally absent | plan item 4 ships the handler first |
| Prompt/photo persistence (AF-004 remainder) | deferred | plan item 9, scoped storage with auto-purge |
| ADPF thermal integration (AF-008 remainder) | queued | plan item 3 |
