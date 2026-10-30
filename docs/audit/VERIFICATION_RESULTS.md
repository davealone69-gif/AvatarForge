# Verification results

Checkpoint: 2026-10-29, branch `arena/aaf85469-avatarforge`.

## Zero-trust pass (code head `d7822b7`)

### Executed locally

- `bash tools/test_renderer_host.sh`: **64 checks, 0 failures** (57 before this pass). The seven new
  checks cover option sanitisation (AF-025): a 65536² shadow map and 64× MSAA request are clamped,
  NaN exposure is replaced, and a later `setOptions` with a 100000² map still renders a frame.
- `bash tools/test_model_host.sh`: **33 checks, 0 failures** (unchanged; no model code was touched).
- `bash tools/build_host.sh test`: **406 checks, 0 failures** (unchanged).
- `git diff --check`: clean.
- `python3 tools/embed_shaders.py --check`: exit 1 with `--check requires glslc; no compiler
  validation is claimed`. Expected in this container; CI performed the real shader gate and passed.

### CI

**REAL + VERIFIED:** run https://github.com/davealone69-gif/AvatarForge/actions/runs/40900908327
succeeded on head `d7822b751fd79f1defc50264d6a70edb16d08410` (12m00s), including the shader
compile/diff gate, model + kernel + renderer host tests, ARM64 native build, SDK JVM tests (new codec
clamping cases), debug APK, explicitly UNSIGNED release AAB, SDK AAR, GLES artifact guard and upload.
Artifact: `AvatarForge-Android-d7822b7…`, id `12910680693`, 17,657,509 bytes. The signed-release job
was skipped (no credentials).

### Not proven by this pass

- Kotlin concurrency correctness (AF-020) cannot be proven by a JVM test; device soak required.
- Device pixels, thermals and frame pacing remain UNVERIFIED.
- Rust crates remain uncompiled here (metadata only updated).

## Executed locally

- `bash tools/test_renderer_host.sh`: **57 checks, 0 failures**. GLES/EGL are stateful host doubles; this proves API sequencing, error propagation, cleanup and C ABI telemetry layout, not pixels.
- `bash tools/test_model_host.sh`: **33 checks, 0 failures**. Covers the world-axis palm and garment-layer/poke-through regressions plus eye-label side mapping, exposed-sclera symmetry, extracted eye-patch symmetry, the chest-plate shell and hair clearance.
- Host GLB export of the current device spec (`athletic woman, ponytail, full plate armour`, 1.75 m, outfit 5, ponytail): **302,545 verts / 412,266 tris**, `validate_glb.py` OK. Eye patches are **2,359 / 3,148** triangles; gear front is **−0.139 m** instead of the pre-fix −0.222 m; hair top is **crown + 0.024 m** instead of +0.077 m. The afro style still clears the skull by **0.085 m**.
- `bash tools/build_host.sh test`: **406 checks, 0 failures**. The old SPIR-V table-count failure is obsolete because the Vulkan/SPIR-V path was removed; the GLES shader table now has exactly the required 12 stages.
- `git diff --check`: clean.

## Not executed locally

- Gradle/JVM tests: no JDK/Gradle/Android SDK in this environment.
- Android native NDK build: no NDK/CMake/Ninja/Clang.
- Rust: no rustc/cargo.
- Device/adb: no device.
- Shader compilation: no glslc; CI performs the compiler gate.

## CI

**REAL + VERIFIED:** run https://github.com/davealone69-gif/AvatarForge/actions/runs/40895347516 succeeded on head `39c8cf68663e6b22419d392780095c74fb08bd3f` in 7m52s.

- GLES shader compile/diff gate passed; there is no SPIR-V warning because the SPIR-V table was deleted.
- Model-detail regressions, full kernel host harness, renderer host tests, ARM64 native build, SDK JVM tests, debug APK, unsigned release AAB, SDK AAR, GLES-only linkage verification, artifact verification and upload passed.
- Artifact: `AvatarForge-Android-39c8cf68663e6b22419d392780095c74fb08bd3f`, id `12905983840`, 17,610,882 bytes. It contains a debug APK, an explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS and PROVENANCE.json.
- Earlier runs 40867172914 (`1fcbe6c`) and 40864270280 (`973f70f`) are superseded by this exact-head eye/hair/chest-fixed run.

## Device evidence

- Camera: **user-confirmed working**. Exact build SHA/counters were not supplied, so this is functional confirmation, not a repeatable regression artifact.
- Earlier device reports (build `6883243`, SM-S921B API37 arm64, thermal severe): gles31 ANGLE/Xclipse 940, 90.7 MiB textures, 27.8–49.2 FPS, 300k–417k submitted triangles. These are historical GLES reports, not proof of the new GLES-only/model-fixed build.
- Current evidence supplied 2026-10-29 21:25 (SM-S921B, GLES 3.1, 416,396 tris): screenshot still shows the pre-fix metal-ball cuirass and hair over the brow. Its GLB measures **1,637 left-eye vs 156 right-eye triangles**, gear front **−0.222 m**, hair top **crown + 0.077 m**. Recovery report shows severe thermal, requested quality MEDIUM but effective LOW, 29.2 FPS, no renderer fault. This is the defect evidence, not proof of the fixes.
- New eye/hair/chest/hand/garment/header fixes: **UNVERIFIED on device** until the next CI APK is installed and captured.

