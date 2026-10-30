# AvatarForge

An offline Android app for generating, auto-rigging, animating and rendering full-body 3D avatars
from text or one/two reference images. Image analysis supplies coarse framing and colour cues; it
does not reconstruct identity or transfer source-image geometry.

No server. No network of any kind. No game engine. The app must work in airplane mode, and an
`INTERNET` permission in the manifest is treated as a build failure.

```
text prompt ─┐
photo ───────┼──▶ anthropometry ─▶ SDF body field ─▶ isosurface ─▶ UV atlas ─▶ PBR bake
2 images ────┘         │                                              │
                       └──▶ skeleton ─▶ skin weights ─▶ 52 ARKit ─▶ 15 clips
                                                          blendshapes
                                                                      │
                              Native Vulkan (default) / GLES fallback ◀──┤
                              GLB export / GLB import ◀───────────────┘
```

## Status, in one paragraph

The default renderer is **native Vulkan**, with **OpenGL ES 3.1 fallback** if Vulkan startup or runtime
recovery fails. AvatarForge now gates that path on loader and physical-device Vulkan API 1.3 and
compiles/validates shaders for `vulkan1.3`. The requested target label `1.3.304` is logged separately;
`VkApplicationInfo` uses the standard `VK_API_VERSION_1_3` encoding (patch zero, since Vulkan ignores
that field). The pinned NDK 26.1.10909125 headers are revision 1.3.237, not 1.3.304. The supplied
SM-S921B checker report is separate capability evidence, not proof that AvatarForge selected Vulkan
or produced visible pixels. The Vulkan path is **unverified in the app on a Samsung GPU**; see
`docs/audit/VULKAN_TARGET_AUDIT.md` and `DEVICE.md` for current evidence and the on-device gate.
Host regressions cover UV-atlas overflow, skinning cost, image bounds and frame-timing validity.
Visible pixels, sustained performance/thermal behaviour and production signing remain unverified
or BLOCKED. See `docs/VULKAN_RESTORE_2026-10-30.md` and `DEVICE.md`.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/afcore.h` | **The shared C ABI** — the single contract every native implementation answers to |
| `native/src/` | C++17 engine kernel: anatomy → field → isosurface → UV → bake → rig → blendshapes → clips → GLB |
| `native/src/render/` | Native Vulkan-default renderer with GLES 3.1 startup/recovery fallback; no game engine |
| `native/src/android/` | JNI bridge |
| `rust/af-core/` | Rust implementation of the same ABI — engine logic, anatomy, prompt grammar, tables |
| `rust/af-sys/`, `rust/af/` | Raw FFI bindings and the safe Rust wrapper |
| `app/` | Jetpack Compose UI (input → progress → viewer/editor → export) |
| `sdk/` | Documented Kotlin SDK (`AvatarEngine.health/capabilities/generateFromText/generateFromImage/…`) |
| `abi/afcore.abi.json` | Generated ABI manifest — sizes, offsets, enum values, symbol list |
| `tools/` | Build scripts and every verification gate (see below) |
| `engine/` | Renderer-side assets/shaders support |
| `STATUS.md`, `BUILD.md`, `DEVICE.md` | Where it stands · how to build · how to verify on hardware |

## Build

```bash
tools/bootstrap_toolchain.sh          # SDK + NDK + CMake + Gradle + JDK 17 + Rust (once)

