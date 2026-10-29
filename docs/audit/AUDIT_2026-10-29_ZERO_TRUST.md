# Zero-trust audit and repair — 2026-10-29

Branch: `arena/aaf85469-avatarforge` (working branch; `main` stays at `40ef65b` and is untouched).
Pass type: repository-wide audit → coordinated repair → automated validation. No release was
published, no branch was merged or rewritten, and no credential was requested.

Labels used throughout: **REAL + VERIFIED** (reproducible evidence in this repository/environment),
**PARTIAL** (implemented, missing a required proof), **UNVERIFIED** (no execution proof),
**MISSING** (absent), **BLOCKED** (external credential/device/tool required).

## 1. What this environment can and cannot prove

| Capability | State | Consequence for this report |
|---|---|---|
| C++17 host build + kernel/renderer harnesses | Present (GCC, host zlib 1.3.1 PIC) | Kernel, model and renderer-sequencing behaviour is executed, not asserted |
| Python tooling (`embed_shaders.py`, `verify_gles_artifacts.py`, GLB validators) | Present | Shader-table and packaging checks run locally |
| JDK / Gradle / Android SDK / NDK | **Absent** | Kotlin/Android/GLSL/native-Android compilation is proven only by CI |
| `glslc` | **Absent** | `tools/embed_shaders.py --check` exits non-zero locally by design; CI runs the real check |
| Rust toolchain | **Absent** | `rust/*` remains UNVERIFIED by compilation |
| Android device / GPU | **Absent** | Every pixel, thermal and frame-pacing claim stays UNVERIFIED/PARTIAL |
| Release keystore | **Absent** | Signed production AAB/APK is BLOCKED |

Nothing below converts compilation success into a rendering claim. The device still has to show the
pixels.

## 2. Coverage map

| Area | What was inspected | Result |
|---|---|---|
| Build system / Gradle / AGP / NDK / CI | `settings.gradle.kts`, root + module build files, `gradle.properties`, wrapper, `.github/workflows/android-apk.yml` | REAL + VERIFIED by inspection; AGP 8.5.2 / Kotlin 1.9.24 / compileSdk 34 / minSdk 26 / NDK 26.1.10909125 pinned. CI jobs unchanged in scope: shader check, model/kernel/renderer host tests, arm64 native build, GLES artifact guard, SDK JVM tests, debug APK + explicitly UNSIGNED release AAB + SDK AAR + provenance |
| App + UI actions | `MainActivity.kt`, `InputScreen`, `ProgressScreen`, `ViewerScreen`, `DiagnosticsDialog`, `AppBitmaps` | Two advertised-but-hollow paths repaired (**AF-021**, **AF-022**, **AF-031**); lifecycle/composition defects repaired (**AF-024**) |
| Architecture & integration | UI → state → SDK → JNI → kernel; Kotlin → C ABI; Rust mirror | Cross-thread ownership hole closed (**AF-020**); Rust metadata drift fixed (**AF-029**), Rust still unbuilt |
| Security & data integrity | Manifest permissions, FileProvider paths, JNI bounds checks, image buffer contract, settings input, secrets | JNI bounds/`GetDirectBufferCapacity` checks verified present; `ANativeWindow_release` verified; image contract hardened (**AF-028**); no INTERNET permission; no secrets in tree. No SQL/database layer exists to audit |
| Native rendering & performance | `af_gl.cpp` (context, targets, UBO alignment, MSAA fallback, HDR fallback, swap-failure path, teardown), `af_jni.cpp`, renderer host harness | Root-cause fix for unbounded target allocation (**AF-025**); frame/submission/present accounting verified by host tests; GPU time still honestly `-1` |
| Tests & quality gates | `tools/build_host.sh test`, `tools/test_model_host.sh`, `tools/test_renderer_host.sh`, `sdk/src/test`, CI workflow | No test deleted or weakened. Renderer suite grew 57 → **64 checks**; codec suite grew by two cases |
| Release & distribution | `app/build.gradle.kts` signing block, versioning, R8 rules, ABI packaging, `tools/verify_gles_artifacts.py`, docs | Fail-closed signing verified by inspection and CI history; R8/minify still disabled (deliberate); doc contradiction fixed (**AF-023**); consumer rules corrected (**AF-027**); signed delivery BLOCKED |
| Documentation truth | `PROJECT_STATUS.md`, `STATUS.md`, `BUILD.md`, `DEVICE.md`, `docs/audit/*`, `docs/ENGINE_CONSOLIDATION_AUDIT.md`, `docs/RENDERER_FAULT_2026-10-29.md` | Vulkan removed from current-facing text; historical documents banner-marked (**AF-030**) |

