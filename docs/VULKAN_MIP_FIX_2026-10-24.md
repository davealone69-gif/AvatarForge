# Vulkan mip-chain layout fix for rippling / "flashing" model surfaces

Date: 2026-10-24
Device evidence: Samsung SM-S921B (Galaxy S24, Exynos 2400, AMD Xclipse 940), Android 17,
Vulkan 1.304, driver 104869891. Screenshot on file: `Screenshot_20261024_191611_Grok Faces.jpg`
(user-provided Google Drive link, not stored in the repo).

## Symptom

The avatar's clothing/armor surfaces show regular, wavy vertical ripples that move while the
model animates, described by the tester as the screen "flashing". The same report measured
**58 FPS, 17.3 ms frame, "on target", quality tier 2**, so this is a rendering-correctness
artifact, not a frame-rate problem. The head also showed a white cap-like surface; that needs
re-checking after this fix because corrupted mip sampling can present as washed-out/white areas.

## Root cause

`VulkanRenderer::generateMips()` builds the atlas mip chain with `vkCmdBlitImage`, but the image
barriers around each blit never performed the required layout transitions:

- every barrier was `TRANSFER_DST_OPTIMAL -> TRANSFER_DST_OPTIMAL` (same layout), so the blit
  source mip was never put into `TRANSFER_SRC_OPTIMAL` even though `vkCmdBlitImage` declared it;
- destination mips were never transitioned out of `UNDEFINED`;
- the final barrier claimed every level was in `TRANSFER_DST_OPTIMAL`, but levels used as blit
  sources were not.

This is a Vulkan spec violation. Permissive drivers (common Mali/Adreno behaviour) map the two
transfer layouts onto the same hardware state and happen to work; stricter drivers, including the
AMD-based Xclipse stack, may return undefined blit data. The corrupted lower mips are then mixed
in by the trilinear atlas sampler (`MIPMAP_MODE_LINEAR`, up to 12 levels, 4x anisotropy), which
produces exactly the observed rippling/shimmering that shifts with camera distance and animation
- the "flash".

The OpenGL ES path is unaffected: it uses the driver-managed `glGenerateMipmap` and has no manual
layout handling (`native/src/render/af_gl.cpp`).

## Fix

`native/src/render/af_vk.cpp`, `generateMips()`:

1. before each blit, two barriers in one batch: source level `TRANSFER_DST -> TRANSFER_SRC` and
   destination level `UNDEFINED -> TRANSFER_DST`;
2. the blit now runs with layouts that match the actual image state;
3. after the chain, levels `0..N-2` (blit sources) and the final level (destination only) are
   transitioned to `SHADER_READ_ONLY_OPTIMAL` with their correct old layouts;
4. the previously-missing single-level path now also transitions `TRANSFER_DST ->
   SHADER_READ_ONLY`, so an image without a blit chain is never sampled in a transfer layout.

## Diagnostics additions in the same change

- The selected depth format is appended to the existing `RendererInfo.driver` string
  (`… · depth D32_SFLOAT` / `D24_UNORM_S8` / `D16_UNORM`) with no C ABI struct change. If the
  device falls back to `D16_UNORM`, coplanar armor/body surfaces are more prone to Z-fighting
  shimmer, and the report will now say so.
- Fixed the Kotlin `RenderSurfaceView` counter bug that made every report show
  `Surface lifecycle: created 0 · changed 0 · destroyed 0` and `Frame loop restarts: 0`: inside
  the member extension `RendererDiagnostics.withCounters()`, the unqualified names resolved to
  the extension receiver's default-zero properties instead of the view's volatile fields. All
  six fields are now explicitly read from `this@RenderSurfaceView`.
- The frozen frame count is relabelled "frame(s) at last state change" so it no longer implies
  only one frame was ever drawn.

## Rebuild requirement

The mip-chain and depth-format changes are C++ and only reach devices after
`tools/build_android.sh` regenerates `prebuilt/arm64-v8a/libavatarforge.so`. The Kotlin counter
and wording fixes ship in the APK regardless.

## Device retest checklist

1. Install a build whose prebuilt `.so` includes this change (check the Backend row now ends with
   a `depth …` suffix - builds without it predate the fix).
2. Open the same "female 1.75 m armor plate sabaton" avatar and compare against the screenshot.
3. Expected: no rippling/shimmer on the clothing at any zoom level; orbit slowly so several mip
   levels are exercised.
4. Open the viewer `diag` tab and confirm the Surface lifecycle row now shows real counts
   (created 1, changed 1+ after layout, destroyed 0 while the viewer stays open).
5. Note the depth format in the Backend row. If it reads `D16_UNORM` and any Z-fighting remains
   on armor edges, that is a separate, now-visible issue to address next.
6. Re-check the head: if the white cap persists with correct mips, it is a content/material bake
   issue rather than a sampler issue and should be reported separately.
