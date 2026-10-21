# Building AvatarForge

Everything here builds on a Linux host. There is no cloud step, no account, no service to run, and
no generated code that is not checked in: a clone plus a toolchain is enough to produce the APK.

The project has four layers, and each can be built and tested on its own:

| Layer | Sources | What it is | How it is built |
|---|---|---|---|
| Kernel | `native/src/*.cpp`, `native/include/afcore.h` | anatomy → SDF field → isosurface → UV → PBR bake → rig → blendshapes → clips → GLB, behind a stable C ABI | `tools/build_host.sh` (host) / CMake+NDK (Android) |
| Renderer | `native/src/render/` | Vulkan primary, OpenGL ES 3.1 fallback, written from scratch | CMake+NDK (Android only: EGL/ANativeWindow); shaders baked on any host |
| Rust | `rust/af-sys`, `rust/af` | safe Rust wrapper over the *same* C ABI as the Kotlin path | `cargo test --offline` against `build-host/libafcore.so` |
| App + SDK | `sdk/`, `app/` | Kotlin SDK façade, Compose UI, one Activity | Gradle + AGP 8.5 |

---

## 0. Toolchain

```bash
tools/bootstrap_toolchain.sh          # installs into /usr/local/tc (~1.2 GB, pinned versions)
```

It installs JDK 17, Gradle 8.9, Android cmdline-tools + platform 34 + build-tools 34 +
platform-tools + CMake 3.22.1 + **NDK 26.1.10909125**, the standalone Kotlin 1.9.24 compiler and
rustup/cargo. Afterwards:

```bash
export JAVA_HOME=/usr/local/tc/jdk17
export ANDROID_HOME=/usr/local/tc/android-sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/26.1.10909125
export CARGO_HOME=/usr/local/tc/cargo-home
export RUSTUP_HOME=/usr/local/tc/rustup
export PATH=$JAVA_HOME/bin:/usr/local/tc/gradle-8.9/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$CARGO_HOME/bin:$PATH
```

Which packages are needed and why is documented at the top of the script; the short version is that
AGP 8.x needs JDK 17, the renderer links `vulkan`/`EGL`/`GLESv3`/`android`/`log` from the NDK
sysroot, and the kernel needs only zlib (provided by the Android platform, `-lz` on a host).

Hardware: this project is developed in a 2 vCPU / 2 GB container. `gradle.properties` disables the
daemon and parallel module builds; `tools/build_gradle.sh` uses one worker, a 650 MiB heap, 400 MiB
metaspace and SerialGC by default. Override with `AF_JVM_ARGS` and `AF_GRADLE_MAX_WORKERS` on a
larger host.

---

## 1. Kernel + host test harness (no device, no Android toolchain)

```bash
tools/build_host.sh test     # builds build-host/libafcore.so and runs the 464-check harness
tools/build_host.sh kernel   # library only
tools/build_host.sh all      # library + harness binary, do not run
```

Expected output ends with:

```
All 464 checks complete: 0 failure(s)
```

The harness covers anthropometry (waist/hip/crotch exactness against tape measurements), signed
distance field sanity, isosurface integrity (closed shell, no boundary loops, plausible volume for
four body variants), determinism (identical spec ⇒ identical mesh), the rig (bind-pose exactness,
fingertip/toe skinning, all 52 ARKit channels carrying real displacement), UV/PBR bake statistics,
apparel, all 13 embedded shader stages (including Forward+ compute), the host-tested renderer culler
(`[10b]`), per-device GPU profile selection, ASTC/ETC2 block encoding, semantic mip chains and async
compression polling (`[10c]`), index-only LOD range construction (`[10e]`), atlas budget selection and
a full staged end-to-end pass through the public C ABI, scene/LOD/profiler checks, and pose composition (`[12]`:
`AF_CLIP_REST` restores the bind pose *exactly*, and playing a clip, returning to
rest and playing it again is bit-identical to playing it once - the skinning path is stateless with
respect to whatever was displayed last).

### Optional diagnostics

```bash
g++ -std=c++17 -O2 -o /tmp/shape_probe tools/shape_probe.cpp -Inative/include -Inative/src \
    -Lbuild-host -lafcore -Wl,-rpath,$PWD/build-host
/tmp/shape_probe            # per-blendshape vertex counts and amplitudes (~35 s)
```

---

## 2. Shaders

Checked in, so a normal build needs nothing extra:

* `native/src/render/af_shaders_vk.h` — 18 kB of SPIR-V words (Vulkan)
* `native/src/render/af_shaders_gl.h` — GLSL ES 3.10 source text (GLES 3.1)

Regenerate after editing anything in `native/src/render/shaders/`:

```bash
python3 tools/embed_shaders.py           # needs glslc (ships in the NDK's shader-tools)
python3 tools/embed_shaders.py --check   # compile + validate, write nothing
```