## 3. Repairs applied in this pass

1. **One kernel fence (AF-020).** New `sdk/src/main/java/com/avatarforge/sdk/internal/KernelAccess.kt`
   provides a single reentrant lock plus a bounded `tryWithKernel` variant. It is taken by:
   `AvatarEngine.applyPose / sampleClip / applyClip / applyBlendshapes / counts / clipDuration`,
   the generation body (`engineReset`, `engineRunAll`, `engineExportGlb`), `parsePrompt`,
   `analyseImages`, `exportGlb`, `exportGlbBytes`, `requery`, `engineDestroy`, and by
   `AvatarRenderer.frame / attachEngine / invalidateMesh / pickRegion`.
   Expression weights are now applied on the render thread through
   `RenderSurfaceView.applyBlendshapes(...)`, so a morph write can no longer interleave with the
   zero-copy vertex upload. Picking uses a 250 ms bounded wait and reports "nothing picked" rather
   than intersecting a mesh that is being rewritten.
2. **Honest face panel (AF-021).** Channel weights are host-owned state; the whole array is applied
   per edit; chips display the applied value and the header shows how many channels are active.
3. **Reachable reference-image measurements (AF-022, AF-033).** Analysis moved to image selection,
   runs on `Dispatchers.IO`, fills the card above the spec editor, updates the spec for correction,
   and is reused for the build. Failure is stated on the card instead of being swallowed.
4. **Renderer input sanitisation (AF-025).** `sanitizeOptions()` clamps/validates every numeric
   option before it can size a GPU allocation; `RenderSettingsCodec.decode` mirrors the bounds so
   the settings UI never displays a value the renderer silently reduced.
5. **Compose/lifecycle correctness (AF-024).** No state writes during composition; stable fallback
   flows; `collectAsStateWithLifecycle()` for the viewer's stats/info/error streams.
6. **Dependency and packaging hygiene (AF-026, AF-027).** Dead `exifinterface` dependency removed
   from the published SDK; unused lifecycle artifact now actually used; invalid consumer rule
   replaced with the correct one.
7. **SDK input contract (AF-028).** Overflow-checked buffer sizing; ARGB_8888 requirement.
8. **Metadata and documentation truth (AF-023, AF-029, AF-030).** Signing docs, Rust versions/URL,
   current-facing Vulkan text, historical banners, benchmark item removed.
9. **Export panel honesty (AF-031).** Real existence/size check; live joint/clip counts.

## 4. Validation actually executed here

Commands (from the repository root, host zlib pre-installed):

```
export CPLUS_INCLUDE_PATH=/home/user/fault-review/deps/zlib-install/include
export LIBRARY_PATH=/home/user/fault-review/deps/zlib-install/lib
bash tools/test_model_host.sh          # focused model-detail regressions
bash tools/test_renderer_host.sh       # real af_gl.cpp against the GLES/EGL test double
bash tools/build_host.sh test          # full kernel host harness
python3 tools/embed_shaders.py --check # requires glslc; expected to fail in this container
git diff --check
```

| Check | Result |
|---|---|
| Model-detail regressions | **33 checks, 0 failures** |
| Renderer host fault injection (after AF-025) | **64 checks, 0 failures** (was 57) |
| Full kernel host harness | see §7 — recorded after the final run |
| `embed_shaders.py --check` | Expected local failure: `glslc` is not installed. CI performs the real compile + table diff |
| `git diff --check` | clean (no whitespace errors) |
| CI on the exact head | recorded in §7 after the push |

The JVM/SDK unit tests, the arm64 native build, the debug APK, the unsigned AAB, the AAR and the GLES
artifact guard are executed by CI only; they cannot be reproduced in this container.

## 5. Findings register

The full register is `docs/audit/FINDINGS.md`; this pass added **AF-020 … AF-033**. Worst open
items:

- **AF-020 (Critical)** is fixed in source and compiled by CI, but a data race cannot be proven absent
  by a JVM test. It needs a device soak with the face panel and clip playback running together.
- **AF-025 (High)** is fixed and host-verified. Device proof still requires a deliberate bad-settings
  run to confirm the clamp (it should now be impossible to lose the context that way).
