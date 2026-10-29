# GLES-only renderer migration — 2026-10-29

## Decision

Remove AvatarForge's native Vulkan renderer and ship OpenGL ES 3.1 as the only renderer backend. This decision supersedes the earlier Vulkan-primary / GLES-fallback design for this branch.

## Source changes made

- Removed `native/src/render/af_vk.cpp` and the generated SPIR-V table `native/src/render/af_shaders_vk.h`.
- Removed the Vulkan renderer and preference toggle from the native renderer interface.
- `Renderer::create()` now creates only the production GLES renderer and reports its actual initialization failure; it does not probe Vulkan or attempt backend fallback.
- Removed the Vulkan library from `native/CMakeLists.txt`; the Android native renderer links EGL/GLES and Android system libraries only.
- Removed the Vulkan preference from the C ABI mirror, JNI argument lists, Kotlin `RenderSettings`, saved settings, and optional Rust renderer binding. This requires the native/JNI/Kotlin signatures to be built together as one revision.
- Removed Vulkan-only result wording from diagnostics and retained backend-neutral error/fatal fields.
- Simplified camera and shadow projection math to OpenGL clip-space depth `[-1, 1]`.
- Changed shader generation to emit the GLES GLSL ES table only. `glslc`, when available, validates GLES stages with the OpenGL target; no SPIR-V is generated.
- Deleted Vulkan-only host tests and the Vulkan validation script.
- Removed Vulkan packages and test steps from the Android workflow; the GLES pixel test remains the renderer regression gate.
- Updated README and build instructions to state the GLES-only architecture.

## Preserved functionality

The change does not remove the avatar-generation kernel, GLB import/export, skeleton/skin weights, 52 blendshapes, animation clips, region picking, index-only LOD, GPU profiles, adaptive MSAA/shadow/bloom settings, GLES shaders, ASTC/ETC2 texture encoders, or the GLES headless pixel-regression suite.

## Important ANGLE distinction

The screenshot's reported renderer was `gles31 · ANGLE (Samsung Xclipse 940) on Vulkan 1.3.304`. The native Vulkan renderer has been removed from this branch. However, Android can route OpenGL ES calls through the system-selected ANGLE implementation, and that ANGLE build can itself use Vulkan internally. The app uses the platform's default EGL/GLES implementation and cannot reliably override a system graphics-driver choice from ordinary app code. Therefore, this source change proves that AvatarForge no longer links or calls its own Vulkan backend; it does **not** prove that the Android graphics stack uses zero Vulkan internally. The driver string must be checked on-device after installation.

## Verification status

- Source edits and removals on the isolated branch: **REAL + VERIFIED** in GitHub.
- Exact-head native/Android build: **PENDING** until CI finishes for the final SHA.
- GLES headless pixel and lifecycle regression: **UNVERIFIED on the final SHA** until the workflow finishes.
- APK native dependency inspection for absence of `libvulkan`: **UNVERIFIED** until a produced APK/shared library is inspected.
- Installed Samsung S24 viewer, sustained FPS, RAM/thermal use and actual ANGLE/native-driver path: **UNVERIFIED** until the exact APK is installed and tested.
- `main`: untouched. PR target remains `work/android-engine-dev`; do not merge before exact-head CI and device checks.
