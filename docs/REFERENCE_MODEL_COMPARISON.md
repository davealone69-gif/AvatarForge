
# Reference model comparison: mylove.glb

Date recorded: 2026-11-10
Purpose: use the user's real GLB as a measurable reference for AvatarForge generation quality.

## Reference file facts

Source: Dropbox /mylove.glb (9,287,272 bytes; Dropbox content hash:
388a94e7495e809577356ba6cf9890a13fe38f8cd4c63ba3228109d19a24b398).

| Property | Value | Interpretation |
|---|---:|---|
| Container | GLB 2.0 | Valid declared file length |
| File size | 9,287,272 bytes | About 9.29 MB |
| Scene / nodes | 1 / 2 | Small scene graph |
| Meshes / materials | 1 / 1 | Single mesh/material asset |
| Vertices | 163,756 | Geometry complexity reference |
| Triangles | 144,073 | Geometry complexity reference |
| Images / textures | 2 / 2 | Two 2048 × 2048 WebP maps |
| Normals / UVs | Present / present | Shading and texture coordinates exist |
| Skeleton / animations | None / 0 | Static model; no skinning or clip benchmark |

The file-level inspection record does not include a trustworthy real-world scale, body height,
chest/waist/hip circumferences, shoulder width, inseam, joint hierarchy or morph targets. Those
values must remain **MISSING**, not guessed.

## Generator comparison records

The current STATUS.md records multiple distinct host fixtures. Keep them separate:

| Generator record | Geometry | Structure |
|---|---:|---|
| Generated live-engine fixture | 285,657 vertices / 382,388 triangles | Generated through the engine pipeline |
| GLB round-trip fixture | 266,797 unique shared-pool vertices | 10 primitives, 71 joints, 52 morph targets, 5 PNG images; about 34 MB |

These records show that the generator can produce substantial mesh/rig/morph data in host tests.
They do **not** show that its visual appearance matches mylove.glb, nor do they establish GPU
rendering or device performance. The reference has fewer vertices/triangles than the generated
fixtures; adding polygons is not justified as the first improvement.

## Capability matrix

| Capability | mylove.glb reference | AvatarForge generator records | Next comparison |
|---|---|---|---|
| Static mesh | Present | Present | Compare silhouette and proportions |
| Normals / UVs | Present | Generated/baked in pipeline | Inspect seams, tangent basis and shading |
| PBR textures | 2 × 2048 WebP | Separate 5-PNG round-trip fixture | Compare albedo, roughness/metalness and texture detail |
| Rig / skinning | Missing | 71-joint fixture | Validate generated rig independently |
| Facial morphs | Missing | 52 ARKit targets in round-trip fixture | Validate expression shape and neutral face |
| Animation | Missing | Clip library in generator | Not comparable using this static reference |
| Anthropometry | Not present in inspected metadata | af_spec supports tape measurements | Extract reference scale and cross-sections first |

## What can safely be fed to the generator now?

Only the fact that this is a static GLB with the recorded geometry/texture complexity can be used as
a **benchmark profile**. It cannot be converted directly into an af_spec because that structure
expects semantic/anthropometric values such as sex/build, stature, muscle/body-fat controls and tape
measurements. Vertex count, triangle count, file size and texture dimensions are not substitutes.

The correct next pipeline is:

1. Parse POSITION accessor bounds and node transforms from the actual GLB; retain source units and
   record whether scale is known or merely assumed.
2. Establish metric height only if the source scale is trustworthy. Otherwise keep stature unknown.
3. Sample horizontal/vertical cross-sections and extract measurable landmarks where the mesh permits.
   Record each result with confidence and source method; do not invent hidden anatomy from a clothed
   or occluded surface.
4. Map only validated values to af_spec fields. Leave unsupported measurements at zero so the
   generator derives them from its own rules; do not force approximate values.
5. Generate a matched AvatarForge fixture with a recorded seed, af_spec, source commit and output
   GLB hash.
6. Compare both models at matched camera, scale, lighting and exposure. Score silhouette, head/face,
   anatomy, normals, UV seams, texture resolution/detail and material response separately.
7. Keep rig, blendshape and animation validation as separate tests because the reference is static.

## Visual evidence still pending

The user supplied a comparison set in Dropbox /Model on 2026-11-10:

- generated_avatar_diagnostic.png
- Screenshot_20261110_195602.jpg
- Screenshot_20261110_195656.jpg

These files are recorded as available inputs. A pixel-level side-by-side assessment has not been
completed here, so no visual-quality score is claimed.

## Status

- Reference GLB metadata: **PARTIAL** (file-level and previously extracted mesh/texture facts; raw
  accessor bounds and metric scale still need extraction).
- Generator geometry/rig host records: **REAL + VERIFIED in host tests as recorded by STATUS.md**;
  current count reconciliation is pending because STATUS.md contains conflicting harness totals.
- Anthropometric mapping from reference to af_spec: **MISSING**.
- Visual similarity assessment: **PENDING**.
- Physical-phone display: user reports the Hub displays this GLB correctly; AvatarForge's own
  renderer/device path is a separate verification item.