- **AF-016 (High)** GPU timestamps / soak / diagnostic ladder remain **MISSING**.
- **AF-012 (High)** no LICENSE file — unchanged, and out of scope for an autonomous pass.
- **AF-032 (Medium)** customization schema is dead weight; integration or deletion is a decision, not
  a silent edit.

## 6. Release and distribution posture (unchanged, re-verified)

- `applicationId com.avatarforge`, `versionCode 2`, `versionName 0.10.1`, compile/target SDK 34,
  minSdk 26, ABIs `arm64-v8a,x86_64` (override with `-Paf.abIs`).
- Release signing **fails closed** without `keystore.properties` or `AF_RELEASE_*`. CI produces an
  explicitly **UNSIGNED** AAB under `-Paf.allowUnsignedRelease=true`; it is compile proof, not an
  installable production artifact.
- R8/minify/shrinkResources remain **disabled**. Consumer rules were corrected anyway so that
  enabling R8 later does not silently break the JNI callback lookup.
- `extractNativeLibs=true`; the manifest declares no INTERNET/network/storage/camera permission, so
  "works offline" is enforced by the platform.
- `tools/verify_gles_artifacts.py` still rejects any packaged library that carries a Vulkan
  `DT_NEEDED`.
- **Signed AAB/APK: BLOCKED** — no keystore exists in this environment.

## 7. Exact-head record

Filled in after the final commit and CI run; see `PROJECT_STATUS.md` for the same values.

- Branch: `arena/aaf85469-avatarforge` (pushed; `main` untouched at `40ef65b`)
- Code head: **`d7822b751fd79f1defc50264d6a70edb16d08410`** (this document's commit follows it and
  changes documentation only)
- Full kernel host harness: **406 checks, 0 failures** (unchanged count; no kernel behaviour was
  modified in this pass)
- Model-detail regressions: **33 checks, 0 failures**
- Renderer host fault injection: **64 checks, 0 failures** (was 57; the seven new checks are the
  option-sanitisation regressions for AF-025)
- `tools/embed_shaders.py --check`: exit 1 with `--check requires glslc; no compiler validation is
  claimed` — the expected local result, not a pass
- `git diff --check`: clean
- CI: run [40900908327](https://github.com/davealone69-gif/AvatarForge/actions/runs/40900908327)
  **SUCCESS** on `d7822b7` in 12m00s; artifact `12910680693` (17,657,509 bytes) contains the debug
  APK, the explicitly UNSIGNED release AAB, SDK AAR, `SHA256SUMS` and `PROVENANCE.json`. The
  `signed-release` job was skipped because no credentials exist.

## 8. Remaining risks and blockers

1. **Device pixels**: every visual fix from the previous checkpoint (eyes, hair, chest plate, palms,
   garments, header) and every lifecycle fix in this pass is still unconfirmed on the SM-S921B.
   Compilation ≠ verified rendering.
2. **Thermal**: the last device recovery report showed `THERMAL_STATUS_SEVERE` with the requested
   MEDIUM preset reduced to LOW at ~29 FPS. That is a device/thermal observation, not a model or
   renderer-correctness claim, and it must be re-measured on the next build.
3. **Concurrency**: the fence closes a real hole, but lock-ordering under a device soak (camera drag +
   clip playback + expression drag + tap picking) is UNVERIFIED.
4. **Rust mirror**: metadata now matches; the crates are still uncompiled here.
5. **Signed delivery**: BLOCKED without credentials.
6. **Performance claims**: no GPU timestamps, no sustained soak. No FPS or "production ready" claim is
   made anywhere in this document.

## 9. Next required action (human)

1. Install the exact-head CI debug APK on the SM-S921B and capture: front/side eyes, hairline,
   chest/shoulder, hands, lower garment, the face panel with two channels active at once, and a
   diagnostics export (which now includes the recovery journal and thermal state).
2. Run the same build for ≥10 minutes with clip playback and camera orbiting to exercise the new
   fence; export the recovery report afterwards.
3. To unblock release: supply `AF_RELEASE_STORE_FILE`, `AF_RELEASE_STORE_PASSWORD`,
   `AF_RELEASE_KEY_ALIAS`, `AF_RELEASE_KEY_PASSWORD` as repository secrets so the tag-only
   `signed-release` job can produce a signed AAB.
