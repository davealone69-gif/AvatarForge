# AvatarForge

A standalone Android app that **generates, auto-rigs, animates and renders a photorealistic
full-body 3D avatar entirely on the device** — from a text prompt, a photo, or a two-image
multi-reference flow.

No server. No network of any kind. No game engine. The app must work in airplane mode, and an
`INTERNET` permission in the manifest is treated as a build failure.

```
text prompt ─┐
photo ───────┼──▶ anthropometry ─▶ SDF body field ─▶ isosurface ─▶ UV atlas ─▶ PBR bake
2 images ────┘         │                                              │
                       └──▶ skeleton ─▶ skin weights ─▶ 52 ARKit ─▶ 15 clips
                                                          blendshapes
                                                                      │
                              Production GLES 3.1 / experimental Vulkan renderer ◀──┤
                              GLB export / GLB import ◀───────────────┘
```

- [Adult model, render and video integration](docs/ADULT_MODEL_RENDER_VIDEO_INTEGRATION.md): Grok-Girls visual/video port scope, Blender/MPFB2 anatomy pipeline, real 3D video requirements and acceptance gates.
## Status, in one paragraph

The current native source passes **464 host checks with 0 failures**; the C++ kernel implements **47/47**
ABI symbols, while the Rust alternative implements **15/47** and matches the C++ reference on the
functions it covers. Host coverage includes culling, index-only renderer LOD data, adaptive atlas
budget selection, GPU-profile policy, ASTC/ETC2 encoding and mip generation, an ASTC decode round-trip,
asynchronous compression polling, a staged native pipeline, and image-role mapping after a rejected
image. GL/Vulkan sources pass host syntax-only compilation with downloaded Khronos headers and an
Android-window stub; CMake host configuration and the scalar ASTC/ETC2 static codec targets also build.
This is **not** an Android NDK build or a GPU upload/runtime test. Kotlin lifecycle, dependency-graph,
thermal and resource changes remain **uncompiled**. The compressed GL/Vulkan upload paths are integrated
but have not been exercised on hardware. The checked-in 0.10.1 APK/AAR predate this follow-up.
**Nothing has been run on a physical Android device**; `DEVICE.md` is the hardware checklist and
`tools/device_check.sh` collects evidence when a device is available.

## Repository layout

| Path | What it is |
|---|---|
| `native/include/afcore.h` | **The shared C ABI** — the single contract every native implementation answers to |
| `native/src/` | C++17 engine kernel: anatomy → field → isosurface → UV → bake → rig → blendshapes → clips → GLB |
| `native/src/render/` | Custom OpenGL ES 3.1 production renderer and experimental Vulkan backend, GPU profiles and compressed atlas uploads |
| `native/third_party/` | Vendored Apache-2.0 ASTC Encoder and Etc2Comp codecs |
| `native/src/android/` | JNI bridge |
| `rust/af-core/` | Rust implementation of the same ABI — engine logic, anatomy, prompt grammar, tables |
| `rust/af-sys/`, `rust/af/` | Raw FFI bindings and the safe Rust wrapper |
| `app/` | Jetpack Compose UI (input → progress → viewer/editor → export) |
| `sdk/` | Documented Kotlin SDK (`AvatarEngine.health/capabilities/generateFromText/generateFromImage/…`) |
| `abi/afcore.abi.json` | Generated ABI manifest — sizes, offsets, enum values, symbol list |
| `tools/` | Build scripts and every verification gate (see below) |
| `engine/` | Renderer-side assets/shaders support |
| `STATUS.md`, `BUILD.md`, `DEVICE.md` | Where it stands · how to build · how to verify on hardware |

## SDK host-app integration

The SDK is a separate Android library, but third-party consumption through a produced AAR has not yet been independently runtime-verified. See [Host-app integration guide](docs/SDK_HOST_INTEGRATION.md) for local Maven publication, dependency metadata, ABI packaging, singleton/resource constraints, and the required clean-consumer smoke test. Do not treat the archived AAR under `artifacts/` as current; rebuild from the exact source commit before consuming it.

## Build

```bash
tools/bootstrap_toolchain.sh          # SDK + NDK + CMake + Gradle + JDK 17 + Rust (once)

tools/build_host.sh test              # kernel + 464-check host harness (no device, no Android SDK)
tools/build_android.sh                # arm64 native libraries into prebuilt/
tools/build_gradle.sh :app:assembleDebug :sdk:assembleRelease
```