Production signed release remains **BLOCKED**: the signed-release job requires production secrets on a tag.

## Exact fix-branch verification — 2026-10-30

The local source branch is `fix/verify-clean-checkout-output-dir`. This record is separate from the historical CI/device checkpoints above; it does not claim a physical-device or shipped-release pass.

### Post-fix host verification

The complete post-fix verifier was run with the Android NDK 26.1.10909125 shader tools explicitly set:

```bash
export ANDROID_HOME=/home/ubuntu/Android/Sdk
export ANDROID_SDK_ROOT=/home/ubuntu/Android/Sdk
export ANDROID_NDK_HOME=/home/ubuntu/Android/Sdk/ndk/26.1.10909125
export AF_GLSLC=/home/ubuntu/Android/Sdk/ndk/26.1.10909125/shader-tools/linux-x86_64/glslc
export AF_SPIRV_VAL=/home/ubuntu/Android/Sdk/ndk/26.1.10909125/shader-tools/linux-x86_64/spirv-val
bash tools/verify.sh
```

The command exited **0** and ended with “All host-side checks passed. Device pass: see DEVICE.md.” Results: the kernel harness passed **423/423** checks; renderer fault-injection passed **87/87** and the Vulkan API-version helper **7/7**; model-detail regressions passed **33/33**; the release-workflow contract passed, including `fix/**` triggers and separation of Vulkan API, shader and header provenance. The device-report check used mock ADB only and explicitly reported no physical device.

All **12 GLES stages** compiled and the embedded table matched. All **13 Vulkan shader sources** compiled for `vulkan1.3`, and each embedded module passed `spirv-val`; the six shader provenance regressions passed. The validator records the pinned source/table history hash `76fdebab`; newer NDK `glslc` output can differ in optimized bytes without changing the pinned-source/module validation contract. The produced GLB passed structural validation: 280,138 vertices, 394,016 triangles, 71 joints, 52 blendshapes and 15 clips. The ABI manifest was fresh at 47 symbols, 11 structs and 12 enums; C++ parity passed 47/47, Rust currently implements 15/47 (32 remain unported), and prompt parity passed 72/72. The default Rust workspace passed 37 tests; two full-avatar integration tests remain intentionally ignored unless explicitly requested.

The verifier’s final repository-artifact scan **skipped** `artifacts/avatarforge-debug-arm64.apk`, `artifacts/avatarforge-release-arm64.apk` and `artifacts/avatarforge-sdk-arm64.aar` because none was staged in that directory. This is not a failure of the separate compile-only Gradle build, and it is not evidence of a signed release.

### Rust renderer safety gate and Android build

After the optional `renderer` feature exposed a duplicate `Camera::default` implementation and a missing crate-private engine-handle accessor, those defects were fixed. `cargo test --offline -p af --features renderer --lib` passed **7/7**. The documentation test passed **1/1** with `compile_fail,E0505`; the compiler specifically rejects moving/dropping the engine while the attached renderer is used. The API now consumes an unbound renderer and returns a renderer whose type retains the engine borrow. The host test type-checks this lifetime contract; it does not link or execute `libafrender.so`.

A final default Rust release build and `cargo test --workspace --offline` passed: 37 passed, 0 failed, with the same two ignored full-avatar tests. The earlier task-local Android validation passed 3 app JVM tests and 38 SDK JVM tests, the ARM64 native build and compile-only APK/AAB/AAR generation. None of these outputs has been installed or exercised on an SM-S921B.

`git diff --check` passed. `cargo fmt --all -- --check` did **not** pass: it reports broad rustfmt diffs across the Rust workspace (168 diff hunks in that check). No mass formatting rewrite was applied because it would reformat unrelated code; this remains a style-cleanup item, not a passing gate.

### What the evidence does not establish

The target label **Vulkan 1.3.304** is not treated as an API, header, loader or driver version. The app requests the standard Vulkan 1.3 API level; the installed NDK headers are revision 1.3.237. The shader target, compiled Vulkan headers, runtime loader, device-reported API, extensions/features, queue support, formats and limits are distinct evidence fields. The host tests and shader validator do not query the SM-S921B or execute Vulkan commands, present pixels, verify visual quality, test GLES recovery on device, or demonstrate thermal/stress behaviour. Those device gates remain open in `DEVICE.md`.