The script compiles every stage twice from one source — SPIR-V for Vulkan, `#version 310 es` for
GLES — validates the SPIR-V with `spirv-val`, type-checks the GLES dialect with `glslc --target-env=opengl`,
and fails loudly if a stage is missing or the two dialects disagree. The kernel harness ([10]) then
verifies the baked tables at runtime cost zero: every pipeline stage present, valid SPIR-V magic,
GLES text well-formed, unknown names returning null (no silent unlit fallback).

### Renderer texture codecs and profiles

The renderer vendors Apache-2.0 ASTC Encoder 5.4.0 and Google Etc2Comp under `native/third_party/`.
CMake selects ASTC NEON for `arm64-v8a`, SSE2 for `x86_64`, and the portable scalar implementation
for other ABIs; Etc2Comp supplies the ETC2/EAC fallback. The host harness exercises ASTC/ETC2 encoding,
semantic mip generation and asynchronous job polling. The GL/Vulkan source uploads these formats only
after checking the device's advertised format support, and retains a raw RGBA8/R8 upload fallback.
The arm64 Android NDK build has now passed for the current source. Host tests and successful
compilation do not exercise actual GPU uploads; a physical-device render check is still required.

---

## 3. Native libraries for Android

```bash
tools/build_android.sh             # arm64-v8a, android-26 -> build-android/ + prebuilt/arm64-v8a/
tools/build_android.sh x86_64      # emulator ABI -> build-android-x86_64/ + prebuilt/x86_64/
tools/build_android.sh arm64-v8a android-29

# Windows (PowerShell 5+, no Android Studio needed):
powershell -ExecutionPolicy Bypass -File tools\build-android.ps1
```

`native/CMakeLists.txt` builds three targets: the kernel (`afcore`), the renderer (`afrender`,
links vulkan/EGL/GLESv3/android/log), and the JNI bridge (`avatarforge`). The Gradle build runs
exactly this through `externalNativeBuild`; the manual invocation is there so the native side can be
iterated without Gradle in the loop, and so the `.so` files can be built once and packaged many times
(`-Paf.prebuiltJni=/path/to/prebuilt`, which is also the way to build the APK on a machine that has
no NDK - see the RAM note in §4).

`tools/build_android.sh arm64-v8a android-26` was run on 2026-10-20 UTC with NDK 26.1.10909125 and
produced current `prebuilt/arm64-v8a/{libafcore,libafrender,libavatarforge}.so` (13,356,600 B /
5,480,152 B / 636,568 B, unstripped and static-STL). Gradle's release APK/AAR contain stripped
libraries (1,753,544 B / 1,010,000 B / 79,920 B). The x86_64 target is supported in project CMake
configuration but was not rebuilt for this arm64 build targeting the requested Galaxy S24 SM-S921B. `tools/build-android.ps1`
is the Windows port of the same script and has **not** been executed - there is no Windows host in this
environment - so use `-DryRun` to print the exact cmake/ninja command lines it would run.

---

## 4. APK

```bash
# Preferred: the wrapper pinned in gradle/wrapper (Gradle 8.9, downloads its distribution once)
./gradlew :app:assembleDebug                 # -> app/build/outputs/apk/debug/app-debug.apk
./gradlew :app:assembleRelease               # signed: keystore.properties, else the debug keystore
./gradlew :sdk:assembleRelease               # the SDK as an AAR for third-party consumers
./gradlew :sdk:testDebugUnitTest              # Kotlin/JVM SDK regressions

# This container / any machine that already has Gradle: the pinning wrapper
tools/build_gradle.sh :app:assembleDebug -Paf.abIs=arm64-v8a -Paf.prebuiltJni=$PWD/prebuilt
```

`local.properties` (git-ignored) or `ANDROID_HOME` must point at the SDK; `ndkVersion` in both
modules must match the installed NDK (26.1.10909125). Project configuration supports `arm64-v8a`
(primary) and `x86_64` (emulator); the current verified artifacts are arm64-v8a only.

Build properties, all optional:

| Property | Effect |
|---|---|
| `-Paf.abIs=arm64-v8a` | build one ABI instead of two (halves the native build time and its peak RAM) |
| `-Paf.prebuiltJni=$PWD/prebuilt` | package the `.so` files from `prebuilt/` and skip CMake entirely - the way to build the APK on a small machine |
| `-Paf.lowRam=true` | `-O1` for the natives plus a single-threaded CMake build |

Release signing: put a `keystore.properties` (storeFile / storePassword / keyAlias / keyPassword) at
the repository root to sign with your own key, otherwise the release build is signed with the debug
keystore so it can still be installed on a test device. R8 is deliberately off; see the comment in
`app/build.gradle.kts`.

### Artifacts built in this repository

These files were rebuilt on 2026-10-21 UTC. The Kotlin/UI thermal-report follow-up was compiled in
this build; the ARM64 native libraries are the current NDK r26b outputs from the preceding native
build.