Full details, including the 2 GB container recipe and JDK/SDK paths, are in **`BUILD.md`**.

## Troubleshooting

Start with [DIY Fault-Finding](docs/DIY_FAULT_FINDING.md), a plain-language checklist tied to the status and measurements shown in the app. For black viewer / zero-FPS diagnosis and the evidence required before calling a renderer fix verified, see [Renderer Recovery and Verification](docs/RENDERER_FIX.md).

## Verification

Every one of these is a gate that fails loudly; none of them needs a device.

```bash
tools/verify.sh                       # everything below, in one run
tools/build_host.sh test              # 464 checks: geometry, render profiles/compression, ABI, GLB, LOD + stages
python3 tools/abi_manifest.py --check # the checked-in manifest still matches afcore.h
python3 tools/abi_conformance.py build-host/libafcore.so --require-complete
python3 tools/abi_conformance.py rust/target/release/libafcore_rust.so --reference build-host/libafcore.so
tools/prompt_parity.sh                # C++ and Rust prompt grammars, diffed over a 72-prompt corpus
python3 tools/validate_glb.py <file>  # exported GLB structure + accessor bounds
```

On hardware:

```bash
tools/device_check.sh --apk artifacts/avatarforge-debug-arm64.apk --soak 120
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
* **The custom renderer stays custom.** OpenGL ES 3.1 is the production priority for the target Galaxy S24; Vulkan remains experimental and must not block GLES delivery. No third-party game engine is linked.
* **Determinism.** The same spec produces byte-identical geometry, and that is asserted, not assumed.

## Engineering quality directive (2026-11-15)

**Standing objective: build the best practical digital-human engine, correctly the first time.**
This is an engine-wide quality requirement, not a hair-only feature request and not permission to
replace working systems indiscriminately.

- **Quality before convenience:** target convincing, anatomically coherent digital humans with
  high-quality geometry, materials, lighting, rendering, animation and asset fidelity. Performance
  is an engineering constraint; do not use it as an excuse for avoidable visual defects.
- **Research before consequential engineering decisions:** for each major upgrade, investigate
  current authoritative specifications, platform/API constraints, proven production techniques and
  relevant device/GPU behavior. Compare viable approaches, record sources and trade-offs in the
  implementation notes, then choose and implement. Do not turn research into a reason to delay
  ordinary coding.
- **One durable architecture:** inspect and reuse the best existing implementation; replace only
  demonstrably inadequate machinery. No disposable prototypes presented as production, duplicate
  pipelines, competing renderers, superficial patches or repeated rebuilds caused by avoidable
  architectural shortcuts.
- **Target-device truth:** the primary phone target is Samsung SM-S921B, Exynos 2400, Xclipse 940,
  Android API 37. OpenGL ES is the production renderer priority. Vulkan is experimental for this
  device and must not block GLES. Any backend or feature claim requires compilation and runtime
  evidence on the relevant target.
- **Measure, do not guess:** profile CPU, GPU, memory, draw calls, overdraw, frame pacing and thermal
  behavior. Missing measurements remain unavailable, never zero. Keep visual comparisons and
  device results tied to exact source and artifact identities.
- **Quality gates:** validate topology, normals, UVs, tangents, materials, skeletons, animation,
  asset packaging and LODs automatically where possible. Review real multi-angle rendered output.
  A successful host test or build is not proof of visual quality or physical-device behavior.
- **No premature completion:** label every claim REAL + VERIFIED, UNVERIFIED, PARTIAL, MISSING or
  BLOCKED. "Fixed" requires evidence for the affected behavior; Android runtime acceptance requires
  installation and testing on the installed app on the target phone.
- **Implementation ownership:** after a short baseline inspection and focused research, execute the
  work continuously. Deliver code, tests, build artifacts and a concise README handover, not another
  plan in place of implementation. Keep `main` stable and do not merge or force-push without
  authorization.

The quality roadmap is engine-wide: (1) geometry and rendering foundation; (2) reusable asset
authoring, baking, packaging and validation; (3) AI-assisted generation, correction and QA;
(4) animation and physics; (5) measured S24 performance and thermal adaptation. These are
integrated production capabilities, not isolated demos. Existing capabilities must be audited
against this standard before being rebuilt.

**Status of this directive:** recorded in documentation only. It does not claim that these upgrades
have been implemented, built, visually accepted or device-verified.

## Licence

Not yet chosen. Add a `LICENSE` before publishing this repository.


## Download and install the Android APK

**Quick links:** [Download builds from GitHub Actions](https://github.com/davealone69-gif/AvatarForge/actions/workflows/android-apk.yml) · [GitHub Releases](https://github.com/davealone69-gif/AvatarForge/releases) · [Current Vulkan recovery PR #14](https://github.com/davealone69-gif/AvatarForge/pull/14)

**Fastest route:** open the [Android APK workflow](https://github.com/davealone69-gif/AvatarForge/actions/workflows/android-apk.yml) and open the latest successful run. Under **Artifacts**, download `AvatarForge-Android-<commit>` and extract the ZIP. Install `AvatarForge-debug-arm64.apk` for testing on a modern ARM64 Android phone.

For a versioned, easy-to-find download, open [GitHub Releases](https://github.com/davealone69-gif/AvatarForge/releases). Pushing a version tag such as `v0.10.0` runs the same build and attaches the debug APK, release APK, and SDK AAR to that release.

The release APK currently uses the repository's fallback debug signing certificate unless `keystore.properties` is supplied to a local build. It is suitable for testing, not a production signing identity. Never commit a release keystore or its passwords.

Install from the phone: download the APK, open it in Downloads, and allow installation from that source if Android asks. The APK is ARM64 and targets Android 8.0 (API 26) or newer. AvatarForge is intended to work offline and its APK must not request the `INTERNET` permission.


## Update imported from Dropbox (2026-10-21)

The update notes and verification records have been imported into
[`docs/dropbox-update-2026-10-21/`](docs/dropbox-update-2026-10-21/). This is documentation and build-evidence
synchronization only: the shared Dropbox folder contains APK/AAR binaries and documentation, **not a full
source checkout**. The binaries have not been uploaded into this repository by this change, and this
commit does not claim a new build or a phone-runtime pass.

- [Updated status and evidence](docs/dropbox-update-2026-10-21/STATUS.md)
- [Build instructions](docs/dropbox-update-2026-10-21/BUILD.md)
- [Physical-device acceptance checklist](docs/dropbox-update-2026-10-21/DEVICE.md)
- [Build 0.10.1 evidence](docs/dropbox-update-2026-10-21/BUILD-0.10.1.txt)
- [SHA-256 manifest for the Dropbox artifacts](docs/dropbox-update-2026-10-21/SHA256SUMS.txt)
- [Open the original Dropbox folder to obtain the APK/AAR files](https://www.dropbox.com/scl/fo/s22v1herlm6dgzc825gx7/AOAZnyUA6jJ3KC1NXT_pXWs?rlkey=b7jvxccotfdaqoab72c88x9qv&st=bml9bve2&dl=0)

**Runtime status remains UNVERIFIED:** the update's own report says the Galaxy S24 installation, rendering,
offline generation, GPU behavior, performance and thermal/lifecycle acceptance have not been tested.


## Architecture boundary and preserved known-good handoff (2026-11-15)

**Do not turn the whole ecosystem into one monolithic application.** These are separate concerns with explicit boundaries:

- **AvatarForge** remains the standalone engine/SDK and engineering test lab. It owns avatar generation, rigging, deformation, animation, rendering, asset validation and engine diagnostics. It must remain independently buildable/testable and usable by more than one host.
- **3D-Mega-Hub** is the intended integrated end-user product/host app. It may consume AvatarForge through its SDK/native interface as an embedded engine component. That does **not** mean copying the entire AvatarForge app/UI into the Hub, merging the repositories, or coupling unrelated services and tools into one Android process.
- **3DDD and other donor projects** remain separate donor/source projects. Reuse only proven components behind narrow interfaces; preserve donor identity, history and independent testability.
- Keep engine, SDK, host UI, asset pipeline, AI/provider adapters, voice, storage and optional services as separately owned modules/processes where their lifecycle, permissions, dependencies or resource needs justify it. One user-facing installable product is a packaging decision, not a mandate for one codebase/module/process.
- Do not create a competing AvatarForge replacement inside 3D-Mega-Hub. Integrate the proven engine capability, not a second engine.

### User-supplied baseline: preserve, do not ask for it again

The user previously supplied a working pre-integration AvatarForge APK in Dropbox:

- File: `AvatarForge-pre-integration-debug-f12a35acefd1 (1).apk`
- Dropbox location: `/AvatarForge/3d engine/AvatarForge-pre-integration-debug-f12a35acefd1 (1).apk`
- Size: **11,026,696 bytes**
- Historical recorded test evidence: **5/5 app tests passed and 43/43 SDK tests passed**; JobHandle retention was included in the SDK coverage. The contemporaneous handoff described the Samsung engine as working/visible.
- Classification: **HISTORICAL KNOWN-GOOD RECOVERY BASELINE** for comparison and regression recovery. This is not evidence that the current branch or current APK has the same behavior, and it is not a current S24 verification result. Preserve the original unchanged; compare new builds against it and never overwrite/relabel it as a newer build.

The user also supplied a full source snapshot in Dropbox:

- `/Update/AvatarForge-fixed-3b98600/AvatarForge/`
- This is a source tree, not merely an APK: it contains `app/`, `engine/`, `native/`, `sdk/`, `rust/`, `abi/`, `tools/`, `docs/`, `requirements/`, `prebuilt/`, `artifacts/`, build files, `README.md`, `PROJECT_STATUS.md`, `BUILD.md`, `DEVICE.md` and `DEVELOPMENT.md`.
- Treat this as an existing user-supplied implementation/reference snapshot. Inspect and compare it before asking for any skeleton, rig, source copy or test baseline again. Do not assume it is identical to the live GitHub head; verify exact files/SHAs before selectively porting.
- Source snapshot checkpoint: [Dropbox PROJECT_STATUS.md](https://www.dropbox.com/scl/fi/bwbd3qrr432poip5epnpv) and [Dropbox README.md](https://www.dropbox.com/scl/fi/lgkron1i7uco8yhiylr4r).

**Resume rule:** Before requesting a user re-upload or proposing a replacement skeleton, inspect this Dropbox source snapshot and the preserved pre-integration APK first. The user has already supplied the baseline. If a file is inaccessible, state the exact access limitation and continue with accessible material; do not ask them to resend something already located.

### Evidence distinction

- **REAL + VERIFIED (historical record):** the original supplied APK was recorded as passing 5/5 app tests and 43/43 SDK tests at handoff time.
- **UNVERIFIED (current):** whether that APK still installs/runs on the present Android version, and whether the Dropbox source snapshot is byte-for-byte represented by any current GitHub branch.
- **UNVERIFIED (current):** current Hub integration of the complete AvatarForge engine. A README, copied source tree, AAR or passing host test alone does not prove runtime integration.
- No current device test is implied by this handoff record.



## Interim product architecture and adult-anatomy status (2026-11-15)

- **Do not split the product into multiple apps yet.** Keep the integrated user experience together while the avatar quality/runtime foundations are being brought to target quality. Preserve clear internal subsystem/API boundaries and independent build/test seams so a later split is possible without a rewrite. Reassess splitting only when measured complexity, ownership, release cadence, security or performance justifies it.
- AvatarForge remains the reusable engine/SDK and test lab; it is not being converted into a second competing product UI. Integration into 3D-Mega-Hub must preserve AvatarForge's standalone buildability.
- **Adult anatomy: PARTIAL / UNVERIFIED.** The branch contains an adult-only customization contract at `config/customization/adult-anatomy.inputs.json`, a user input schema at `requirements/custom-inputs/adult-anatomy.user.json`, and SDK safety tests at `sdk/src/test/java/com/avatarforge/sdk/AdultSafetyTest.kt`. The contract explicitly marks all detailed genital/anatomy controls `BLOCKED_ENGINE_GEOMETRY` and says it is not proof geometry exists. The SDK gate requires explicit adult-mode confirmation and age 18+, and rejects minor/teen prompt markers for unclothed generation. These are code/test contracts, not proof of a working adult anatomical mesh or rendered output.
- Keep adult mode disabled by default, separately opt-in, with a clear 18+ gate, adult-only fictional subjects, consent/ownership checks for reference material, private handling of adult assets, and provider-policy checks. Reject minors and ambiguous-age prompts. Never report a provider as supporting adult output without a successful end-to-end generation test.
- Next adult-engine work is not a UI toggle: implement and validate the missing anatomy geometry/deformation/material/LOD path behind the existing contract, then verify actual rendered output and performance on GLES. Keep controls hidden while their engine geometry remains blocked.
