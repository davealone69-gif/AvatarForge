# Device evidence pack — full-screen atlas defect (AF-020)

Date: 2026-10-25 (Australia/Perth). Device: Samsung SM-S921B (Galaxy S24, Exynos 2400,
Xclipse 940), Android 17 (API 37), arm64-v8a. Renderer on every captured frame:
`gles31 · ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304) · OpenGL ES 3.2`, driver
`256.4 / 9fe6623ec0db`. This document consolidates the two chat-only analysis reports from
2026-10-25 so the evidence lives in the repository; the screenshots themselves are archived
alongside it under `screenshots/`.

Status labels follow the project convention: CONFIRMED means directly evidenced; UNVERIFIED
means not yet observed; hypotheses are labelled as such. Nothing here claims a device-verified
fix — the sandbox has no NDK/device, and no post-fix build has been run on hardware.

## 1. Screenshot ledger (all 1080×2340 JPEG, source: user's Google Drive export)

| File (this folder) | Original filename | Renderer card | Generation time / memory | What is shown |
|---|---|---|---|---|
| `SM-S921B_20261024_191611_vulkan-labelled_body-visible.jpg` | `Screenshot_20261024_191611_Grok Faces.jpg` | labelled "vulkan" (verbatim; see note) | 18 s | The skinned 3D avatar. AF-010 mip rippling and the AF-014 white head cap are both visible. |
| `SM-S921B_20261024_230452_gles-angle_atlas.jpg` | `Screenshot_20261024_230452_Grok Faces.jpg` | gles31 / ANGLE | 33 s, 37.5 MB | Full-screen packed texture atlas; 58 FPS, 15 draw calls. |
| `SM-S921B_20261025_000636_gles-angle_atlas.jpg` | `Screenshot_20261025_000636_Grok Faces.jpg` | gles31 / ANGLE | 37 s, 37.6 MB | Full-screen packed texture atlas, 58 FPS. |
| `SM-S921B_20261025_000843_gles-angle_atlas.jpg` | `Screenshot_20261025_000843_Grok Faces.jpg` | gles31 / ANGLE | 40 s, 38.9 MB, 1.78 M verts / 426,170 tris | Full-screen packed texture atlas, 58 FPS; primary pixel-analysis subject. |

Note on the "vulkan" label: the older card's text is recorded verbatim. All numeric renderer
reports from this device identify the active backend as GLES31 via ANGLE-on-Vulkan; native
Vulkan initialisation is declined (see AF-018). Whether the older build truly executed a Vulkan
swapchain is itself UNVERIFIED, but its pixels are unambiguously a rendered 3D body rather than
an atlas.

## 2. Why the image is a full-screen textured quad, not a rasterised mesh

Method (all arithmetic reproducible with PIL/numpy on the archived JPEGs):

1. **Clear-colour accounting.** The GLES forward pass clears to RGB (14,19,28)
   (`af_gl.cpp::drawForwardPass`, `glClearColor(0.055, 0.075, 0.11, 1)`). In `000843`, only
   **1.4 %** of render-area pixels match that dark bluish clear colour. If the scene were a mesh
   drawn on a cleared background, the clear colour would dominate everything outside the figure.
2. **Atlas background accounting.** **65.1 %** of render-area pixels are neutral gray with mean
   RGB ≈ (124,123,123) — the texture packer's background fill. For comparison, the extracted
   sample baseColor atlas is 70.6 % the same neutral gray. Edge/corner samples of the screenshot
   are that gray at all four corners, i.e. the texture covers the full surface.
3. **Content match.** The five baked textures were extracted from
   `artifacts/avatarforge-sample.glb` (PNG bufferViews 120–124; bin chunk begins at byte offset
   70176). The baseColor image (2048², 735 KB PNG) shows exactly the on-screen structure: gray
   packer background, the hair-dot band, and the armour/cloth UV islands. A normalised
   cross-correlation against the on-screen avatar region returned ~0.056 (other four maps
   ≤0.058) — **inconclusive and expected**, because the on-screen avatar was generated from
   different source art than the sample GLB; structural/background matching is the reliable
   signal, pixel matching across avatars is not, and should not be retried.