| Artifact | Size | SHA-256 | Contents |
|---|---:|---|---|
| `artifacts/avatarforge-debug-arm64.apk` | 10,163,456 B | `fc4e57dd483dd6b5b8e4ed280dcc90a5eb88fa98882029d5bd08cd0c183603f0` | Debug app, arm64 natives, both offline customization contracts and live device report |
| `artifacts/avatarforge-release-arm64.apk` | 7,190,802 B | `981ca221e4b62b61a3730f8b5d2c9df9dc0c3e3e26897086afd4156700d2ce58` | Release app and arm64 natives, signed with the local debug certificate |
| `artifacts/avatarforge-sdk-arm64.aar` | 1,324,508 B | `7f7082c1f42f3837e50fe73a0646a4c7e25a02fd4adcda4f0018784dd6226b49` | SDK classes/resources, three arm64 native libraries, both customization contracts |

Both APKs list only `com.avatarforge.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION` and do not request
`INTERNET`; both have the non-exported `FileProvider`. The AAR carries the native libraries and
customization assets; its manifest is merged by the consuming app. Static package, zipalign, v2
signature, C ABI and JNI audits passed. The release/debug APK signer certificate SHA-256 is
`960c75a9648a74dc189f3510a6c7f80e7029a63680b5dbdea0a2d87b244e5514` (local debug key, not a store
key). The previous artifact set used a different certificate (`9d24f3af…`), so Android may reject an
in-place update. Export saved avatars before uninstalling any prior app; uninstall may erase its
private data.

The app's **☰ MENU → Live device report** refreshes thermal status, RAM, free storage, low-memory state
and CPU cores every second without USB/ADB or network. API 29+ thermal telemetry allows generation
through Severe (3), blocks at Critical (4)+, and cooperatively cancels an active job when Critical+
is detected; renderer quality drops to LOW at Severe. On API 26–28 the report honestly says thermal
telemetry is unavailable. This is compiled/JVM-tested policy, not hardware evidence. `tools/verify.sh`
re-checks APK permission/library claims; the physical-device acceptance pass is in **DEVICE.md**.

Install and run:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.avatarforge/com.avatarforge.app.MainActivity
```

### What to expect on a device

Planning estimates only — **UNVERIFIED**, not measurements or performance promises (no physical SoC run exists):

| Step | Estimated time (Snapdragon 778G-class) | Notes |
|---|---|---|
| Prompt/photo analysis | < 1 s | 256 px working grid, classic segmentation, no neural net |
| Geometry (0.008 m lattice) | 30–70 s | ~70 % of the total; the memory budget is enforced here |
| UV unwrap | 1–3 s | 14k charts, 65 % atlas coverage |
| Texture bake | 2–5 s | five maps, resolution selected from 2048² down to 256² by the budget |
| Rig + blendshapes | 8–20 s | 71 joints, 52 channels |
| Clips | < 1 s | 15 authored clips sampled and cached |
| GLB export | 1–2 s | 33–39 MB, validated structurally |

The SDK requests a 1.5 GiB native generation budget by default; it is not a guaranteed fixed RSS
ceiling. Before a build, the SDK may lower the request to measured available RAM minus a 512 MiB
headroom reserve (minimum effective request 64 MiB), and Android can reclaim memory at any time. The
on-device RSS target remains <1.5 GiB and must be measured. 30 FPS is the floor, 60 the target, and
the renderer has a budget-controller policy intended to step quality down when frames run long; device behavior must be measured against the acceptance criteria in `DEVICE.md`.

---

## 5. Rust crates

```bash
cd rust
AF_CORE_LIB_DIR=/home/user/AvatarForge/build-host cargo build --offline
cargo test --offline                       # unit tests
AF_CORE_LIB_DIR=/home/user/AvatarForge/build-host \
  cargo test --offline --release -p af --test engine -- --ignored --nocapture --test-threads=1
```

`af-core` emits `libafcore_rust.so` (not `libafcore.so`): the unique filename prevents Cargo's
workspace-test loader from shadowing the C++ reference library of the same ABI. Use
`python3 tools/abi_conformance.py rust/target/release/libafcore_rust.so --reference build-host/libafcore.so`
to check the Rust implementation.

`af-sys` binds the C ABI, `af` wraps it safely (`Engine`, `JobHandle`, `Spec`, `export_glb`, and an
optional `renderer` feature that drives the same renderer through its flat C ABI). The end-to-end
test builds a watertight avatar through the Rust API and inspects it — the same claims the C++
harness makes, asserted from a second language, which is what keeps the ABI honest.

---

## 6. Verifying an exported model

```bash
python3 tools/validate_glb.py /path/to/avatar.glb     # chunk layout, accessors, morph targets, skin
python3 tools/glb_census.py  /path/to/avatar.glb      # counts by primitive/material/animation
```

`validate_glb.py` prints `OK: all structural checks passed` on a file the engine produced. Both
scripts read the container by hand (no dependencies), so they work on a machine with nothing
installed but Python.

---

## 7. Verify everything at once

```bash
tools/verify.sh
```

Five sections, non-zero exit on the first failure: the kernel harness (464 checks), the shader tables
against their sources, the structural validation of an exported `.glb`, and an audit of the built
artifacts - including the one claim that must never regress, **no `INTERNET` permission in any APK**.

What that script cannot check is the device: rendering, frame timing, thermals and touch. That pass is
`DEVICE.md`.

