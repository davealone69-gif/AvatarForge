# Avatar realism acceptance target

This document turns the supplied gothic full-body character reference into concrete acceptance criteria for AvatarForge. It is a target specification, not a claim that the current procedural generator can already reproduce the reference.

## Target appearance

- Full-body adult human with coherent head-to-body proportions and no cropped scalp, face, hands, feet, or clothing.
- Recognisable face: skull/jaw/cheek/nose/ears, eye sockets and eyelids, separate eyeballs with irises/pupils, lips and mouth cavity, teeth where appropriate. Facial parts must be geometry or correctly placed textured geometry, not painted blobs.
- Smooth continuous skin surface with stable vertex normals. No ribbed/striped shading, checker patterns, inverted faces, self-intersections, or visible seams at neck, shoulders, wrists, hips, and ankles.
- Anatomically plausible silhouette, hands with distinct fingers, feet fitted inside footwear, and clothing that follows but does not z-fight with the body.
- Layered clothing: lace/embroidery as a material/opacity pattern or real mesh detail, fitted bodice, sleeves, stockings, high-heeled boots, and a separate cape. Materials must remain distinguishable under neutral lighting.
- Long dark burgundy hair as a coherent hairstyle with strands/clumps, not a helmet; black raven as a separate articulated prop when requested.
- Cinematic candle-warm key light and cool blue rim light are optional presentation presets, not baked into skin albedo.

## Required technical gates

1. **Geometry validation:** finite positions/normals/UVs; valid triangle indices; non-zero triangle area; no NaN/Inf; consistent winding; bounds cover the complete avatar; detect degenerate triangles and disconnected islands.
2. **Normals:** recalculate area-weighted normals after isosurface extraction, smoothing, remeshing, clothing generation, and import transforms. Do not average across hard material boundaries. Tangents must agree with UV orientation.
3. **UV/material validation:** UVs stay within intended atlas regions; atlas padding prevents bleed; no default magenta/unbaked texture can be exported as a success. Distinct skin, hair, lace, fabric, leather, eye, and metal material presets.
4. **Face validation:** required facial landmarks and feature meshes exist before export; missing facial anatomy must be a visible generation error, not reported as a finished realistic avatar.
5. **Framing:** compute bounds from the final transformed model, include head and feet with margin, and derive camera near/far planes from bounds. Reframe after model replacement or orientation changes.
6. **Render validation:** screenshot golden tests at front/side/three-quarter views with neutral studio lighting and a separate cinematic preset. Test Vulkan and GLES fallback on the target Android GPU.
7. **Performance:** preserve interactive frame pacing on the Samsung S24 while keeping geometry/material correctness. FPS alone is not an acceptance test.
8. **Truthful completion:** report geometry/face/material checks individually. A successful GLB write is not proof of a photorealistic or correctly rigged avatar.

## Priority order

P0: repair invalid/striped mesh normals, clipping, and face geometry; prevent falsely successful exports.
P1: physically plausible skin/fabric/leather PBR, UV atlas validation, clothing/body intersections, camera framing.
P2: high-quality hair, lace and embroidery, accessories, pose/rig polish, cinematic presets.
P3: Android GPU profiling, adaptive LOD/texture compression, thermal/memory governor, regression suite.

## Current status

This is an acceptance contract. It does not mark the current screenshot defects fixed. Build, exported-GLB inspection, and installed-device checks are required before marking any visual gate PASS.
