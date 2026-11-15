# Cross-project reuse audit — 2026-11-15

## Scope and evidence

This is a read-only source/branch audit for the adult nude-body, GLB/rig, renderer and real 3D-video requirements. It extends the account inventory and 17-candidate source review recorded in the 2026-11-08 repository discovery report. This pass checked the active integration branches and relevant donor branches, plus all five private repositories and their branch lists.

No donor repository was modified. No branch was created, merged or force-pushed. No APK was built by this audit and no physical-device test was performed. Source presence, headless tests and CI configuration are not proof of visible runtime output.

## 1. What nude rendering exists now?

### AvatarForge — PARTIAL, real base surface in source

On `work/android-engine-dev`:

- `Outfit.NONE` maps to native `AF_OUTFIT_NONE` and is the engine's actual unclothed adult base surface, not a prompt-only wardrobe label.
- The standalone AvatarForge Android input screen exposes the option and an explicit adult-confirmation checkbox. `AvatarSpec.requireGeneratable()` rechecks confirmation and age 18+ before generation; prompt checks also reject minor/teen markers.
- This is a usable first-stage unclothed body surface for renderer, skin-material and pose work.
- It is **not** a finished detailed anatomical model. Genital, nipple, areola and pubic-hair geometry/material/deformation paths remain blocked and their controls stay hidden.

**Runtime status: UNVERIFIED.** The source path exists; this audit did not install or render it on the Samsung phone.

### 3D-Mega-Hub — MISSING from the current UI

On canonical PR #9 branch `integration/avatarforge-renderer-port`, `AvatarStudioScreen.kt` currently generates with `Outfit.JEANS_JACKET`. It does not expose `Outfit.NONE` or the adult-confirmation control. Imported file-based GLBs use the existing Filament gltfio preview and its animation controls; the native AvatarForge GLES preview remains a separate path and does not yet skin imported GLBs.

Do not add a nude-mode button to the Hub until the current visible-pixel/runtime gate is fixed.

## 2. Best reusable project components

### 3DDD — best video-export donor, but not ready to port unchanged

Branch: `fix/mp4-export-finalize-validate-gallery-playback`, head `f70e9f43d106c073aa42e1b482d6f1ec28445d78`.

Useful source:
- Filament scene rendering and offscreen capture through a Filament render target.
- H.264 `MediaCodec` surface input and `MediaMuxer` MP4 finalization/validation.
- Export progress, cleanup and gallery-copy validation.
- `RuntimePipelineTest` covers model load, animation metadata, capture and media playback.

Important defects/limits found:
- `ExportManager.exportMp4()` changes `orbit.azimuth`, but does not call `OrbitCamera.apply()` before `renderEncoderFrame()`. The camera setter only changes a value; `apply()` performs the actual camera update.
- The MP4 loop does not advance the character's `Animator` per frame. It is not yet a verified animated-character video path.
- The test checks that the MP4 finalizes and plays, but does not assert that rendered frames differ or that the model animates.
- CI fails at `:app:lintVitalRelease`: backup rules include and exclude `exports/` and `audio/` simultaneously. The instrumentation job was skipped, so the runtime export test has not been demonstrated by that workflow.

**Decision:** reuse/adapt the encoder, muxer and validation pieces only. Integrate them into the Hub's existing `FilamentPreviewView` and its existing Filament Engine/Animator. Fix camera application, sample animation at each output timestamp, and add frame-difference plus decode/playback assertions. Do not transplant 3DDD's separate renderer/loader wholesale.

The bundled 3DDD `female.glb` and `male.glb` are small static body fixtures with seven morph targets each, but no skin, skeleton, animation or embedded images. They are not production anatomy assets. Its small rigged studio avatar is useful as an animation test fixture, not as a human body asset.

### Grok-Girls — strongest authorized GLB implementation reference

Branch: `renderer/native-engine`, head `02363addc8c80fdf7cc28d01a064ec3efc50867a`.

The native Android branch contains a Kotlin/OpenGL ES 3.0 GLB 2.0 loader and renderer with PBR materials, skinning, morph targets, TRS and morph-weight animation, IBL and ACES. Its README records 57 headless assertions and 21 GLSL variants. The headless GLES stub is no-op, so those checks do not prove real GPU rendering.

Its 28 KB generated test avatar is deliberately box-built (four joints, blink/smile morphs, a short idle clip and five PBR textures). It is a useful parser/animation fixture, not anatomy. The `work/full-repo-donor-integration` branch adds `ProceduralAvatarParts.kt` for hair and outfit accessories; those are simple additional meshes, not detailed body anatomy or a production hair-card system.

**Decision:** use this code as a behavioral reference and test-oracle source for GLB skin/morph/animation gaps. Do not add its entire Kotlin renderer as a second renderer beside AvatarForge and Filament.

Grok-Girls' web `VideoExportPage` is a 2D canvas/Ken Burns recorder, not 3D character video. Do not port it as the 3D video engine. The visual settings and age-gate concepts remain reusable separately from adult chat, which stays deferred.