The remote `arena/c87f5c50-avatarforge` tip was `f82287b6e13c1dc131c5d1b5d4566b30d9e74d62`, two commits past the local branch’s common base. No rebase, merge, force-push or direct push to the arena branch was performed. The normal push created `origin/fix/verify-clean-checkout-output-dir` at `401a005eac045371c0d3625baf4c523a55288435`; the arena ref remained unchanged. Exact-head Android APK workflow [41002485082](https://github.com/davealone69-gif/AvatarForge/actions/runs/41002485082) succeeded on that commit, including artifact staging, verification and upload. Uploaded artifact `AvatarForge-Android-401a005eac045371c0d3625baf4c523a55288435` has ID `12954506148`, size 18,048,697 bytes, and is not expired. The runner emitted non-blocking Node.js 20 / Node.js 24 and `actions/setup-java@v4` deprecation warnings. Production signing remains blocked without release credentials.

## Follow-up validation on the branch — 2026-10-30

Repository `/workspace/AvatarForge`, branch `fix/verify-clean-checkout-output-dir`, local/remote
branch tip at validation start `f6929e98d23a7e45c31d7f6bac35f7560e4fcf9a`. The tests below ran on a
working tree based on that tip; the successful public CI run above is on earlier commit `401a005` and
does not verify this follow-up. This section records local test/build evidence only.

### Tests and builds

- `bash tools/build_host.sh test` initially failed **1/438** checks: with a 1.00-texel packed
  interior, **19/14,412** 2048 chart cells still had a sub-texel dimension. The floor was changed to
  **1.01 texels** to preserve a full texel after float UV bounds are stored. The final rerun passed
  **438/438**, reporting **0/14,412** sub-texel cells and 0.82 packed-cell coverage. This fixes the
  numeric host gate; visual/device bake confirmation is not claimed.
- `bash tools/test_model_host.sh` first failed **1/39** because shoulder/hip measured **1.048**, just
  below the 1.05 lower target. After the bounded hip-profile adjustment it passed **39/39** at
  **1.052**; the final run, with an additional finite shoulder/waist upper safety assertion, passed
  **40/40**. Final no-garment ratios: shoulder/waist **1.346**, waist/shoulder **0.743**, shoulder/hip
  **1.052**. Exact 68 cm waist and 96 cm hip tape inputs remain within 0.01 cm.
- `bash tools/test_renderer_host.sh`: **102 checks, 0 failures**. The output also says
  `SKIP Vulkan version helper unit test (pinned NDK Vulkan headers not found)`. Installed NDK headers
  are revision 1.3.237; no test result is claimed for the missing pinned-header helper.
- `ANDROID_HOME=/home/ubuntu/Android/Sdk ANDROID_SDK_ROOT=/home/ubuntu/Android/Sdk bash gradlew
  --no-daemon --console=plain --rerun-tasks :app:testDebugUnitTest :sdk:testDebugUnitTest`:
  **3 app tests and 38 SDK tests passed** on the forced execution.
- `python3 tools/test_release_workflow.py`: both source-contract checks passed (signed artifact
  staging/verification and `fix/**` trigger/version-provenance separation).
- `ANDROID_HOME=/home/ubuntu/Android/Sdk ANDROID_SDK_ROOT=/home/ubuntu/Android/Sdk bash gradlew
  --no-daemon --console=plain :app:assembleDebug`: **BUILD SUCCESSFUL**. CMake compiled
  `af_vk.cpp`, `af_gl.cpp`, and JNI/native libraries for arm64-v8a and x86_64. The generated debug
  APK is `/workspace/AvatarForge/app/build/outputs/apk/debug/app-debug.apk`, **11,771,041 bytes**,
  SHA-256 `ea00a8dbe4f7d050d582e3febfe412a86909b9f6aaee55cec0b112765efdc931`. `apksigner verify`
  succeeded using APK Signature Scheme v2; the package contains both native ABIs and does not request
  `android.permission.INTERNET`. This is debug build/package verification, not production signing.
- `adb devices -l`: no attached device. The APK was not installed or launched; AvatarForge runtime,
  Vulkan/GLES GPU timing, visible pixels, fallback, thermal behavior and device-side stale-handle
  stress remain **UNVERIFIED**.

### Corrected intermediate failures and build diagnostics

- `./gradlew --no-daemon :app:assembleDebug` failed with `Permission denied`; the wrapper is not
  executable in this checkout. `bash gradlew --no-daemon :app:assembleDebug` then failed because no
  Android SDK location was configured. Supplying `ANDROID_HOME`/`ANDROID_SDK_ROOT` reached the NDK,
  where the first compile failed at `native/src/android/af_jni.cpp:80`: the Android NDK expects
  `JNIEnv**` for `AttachCurrentThread`, but the code passed `void**`. The Android-specific call was
  corrected while preserving the host-JDK signature; the final two-ABI NDK build above passed.
- Final Android build emitted non-fatal SDK XML v4/tool-version, `extractNativeLibs`, and existing
  unused-parameter/variable warnings. They did not prevent build completion.
- Earlier public CI run [41002485082](https://github.com/davealone69-gif/AvatarForge/actions/runs/41002485082)
  was independently confirmed successful on `401a005`; this local verification record does not claim
  CI validation for the follow-up changes.