4. **Geometry is still submitted.** The on-screen stats (15 draw calls, 417–426 k tris) prove
   the forward pass records draws; the draw counter is not visual proof of a rendered body.
5. **Not quality-tier dependent.** The atlas appears at adaptive tier 2 and tier 1, so the
   `targetsDirty_` framebuffer rebuild path is not the trigger. The defect is in the fundamental
   GLES/ANGLE execution path.

## 3. Concrete code defect found: double-buffered camera UBO overrun

Facts established by arithmetic and source trace (status: CONFIRMED defect; causal link to the
atlas image UNPROVEN):

- `af::CameraUbo` is **448 bytes**, pinned by `static_assert(sizeof(CameraUbo) == 448)` in
  `native/src/render/af_render_common.h`.
- GLES before the fix: the camera buffer was allocated `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT * 2`
  bytes and each frame used `frame * uboAlignment_` as its offset. On the common Android
  alignment of 256 that is a 512-byte buffer; frame 1's upload/bind covers [256, 704) —
  **192 bytes past the end** at both `glBufferSubData` and `glBindBufferRange`. GLES raises GL
  errors and an invalid/zeroed binding rather than silently corrupting adjacent memory, which is
  a plausible mechanism for a broken forward pass but does not by itself explain an atlas image.
- Vulkan had the same latent pattern (`uboAlignment_ * kFramesInFlight` allocation, matching
  memcpy and seven dynamic-offset sites). It rendered acceptably only because the view/projection
  matrices are the block's first members and remain inside slot 0; later members (SH irradiance
  etc.) could read out of range on strict drivers.
- The fix (same branch, 2026-10-25): new constexpr helper `af::afRoundUp` in the
  dependency-free header `native/src/render/af_align.h`; both backends now compute
  `cameraSlotSize_ = afRoundUp(sizeof(CameraUbo), uboAlignment_)` (512 at 256-byte alignment),
  allocate two padded slots (1024 bytes) and use the slot stride at every frame-offset site
  (GLES: allocation, `glBufferSubData`, `glBindBufferRange`; Vulkan: allocation, memcpy, seven
  descriptor/dynamic-offset sites).
- Regression coverage: host harness section [17], 34 new checks (439 total, 0 failures locally):
  helper arithmetic including non-power-of-two alignments, the full 16–4096 driver-alignment
  table, the frame-1 containment invariant, and a pin documenting the exact old overrun. The
  renderer TUs themselves need EGL/Vulkan headers and compile only under the NDK/CI ABI jobs, so
  their verification status is CI-pending, not host-verified.

## 4. Hypotheses ruled out by source trace (do not re-pursue without new evidence)

- Intentional atlas/debug screen: no such code path exists; overlays are skeleton/flat/highlight
  only and none renders a texture to the surface.
- Vertex-attribute or UV swap: attribute locations, byte offsets and the 88-byte stride match
  across `forward.vert`, `af_gl.cpp`, `af_render_common.h`, `afcore.h` and the Vulkan backend.
- GLES embedded-shader location stripping: `af_shaders_gl.h` retains every
  `layout(location=N) in` qualifier; sources are `#version 310 es`.
- Sampler-unit collision: the five atlas textures use units 0–4 (`AF_TEX_COUNT = 5`); post
  processes use 6/7.
- Composite wrong-texture binding: `drawComposite` binds `hdrTexture_` on unit 6; bloom binds
  unit 6 only while its own FBOs are active.
- FBO/texture name aliasing: `makeFramebufferWithTexture` generates fresh names per target with
  correct attachments; GL never recycles a live name.