tools/build_host.sh test              # kernel host harness (no device, no Android SDK)
bash tools/test_model_host.sh         # focused hand/garment detail regressions
tools/build_android.sh                # arm64 native libraries into prebuilt/
tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease
```

Full details, including the 2 GB container recipe and JDK/SDK paths, are in **`BUILD.md`**.

## Verification

Every one of these is a gate that fails loudly; none of them needs a device.

```bash
tools/verify.sh                       # everything below, in one run
tools/build_host.sh test              # kernel checks: geometry, rig, bake, ABI, GLB round trip, scene
bash tools/test_renderer_host.sh      # GLES fault injection + Vulkan selector double (no GPU)
python3 tools/test_renderer_artifacts.py # dual API ELF/package guard tests
python3 tools/test_embed_vulkan_shaders.py # pinned-source/table provenance regressions
python3 tools/embed_vulkan_shaders.py --check # requires NDK glslc + spirv-val for pinned table
python3 tools/abi_manifest.py --check # the checked-in manifest still matches afcore.h
python3 tools/abi_conformance.py build-host/libafcore.so --require-complete
python3 tools/abi_conformance.py rust/target/release/libafcore.so --reference build-host/libafcore.so
tools/prompt_parity.sh                # C++ and Rust prompt grammars, diffed over a 72-prompt corpus
python3 tools/validate_glb.py <file>  # exported GLB structure + accessor bounds
```

On hardware:

```bash
tools/device_check.sh --apk AvatarForge-debug-arm64.apk --soak 120
```

## Stability self-improvement path (no AI)

AvatarForge has a bounded, offline renderer-stability loop: diagnostics -> safe fallback -> private
fault journal -> frame-submission probe -> exportable regression report. It does not use AI, upload
data, or rewrite code. Open **MENU -> Self diagnostics** to copy/export the report, run **Safe
recovery**, or clear the journal. See [docs/SELF_IMPROVEMENT_PATH.md](docs/SELF_IMPROVEMENT_PATH.md).
The probe proves sustained non-empty frame submission, **not** visible pixels; device visibility still
requires human confirmation.

```bash
./gradlew :sdk:testDebugUnitTest      # recovery policy/probe/journal JVM regressions
bash tools/test_renderer_host.sh      # renderer fault-injection regressions
```

## Design rules this codebase holds itself to

* **Airplane mode is the normal case.** No HTTP, sockets, telemetry, or remote model fetches. Models
  and clips ship in the APK.
* **No fakes.** No placeholder generation, no disabled buttons, no feature that reports success
  without doing the work. When something is unsupported the code says so with a specific reason —
  e.g. a GLB import refuses to half-adopt a foreign skeleton and reports `joint_count == 0` rather
  than looking rigged and not animating.
* **A missing measurement is never a zero.** The frame profiler reports `-1` for GPU timings on
  devices that cannot measure them instead of a plausible-looking `0.0`.
* **The custom renderer stays custom.** Native Vulkan is the default, with GLES 3.1 as explicit
  choice and bounded fallback; no third-party engine is linked.
* **Determinism.** The same spec produces byte-identical geometry, and that is asserted, not assumed.

## Licence

Not yet chosen. Add a `LICENSE` before publishing this repository.


## Download and install the Android APK

**Fastest route:** open the [Android APK workflow](https://github.com/davealone69-gif/AvatarForge/actions/workflows/android-apk.yml) and open the latest successful run. Under **Artifacts**, download `AvatarForge-Android-<commit>` and extract the ZIP. Install `AvatarForge-debug-arm64.apk` for testing on a modern ARM64 Android phone.

For a versioned production download, open [GitHub Releases](https://github.com/davealone69-gif/AvatarForge/releases). A version tag starts a separate signed-release job; it requires production `AF_RELEASE_*` credentials and does not publish if they are missing.

Release signing is fail-closed: there is no fallback debug certificate. The CI unsigned AAB is compile verification only, not a production release. The debug APK is the installable test artifact. Never commit a release keystore or its passwords.

Install from the phone: download the APK, open it in Downloads, and allow installation from that source if Android asks. The APK is ARM64 and targets Android 8.0 (API 26) or newer. AvatarForge is intended to work offline and its APK must not request the `INTERNET` permission.


## Renderer recovery retest (2026-10-21)

A separate recovery build is available for retesting the reported black-viewer / 0 FPS failure. Its physical-device result is still **UNVERIFIED**. See [renderer recovery notes and checksums](docs/RENDERER_RECOVERY_2026-10-21.md) for the bundle identity, exact checksums, download source, and device acceptance checklist. The APK/AAR binaries are not GitHub release assets yet; the notes link to the original Dropbox bundle.