### 3d-models — duplicate renderer lineage, not a separate engine to add

Its native Kotlin GLES renderer and 28 KB rigged fixture substantially duplicate the Grok-Girls native-engine branch. The fixture has four joints, one idle animation, head/eye morph targets and five images, but is a stylized clothed avatar. The repository is a useful reference, not a reason to introduce another renderer.

### 3d4me — parameter/UI reference only

The `shared/avatar3d.ts` implementation is a simple lofted torso plus capsule-like limbs and separate clothing geometry. It always builds clothing, has no skeleton/animation and no detailed nude anatomy. Its relevant Android/UI-fix branches do not change the avatar geometry file.

### Other candidates

- **Aaa** `feat/talking-avatar-video-worker`: EchoMimic/audio-driven video worker. It is an image/video inference service, not a rigged 3D renderer or Android character-video pipeline.
- **Aura-Studio-Avatar**: adult controls are primarily prompt/UI state. Its local video JNI bridge is a stub returning failure/null. Not a 3D donor.
- **morph-lab-game** (private): a fully clothed browser RPG using procedural Three.js geometry and local state. No native renderer or reusable adult rig.
- **DD3-Intro** (private): a pre-rendered 11-second MP4 and Android splash integration. It is not a live 3D renderer.
- **Git2-Clean, GitHubClient and OmniSwarm** (private): no relevant avatar geometry, GLB renderer or 3D-video pipeline surfaced.
- **Hunyuan3D-2**: external GPU-backed generation only. It is not an Android renderer and its Tencent community/model licensing requires separate review; do not transplant or redistribute it as an app component.

## 3. Branch and CI findings

- **AvatarForge canonical working branch:** `work/android-engine-dev`. The `fix/avatar-head-hands-geometry-pass1` branch has one unique commit relative to this line, limited to head/finger geometry adjustments; it does not add adult anatomy. The realism/asset-registry and GLB-measurement branches are divergent references. Do not merge any of them wholesale.
- **Hub canonical review:** PR #9, `integration/avatarforge-renderer-port`, head `13117dc4e8fc690753eb06e86558e71362fd7141`, remains open and unmergeable. Release-preview and emulator install/launch checks passed, but the generated-avatar runtime test failed because no drawable EGL-presented model pixels appeared. The native sampling regression also failed on missing `af::REGION_HAIR`. This is not a green runtime build and is not S24 verification.
- **Private repositories:** all five private repositories were checked; their branch scans returned only `main`. The potentially relevant private projects are `morph-lab-game` and `DD3-Intro`, described above. No hidden private branch supplied a finished nude rig or live 3D-video engine.
- **AvatarForge test-count reconciliation:** its README reports 464 host checks, while the dated STATUS.md checkpoint records 405/405. This audit did not rerun the host suite; reconcile the count on the next exact-head test run.

## 4. Preserved model asset

Dropbox contains `/3d-mega-hub-diagnostics/3d-mega-hub/mylove_rig_prepared.glb` at 13,221,520 bytes. Its filename and size do not prove that skin weights, joints, morphs or animation clips are valid. The Dropbox connector's text-fetch limit is 5 MB, so the binary was not inspected in this pass. The existing README checksum is historical until recomputed from the exact binary. Status: **UNVERIFIED**. Do not ask the user to re-upload it; use the existing Dropbox copy or a verified repository/artifact copy when binary inspection is available.

## 5. Patch decision and order

1. Fix the current PR #9 visible-pixel failure and the `af::REGION_HAIR` native sampling regression. Keep all work on the existing canonical branch.
2. Inspect `mylove_rig_prepared.glb` at the byte level: mesh inventory, skin/joint hierarchy, inverse binds, JOINTS/WEIGHTS, morph targets, animation channels, materials and textures. Do not infer from its filename.
3. Adapt 3DDD's `MediaCodec`/`MediaMuxer` export and validation into the Hub's existing Filament preview. Use the existing Engine and Animator; do not create a second renderer. Add deterministic frame timestamps, camera updates, animation sampling, frame-difference checks and a real decode/playback test.
4. Expose the existing AvatarForge `Outfit.NONE` path in the Hub only after its renderer can visibly render and the adult confirmation gate is wired through the host UI.
5. Build the missing detailed anatomy as project-owned geometry/material/deformation assets, then test skin weights, corrective shapes, UVs, LOD and full-pose deformation. No audited repository currently supplies a production-ready detailed anatomy set.

## Overall status

- **REAL + VERIFIED (source):** AvatarForge's adult-only unclothed base path; Hub's Filament GLB animation-preview code; donor source files and branch references.
- **PARTIAL:** adult base anatomy, GLB importer/animation coverage, 3DDD video-export prototype.
- **MISSING:** detailed genital/nipple/areola/pubic-hair geometry and materials.
- **UNVERIFIED:** prepared Dropbox rig, visible S24 render, animated 3D MP4 output and target-device performance.
- **BLOCKED:** current Hub generated-avatar visible-pixel gate; 3DDD donor release lint until conflicting backup rules are corrected.
