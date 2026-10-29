# Building AvatarForge

Everything here builds on a Linux host. There is no cloud step, no account, no service to run, and
no generated code that is not checked in: a clone plus a toolchain is enough to produce the APK.

The project has four layers, and each can be built and tested on its own:

| Layer | Sources | What it is | How it is built |
|---|---|---|---|
| Kernel | `native/src/*.cpp`, `native/include/afcore.h` | anatomy → SDF field → isosurface → UV → PBR bake → rig → blendshapes → clips → GLB, behind a stable C ABI | `tools/build_host.sh` (host) / CMake+NDK (Android) |
| Renderer | `native/src/render/` | OpenGL ES 3.1 only, written from scratch | CMake+NDK (Android only: EGL/ANativeWindow); GLES shaders baked on any host |
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
export PATH=$JAVA_HOME/bin:/usr/local/tc/gradle-8.9/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:/usr/local/tc/cargo/bin:$PATH
```

Which packages are needed and why is documented at the top of the script; the short version is that
AGP 8.x needs JDK 17, the renderer links `EGL`/`GLESv3`/`android`/`log` from the NDK
sysroot, and the kernel needs only zlib (provided by the Android platform, `-lz` on a host).

Hardware: this project is developed and verified on a 2 vCPU / 2 GB container. Configure Gradle
accordingly (`gradle.properties` already does: no daemon, no parallel module builds, 1.5 GB heap).

---

## 1. Kernel + host test harness (no device, no Android toolchain)

```bash
tools/build_host.sh test     # builds build-host/libafcore.so and runs the 386-check harness
tools/build_host.sh kernel   # library only
tools/build_host.sh all      # library + harness binary, do not run
```

Expected output ends with:

```
All 386 checks complete: 0 failure(s)
```

The harness covers anthropometry (waist/hip/crotch exactness against tape measurements), signed
distance field sanity, isosurface integrity (closed shell, no boundary loops, plausible volume for
four body variants), determinism (identical spec ⇒ identical mesh), the rig (bind-pose exactness,
fingertip/toe skinning, all 52 ARKit channels carrying real displacement), UV/PBR bake statistics,
apparel, the embedded shader tables, a full end-to-end pass through the public C ABI, and pose
composition (`[12]`: `AF_CLIP_REST` restores the bind pose *exactly*, and playing a clip, returning to
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

* `native/src/render/af_shaders_gl.h` — the complete GLSL ES 3.10 source table (GLES 3.1)

Regenerate after editing anything in `native/src/render/shaders/`:

```bash
python3 tools/embed_shaders.py           # deterministic text embedding; no compiler required
python3 tools/embed_shaders.py --check   # requires glslc: compile every stage and reject a stale table
```

The script embeds exactly the 12 required GLES stages and fails loudly on a missing, duplicate or
unexpected stage. With `glslc` present (NDK shader-tools or `AF_GLSLC`) it type-checks every stage
using `--target-env=opengl`. The kernel harness ([10]) verifies stage coverage, GLSL ES version and
explicit null lookup for unknown names. There is no SPIR-V table and no alternate graphics API.

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
links EGL/GLESv3/android/log), and the JNI bridge (`avatarforge`). The Gradle build runs
exactly this through `externalNativeBuild`; the manual invocation is there so the native side can be
iterated without Gradle in the loop, and so the `.so` files can be built once and packaged many times
(`-Paf.prebuiltJni=/path/to/prebuilt`, which is also the way to build the APK on a machine that has
no NDK - see the RAM note in §4).

`tools/build_android.sh` produces `prebuilt/arm64-v8a/{libafcore,libafrender,libavatarforge}.so`
(static-STL; Gradle strips them when packaging). `prebuilt/` is generated and git-ignored: stale
Vulkan-era binaries were removed from tracking, and `tools/verify_gles_artifacts.py` rejects any
prebuilt or packaged library that still links a Vulkan loader. `tools/build-android.ps1` is the
Windows port and has **not** been executed here; use `-DryRun` to inspect its command lines.

---

## 4. APK

```bash
# Preferred: the wrapper pinned in gradle/wrapper (Gradle 8.9, downloads its distribution once)
./gradlew :app:assembleDebug                 # -> app/build/outputs/apk/debug/app-debug.apk
./gradlew :app:assembleRelease               # signed: keystore.properties / AF_RELEASE_*; otherwise BLOCKED
./gradlew :sdk:assembleRelease               # the SDK as an AAR for third-party consumers

# This container / any machine that already has Gradle: the pinning wrapper
tools/build_gradle.sh :app:assembleDebug -Paf.abIs=arm64-v8a -Paf.prebuiltJni=$PWD/prebuilt
```

`local.properties` (git-ignored) or `ANDROID_HOME` must point at the SDK; `ndkVersion` in both
modules must match the installed NDK (26.1.10909125). ABIs built: `arm64-v8a` (primary) and
`x86_64` (emulator).

Build properties, all optional:

| Property | Effect |
|---|---|
| `-Paf.abIs=arm64-v8a` | build one ABI instead of two (halves the native build time and its peak RAM) |
| `-Paf.prebuiltJni=$PWD/prebuilt` | package the `.so` files from `prebuilt/` and skip CMake entirely - the way to build the APK on a small machine |
| `-Paf.lowRam=true` | `-O1` for the natives plus a single-threaded CMake build |

Release signing is fail-closed: supply `keystore.properties` (storeFile / storePassword / keyAlias /
keyPassword) or `AF_RELEASE_*` environment variables. Without them, release assembly is BLOCKED.
CI may compile an explicitly unsigned AAB with `-Paf.allowUnsignedRelease=true`; that artifact is
labelled UNSIGNED and is not a production release. R8 is deliberately off; see `app/build.gradle.kts`.

### Artifacts

Generated binaries are no longer tracked. The authoritative deliverables are the exact-head CI
artifacts from the **Android APK** workflow: `AvatarForge-debug-arm64.apk`, an explicitly
`AvatarForge-release-UNSIGNED-arm64.aab` (compile verification only), `AvatarForge-sdk.aar`,
`SHA256SUMS` and `PROVENANCE.json`. CI rebuilds the ARM64 natives from the checkout, verifies
GLES-only linkage, checks the APK/AAB/AAR, and uploads the result. Production signed AAB/APK delivery
requires real signing credentials and remains BLOCKED until they are supplied.


## 5. Rust crates

```bash
cd rust
AF_CORE_LIB_DIR=/home/user/AvatarForge/build-host cargo build --offline
cargo test --offline                       # unit tests
AF_CORE_LIB_DIR=/home/user/AvatarForge/build-host \
  cargo test --offline --release -p af --test engine -- --ignored --nocapture --test-threads=1
```

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

Five sections, non-zero exit on the first failure: the kernel harness (386 checks), the shader tables
against their sources, the structural validation of an exported `.glb`, and an audit of the built
artifacts - including the one claim that must never regress, **no `INTERNET` permission in any APK**.

What that script cannot check is the device: rendering, frame timing, thermals and touch. That pass is
`DEVICE.md`.
