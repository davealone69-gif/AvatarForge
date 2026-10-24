# Black/blank viewer repair — 2026-10-24

## Scope and status

Target: **`work/android-engine-dev` at `9ec715723bfa0cccdf31c78e197fd365f2818908`**,
not `main` and not the older `21c1bc2` ancestor. The remote branch head was rechecked during
this repair and still matched this baseline. Changes are local; nothing has been pushed or merged.

This is a **locally build-verified and software-driver pixel-verified test candidate**, not a
physical-device verification or a production-readiness claim. The tests render a deliberately tiny
GLB triangle through the production backends. They do not establish the appearance, performance,
or lifecycle behaviour of a complete avatar on a Samsung Galaxy S24.

## Reproduced defects and repairs

- **GLES initialization:** sampler uniforms were set without selecting their owning program, and
  vertex attributes captured buffer zero because VBOs were generated after VAO setup. Correct
  program selection and buffer/VAO creation order eliminate `GL_INVALID_OPERATION`.
- **Uniform buffers in both APIs:** offset alignment was incorrectly treated as the size of a
  uniform block. The camera is 448 bytes, so 256-byte alignment requires a 512-byte slice. Camera,
  material and post-process slices now use independently rounded strides and valid allocations.
- **Vulkan initialization:** baked SPIR-V targeted Vulkan 1.1 while the instance requested 1.0;
  compute visibility/dynamic offsets were incomplete; the initial material descriptor referenced
  no buffer; disabled bloom still created pipelines against null render passes. These are corrected.
- **Blank Vulkan geometry:** the single-sample depth attachment was cleared as colour, and shadow
  comparison samplers did not match the shaders' ordinary depth samplers/manual PCF. Depth clearing
  and sampler types now match the actual attachments and shaders.
- **NaN/transparent pixels:** anisotropic fallback sampling with zero UV derivatives could poison
  alpha. Small/fallback textures use non-anisotropic sampling; compressed replacements retain the
  right selection. Disabled material maps are selected conditionally rather than combined with
  `mix(fallback, possiblyNaN, 0)`. A textured-to-untextured mesh replacement also exercises this fix.
- **Synchronization and rebuilds:** texture transfers and shadow/HDR/bloom dependencies now describe
  the real reads/writes, including bloom LOAD and cross-frame depth writes. Disabled shadows still
  clear/transition their sampled image. Pipeline layouts are retained across target rebuilds.
  The renderer waits for all frame owners before writing its still-shared mapped CPU buffers;
  a future per-frame-buffer redesign could recover more CPU/GPU overlap, but must not restore races.
- **Android loop/recovery:** adapted PR #16's separation of host intent from effective view/window/
  surface visibility rather than merging that branch wholesale. Detach stops scheduling before
  SurfaceView teardown. Queued callbacks are deduplicated. Failed creation/resize retries at a low
  rate with the engine lease retained, rather than cancelling the only frame callback permanently.
- **Honest watchdog:** JNI optionally appends a native frame sequence while preserving the previous
  nine-int output layout. A native fatal return or a non-advancing sequence cannot be hidden by old
  positive FPS/triangle counts. Errors clear only after fresh accepted-frame progress. GLES no longer
  counts a failed resize/swap as an accepted frame. Accepted frames are still **not** display proof.

Existing culling/LOD, GPU profiles, texture encoders, engine leases, thermal/performance-hint hooks,
and Vulkan surface/swapchain recovery are preserved. Historical checked-in binaries under `prebuilt/`
and `artifacts/` are not updated; rebuild from source or use the separately supplied new test APK. No branches or PRs were closed/deleted.

## Verification

| Local check | Result |
| --- | --- |
| Full native host regression | 468 checks, zero failures |
| Production GLES on Mesa/llvmpipe | 270 checks, zero failures |
| Production Vulkan on Mesa/lavapipe | 88 checks, zero failures and zero validation errors |
| Checked-in SPIR-V | All 13 modules validate for Vulkan 1.0 |
| SDK JVM tests | 26 tests, zero failures/errors; includes 8 scheduling/health policy tests |
| Android NDK build | All three ARM64 libraries built from current source with NDK 26.1.10909125 |
| Debug APK + release SDK AAR | Gradle build and unit-test tasks passed |
| Public kernel C ABI | 47/47 symbols and invariants passed; manifest matches |
| APK audit | Signature verifies; no INTERNET permission; three ARM64 libraries' ELF Build IDs match the new native build |
| Physical Android device / S24 | **NOT TESTED** |
| GitHub Actions for this patch | **NOT RUN — patch not pushed** |

GLES runs at UBO alignments 16, 64 and 256. Both tests run the real software-device profile and a
clearly labelled policy override to exercise supported MSAA and five bloom levels on Mesa. They
check actual red geometry pixels, empty-scene timing, alternating frame slices, quality changes,
texture reattachment, and resize. Vulkan enables synchronization validation and tests consecutive
submissions without a test-inserted idle between frames. GLES injects one transient presentation
failure; the corresponding `eglSwapBuffers failed (0x3003)` message is intentional and must not
advance the accepted-frame sequence. These are correctness tests, **not FPS benchmarks**.

A separate renderer-pixel job in `.github/workflows/android-apk.yml` now gates Android packaging.
It installs Mesa/validation/Xvfb, executes both driver tests, and retains their logs on failure.
This workflow change has been parsed locally, but has not run on GitHub.

### Run the checks

On Debian/Ubuntu:

```sh
sudo apt-get install g++ zlib1g-dev libegl1-mesa-dev libgles2-mesa-dev \
  mesa-vulkan-drivers libvulkan-dev vulkan-validationlayers libx11-dev xvfb xauth
bash tools/build_host.sh test
bash tools/test_renderer.sh
bash tools/test_renderer_vk.sh
python3 tools/abi_manifest.py --check
python3 tools/abi_conformance.py build-host/libafcore.so --require-complete
# glslc + spirv-val required for source shader compilation:
python3 tools/embed_shaders.py --check
```

The driver test scripts rebuild production support code when inputs change. Android-specific
window/log shims are isolated under `native/tests/support`; they are not on the Android build path.
No diagnostic shaders or hard-coded red rendering code are shipped in the app.

## CI provenance and remaining acceptance

The pre-existing baseline's Android workflow succeeded:
<https://github.com/davealone69-gif/AvatarForge/actions/runs/39998014453>.
That run is for **unmodified `9ec7157`**, not this repair. A green compilation-only run did not catch
the initialization/pixel/synchronization failures reproduced here.

PR #16 was inspected at `0e5f772d7608a7323f33f7143d56d07c22c9eb59`; its relevant visibility intent
logic was selectively adapted. Other open PRs are not implicitly merged or verified by this patch.

Before declaring the user-visible issue resolved on hardware:

1. Apply/commit this patch on the canonical branch, push, and verify both new pixel and Android
   packaging jobs for that exact new SHA. Do not use the baseline's green run as evidence.
2. Install the newly produced APK on the S24 (or the actual affected device). Check a complete
   avatar, non-zero fresh frame telemetry, resize/rotation, background/foreground, recovery,
   generation, GLB export/import and offline behaviour on Vulkan and GLES.
3. Capture logcat/Perfetto and run a sustained thermal/performance soak. In particular, assess the
   safety-first shared-buffer fence wait on hardware. Native waits are not a bounded GPU-hang
   watchdog; an actual driver/GPU hang still requires device-level investigation.
4. Keep device status **UNVERIFIED** until those results are recorded. The debug APK uses a test
   signing certificate and the local native build used `-O1` to fit the build sandbox; it is not a
   signed production release or a device performance reference.
