# Adult model, render and video integration

Status: **PARTIAL / NOT YET IMPLEMENTED** (2026-11-15)

This is an implementation handoff, not a claim that adult anatomy or adult video rendering already works.

## Product decision

Keep one 3D-Mega-Hub product experience while core quality is being achieved. Keep AvatarForge as an independently buildable engine/SDK and test lab. Keep the adult visual capability as an optional subsystem with a clean interface, not a separate app. Adult chat is explicitly out of scope for this phase.

The Grok-Girls source was authored for this project by the user with Grok. The user has confirmed it is authorised for reuse. Do not block the port on an assumed third-party licence issue. Preserve the source repository and its history.

## What to port from Grok-Girls now

Port/adapt only model, visual-render and video-workflow concepts:
- Adult-only profile state and explicit opt-in gate from `src/services/settingsState.ts` and `src/services/ageGate.ts`.
- Visual selection schema from `src/services/adultOptions.ts`: wardrobe/coverage, pose, body presentation, scene, lighting, camera, styling, accessories and mood. Map these to typed engine/UI enums; do not copy web-only React state wholesale.
- Relevant render controls and negative-prompt/provider capability metadata from `src/services/adultActs.ts`, excluding chat replies, affinity, relationship memory and conversational routing.
- Video export UX/settings from `src/pages/VideoExportPage.tsx`: duration, frame rate, output aspect, resolution/quality, motion profile, progress, cancellation, output asset. Replace its still-image Ken-Burns recorder with actual rendered 3D frames from AvatarForge. Its current local recorder is a 2D canvas recording, not 3D character animation.
- Do not port adult chat, story dialogue, chat memory, companion responses or chat provider routing in this phase.

## Anatomy source and modelling decision

Use a hybrid pipeline:
1. **Base human:** MPFB2/MakeHuman inside Blender as a parametric, riggable starting mesh and body-proportion system. It is not the finished adult anatomy solution; the standard MakeHuman-derived generator described by 3d-gen-TU explicitly has neutral bodies without genital geometry.
2. **Anatomical detail:** project-authored Blender geometry and sculpted/parametric corrective meshes, fitted to the base topology. Build anatomically coherent external anatomy as topology-aware mesh components or integrated body regions, not prompt-only detail or floating primitives. Use explicit body-specific fit data, clean UVs, normals/tangents, material IDs and deformation tests.
3. **Deformation:** bind every component to the same body skeleton, preserve skin weights, add corrective shape keys for pose-dependent deformation and body variation, and test seams/intersections across the full motion range.
4. **Materials:** use physically based skin shading and authored maps; maintain separate, consistent roughness/normal/AO and skin-detail layers. Avoid a separate opaque material patch that looks pasted on.
5. **LOD:** generate deformation-aware LODs that preserve silhouette, material boundaries and weights. Never decimate away deformation data.
6. **Export:** Blender glTF 2.0 exporter to GLB with meshes, armature, skin weights, shape keys and animation clips. Validate the exported GLB before runtime import.

AI 3D mesh generators may be used for concept exploration or rough accessory/blockout geometry, but must not be treated as anatomically accurate or production-ready without retopology, UV, rig, deformation and visual QA. MPFB2 is the preferred reproducible base authoring path; it is not a genital-anatomy asset pack.

## Video/render pipeline

- Render video from actual engine animation frames, not a still image with pan/zoom.
- Pipeline: selected adult avatar + approved pose/action clip + scene/camera/light profile -> deterministic timeline -> GLES frame rendering -> frame capture -> encoder/mux -> MP4/WebM output and metadata.
- Keep render resolution/FPS/duration bounded by device memory and thermal budget. Provide cancellation, progress and failure states; no fake success or silent fallback.
- Add animation clips and pose presets only after the imported/generated skeleton and animation path is verified.
- The current Grok-Girls local recorder makes a 5-second WebM from a 2D canvas and applies Ken Burns motion. It is a preview effect, not 3D character video rendering.

## Safety and data invariants

- Adult mode disabled by default and requires explicit 18+ confirmation.
- Reject minors and ambiguous-age prompts for any unclothed/adult-only generation.
- Restrict this capability to fictional adult characters; enforce consent/ownership checks on reference material.
- Keep adult assets private by default. Do not send them to a cloud provider without a clearly disclosed, explicitly selected provider and verified capability/policy.
- Adult chat remains deferred.
- Do not expose anatomy controls while the corresponding geometry/deformation implementation is missing.

## Implementation gates

1. Port typed adult visual settings and gate; tests prove persistence, defaults, explicit opt-in and age rejection.
2. Produce one neutral base adult body from MPFB2, then author the required anatomy components in Blender with project-owned source files and documented topology/rig/material setup.
3. Export GLB and run glTF validation; verify skeleton, skinning, shape keys and clip metadata.
4. Integrate the model through AvatarForge's asset-to-pixel runtime; no claim of success until visible GLES pixels and deformation are tested.
5. Implement actual 3D frame capture/video encoding and verify a playable clip.
6. Benchmark memory, GPU time, frame time and thermal stability on the Samsung SM-S921B. Physical phone verification remains separate from headless CI.

## Known starting evidence

- AvatarForge already has `config/customization/adult-anatomy.inputs.json`, `requirements/custom-inputs/adult-anatomy.user.json` and `sdk/src/test/java/com/avatarforge/sdk/AdultSafetyTest.kt`. The anatomy config marks detailed geometry controls `BLOCKED_ENGINE_GEOMETRY`; this is a contract, not a mesh.
- Grok-Girls has adult visual options, age gate, prompt/provider logic and a video export page. Its local video output is a 2D canvas recording.
- AI human-generation tools are not a substitute for anatomically correct production topology. MPFB2/MakeHuman is a useful parametric base; custom Blender modelling and deformation QA are still required.