- Tier-change target rebuild as trigger (atlas present at both tiers, see §2.5).
- VBO/skinning corruption: `ensureGeometryBuffers` is a straight copy of `af_engine_vertices`
  at the shared 88-byte stride — the same buffer the Vulkan path renders correctly.
- Cross-avatar pixel matching (NCC ≈ 0.056): different source art, documented as inconclusive.

## 5. Remaining hypotheses for the visual defect (ranked)

- **H1 — invalid camera binding on odd frames.** The UBO overrun (§3) is the only concrete
  code-level defect on the GLES path; ANGLE may enforce buffer bounds more strictly than the
  older driver and leave the forward program in an error/unbound state. The 2026-10-25 fix
  addresses it, but no evidence yet proves it produces the atlas image.
- **H2 — MSAA resolve / HDR target behaviour under ANGLE.** If the multisampled renderbuffer
  resolve blit or the float/HDR colour attachment misbehaves, the composite fullscreen triangle
  could sample storage that ends up showing texture contents rather than the rendered scene.
- **H3 — ANGLE-specific program/descriptor state issue** outside the paths traced so far.

The current build exposes no GL error string or debug-logger callback, so H1–H3 cannot be
discriminated from source alone. A future diagnostic build with `KHR_debug` output (or
`glGetError` checks surfaced into the renderer info string) is the most direct next instrument;
that work should follow the renderer C-API gating in AF-019 rather than bypass it.

## 6. Deterministic device ladder for the next on-device run

No new build is required; every control below exists in the shipped viewer settings panel
(`ViewerScreen.kt`) and changes a known pipeline stage. Screenshot each state on an atlas-build
and record whether the on-screen image changes.

| Step | Control (shipped) | Pipeline stage touched | Discrimination |
|---|---|---|---|
| 1 | Exposure slider 0.30 → 2.50 | composite tonemap only (`uSourceTex`, fullscreen triangle) | Atlas brightens/darkens → composite fragment runs and its source already contains the atlas (points at H2/H3, forward output lost upstream). No response → the atlas bypasses the composite path entirely. |
| 2 | Ground plane off → on | forward pass geometry + material only | No contact plane appears/disappears → forward-pass output is not reaching the surface. |
| 3 | Shadows off → on | forward pass + shadow FBO only | No change → further evidence the displayed pixels are not forward-pass output. |
| 4 | Sun azimuth/elevation/intensity sliders | forward lighting only | Raw texture ignores lighting; lit geometry would respond. No response confirms unlit texture content on screen. |
| 5 | Bloom off → on (default off) | inserts bloom passes between forward and composite | If the atlas persists unchanged, the post chain is not its source. |
| 6 | Sub-surface scattering toggle | forward material shader only | No response → material shading absent from the displayed image. |
| 7 | Prefer Vulkan toggle, then "Retry renderer" | full backend re-create | Confirms whether the atlas survives a fresh GLES context (expected: yes, given four restarts already observed) and captures any new start-up error string. |

After the AF-020 UBO fix ships in a fresh APK, the primary check is simply: does the same
generation flow show the 3D body (with the diagnostics ladder repeated if it does not). AF-010
rippling and the AF-014 head cap can only be re-scored once a body is visible again, or on a
Vulkan-active session.

## 7. Tool limitations recorded honestly

- No JDK, Android SDK or NDK in the analysis sandbox: Kotlin/NDK verification happens only in
  CI; the host harness cannot compile `af_gl.cpp`/`af_vk.cpp` (EGL/Vulkan headers absent).
- No device GL error log exists in current builds; hypotheses H1–H3 remain source-level.
- GitHub Actions log/artifact downloads and `workflow_dispatch` are unavailable from the sandbox,
  so CI results are taken from the run IDs recorded by the push that created them.
- The two earlier 2026-10-25 chat reports were not stored verbatim; this pack is the consolidated
  replacement and contains every measurement they cited.
