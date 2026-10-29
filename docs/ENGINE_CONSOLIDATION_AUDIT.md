# Repository-wide 3D engine consolidation audit

> **Historical snapshot (2026-10-18).** It describes the repository as it was before the GLES-only
> conversion. Statements about a Vulkan renderer or a Vulkan/GLES comparison are superseded: Vulkan
> was removed from the active project, and the supported backend is OpenGL ES 3.1.

Date: 2026-10-18
Target application: AvatarForge (Android-first)
Owner scope: davealone69-gif GitHub repositories visible to the connected account

## Goal

Produce one installable Android 3D creation app by preserving the strongest verified engine, rendering, avatar, asset, animation, export, resource-management, and diagnostics work across the user's repositories. This is a selective consolidation, not a blind merge. Donor projects remain intact. No repositories or branches are deleted or rewritten as part of this audit.

## Audit coverage and honesty

- Repository inventory: 95 repositories returned by the connected GitHub account listing.
- Branch inventory: branch names queried for the repositories in the account listing; this report records the relevant 3D project's branch landscape below.
- Default-branch tree scan: recursive file trees were inspected for the repository set. The API did not resolve every default-branch tree under the literal `main` name (for example, some repositories use `master` or were inaccessible to the tree endpoint). These are not counted as source-reviewed.
- Source-level inspection: targeted rendering, GLB loading, photo mesh, generation, resource-governor, and engine files were read in AvatarForge, Aaa, 3d4me, 3DDD, Grok-Girls, Morph-Lab, Aura-Studio-Avatar, Aura-Avatar, AuraAvatarStudio, Hunyuan3D-2, 3d-models, DD3-Intro, Forge, Builder, New-app, and GitHub-Boss.
- Important limitation: this is a broad repository/branch inventory plus targeted code review, not a claim that every line on every branch has been manually audited. The actual integration still requires per-donor tests, licensing checks, build checks, and physical-phone verification.

## Canonical app decision

**Keep AvatarForge as the canonical Android engine app for the first consolidation pass.**

Reasons observed in its repository:
- A real Android Compose app, Kotlin SDK, JNI/C ABI, native C++ generation kernel, Rust interface, native GLB writer/reader and validator tooling already exist.
- Vulkan and OpenGL ES renderer implementations are present behind a shared renderer contract.
- It has native host regression harnesses and existing Android build workflows/artifacts.
- It has explicit status documentation distinguishing host tests from device verification.
- The current app already owns the creation flow and can become the single front door for imported/generated assets.

Do not call AvatarForge photorealistic or device-verified merely because it builds or reports a high triangle count. Its own STATUS.md says GPU/rendering/runtime/device checks remain incomplete.

## Best donor candidates

| Donor repository | Strongest observed reusable work | Decision |
|---|---|---|
| `AvatarForge` | Native generation kernel; shared C ABI; Vulkan + GLES renderer; Kotlin SDK; GLB import/export; native test harness; scene graph, frustum/LOD/profiler code | Canonical base. Preserve and complete its missing renderer-to-scene wiring before claiming scene/LOD support. |
| `3DDD` | Filament viewport lifecycle; mature glTF document parsing; node graph; skinning; animation sampling; morph target update path; PBR texture/material handling; orbit camera; photo mesh builder; GLB morph export tests | Primary reference/donor for import correctness, material semantics, camera fitting, animation/morph edge cases, and photo-mesh test cases. Do not wholesale replace AvatarForge's custom renderer without a measured bake-off. |
| `Grok-Girls` | Separate native Android and web renderer implementations; GLB loader; texture loading; skeleton matrices; glTF animation; PBR shaders; hair/eye/skin materials; renderer diagnostics; Hunyuan/TripoSR/photo-generation adapters; extensive validation branches | Donor for targeted renderer regression cases and optional generation-provider adapters. Its branches are numerous and must be compared by commits before any cherry-pick. Do not assume the web IBL path is production-grade: its source comments identify the current IBL/BRDF LUT as a bootstrap approximation. |
| `Aaa` | SDK boundaries; GLB validator; generation/provider contracts; model manager; persistent avatar library; resource governor; truthful unavailable states; asynchronous video-worker validation work | Donor for resource budgets, persistence, validation contracts and job-state semantics. Video worker is a separate optional subsystem, not a dependency of the local 3D engine. |
| `3d-models` | Repeated Android GLB loader, PBR shaders, texture manager, skeleton matrices, animation and local model setup | Secondary donor/reference only. Overlaps Aura/Grok work; compare against Grok-Girls before copying to avoid duplicate render stacks. |
| `Aura-Studio-Avatar` | Avatar library/designer, local model management, generation/provider abstractions, persistence and diagnostics UI | Donor for product workflows and model-management UX after comparing against Aaa. Not the canonical renderer. |
| `Aura-studio`, `https-github.com-davealone69-gif-Aura-Studio-Avatar`, `Aura-Avatar`, `AuraAvatarStudio` | Smaller/duplicate Aura variants and UI/renderer experiments | Treat as historical donors until unique working code is identified. Avoid merging duplicate projects wholesale. |
| `Hunyuan3D-2` | Upstream image/text-to-3D model pipeline and texture-generation source | Optional external generation backend. Heavy GPU model inference is not a realistic native-phone dependency for the current Samsung; require an explicitly configured capable worker or report it unavailable. |
| `3d4me` | Web avatar/model workshop and avatar state/design code | UX/reference donor; not a renderer replacement. Current app startup/runtime problems remain separate and must not be hidden by copying its web UI. |
| `Morph-Lab` | Mobile-friendly procedural game engine, renderer, camera, input, physics, profile and asset systems | Keep as a separate project. Reuse only isolated utilities that solve a demonstrated engine need. Do not merge game loop/UI into the avatar studio. |
| `morph-lab-game`, `morph-lab-game-pages` | Related web/game builds | Keep separate; compare unique assets/features with Morph-Lab before any consolidation. |
| `Builder`, `GitHub-Boss`, `New-app` | Bundled 3DoubleD/Filament renderers and model studio workflows | Additional 3DDD donor snapshots. Prefer the canonical `3DDD` source and its tests over copies nested in builder apps. |
| `DD3-Intro` | Rendered intro/video assets | Asset-only candidate, not an engine. Validate licensing and formats before use. |
| `Forge`, `new` | 3D library selection notes and broader app/tooling assets | Research references only until a concrete dependency and license are verified. |
| Other repositories in the 95-repo inventory | General Android, AI, builders, agents, workshop tools and utilities; most default trees showed no relevant 3D engine/render/GLB source by filename scan | Do not pull unrelated code into the engine. Revisit only if later content search exposes a specific reusable implementation. |

## Technical findings that affect the integration

1. **Rendering architecture:** AvatarForge already has shared Vulkan/GLES shader source and renderer implementations. Keep a single renderer contract. Compare output quality and device behavior against 3DDD's Filament renderer before deciding whether Filament becomes a supported backend or remains a reference. Do not silently maintain three competing renderers.
2. **Scene system:** AvatarForge contains a CPU scene graph, frustum culling, light culling and LOD selection. Its status documentation says the new scene layer is not yet consumed by the renderer. This is a concrete integration gap and must be wired and tested before those features are advertised as active.
3. **PBR:** AvatarForge's shared shader already documents skin/eye/hair/cloth/metal shading, SH irradiance, shadows and tone mapping. Grok-Girls has useful skin/hair/eye shader and regression references, but its web IBL comments explicitly call out placeholder irradiance/prefilter and a constant BRDF LUT. Do not transplant that stub as a quality upgrade.
4. **GLB:** Use 3DDD's glTF parser/loader tests and Aaa's GLB validator as compatibility references. Preserve AvatarForge's strict import behavior: unsupported extensions must be reported, not silently misinterpreted; rejected imports must not corrupt live state. Add tests for GLB chunks, accessors, sparse accessors, images, PBR maps, node transforms, skinning, animation and morph targets.
5. **Avatar quality:** Procedural geometry and image-color analysis do not equal neural human reconstruction. Hunyuan3D/TripoSR must be represented as separate, capability-checked generation providers. If the model weights/backend are missing, the app must report that honestly instead of producing a featureless procedural body and calling it photorealistic.
6. **Phone resources:** Adapt Aaa's measured RAM/storage/thermal governor into AvatarForge's job/quality decisions. Do not use `largeHeap` as a substitute for controlling allocations. Limit simultaneous heavy jobs and degrade resolution/quality based on measured resource state.
7. **Export:** Reuse the strongest validation patterns from Aaa/3DDD/Grok: parse the exported GLB back, validate its structure, verify nonempty mesh/material/texture data, validate skin and morph targets, and expose success only after the destination copy succeeds.
8. **App product:** AvatarForge remains the Android front door. Import existing GLB, create from prompt/reference, inspect geometry/materials/rig, pose/animate, save projects, and export validated GLB must be real routes with errors surfaced. Keep cloud/worker generation optional; the core viewer and local procedural tools must work offline.

## Consolidation order

### Phase A: Freeze and prove the base
- Keep original repos and branches untouched.
- Capture main/default branch SHAs and branch names for every donor used.
- Run AvatarForge native host tests, Rust tests, Kotlin/JVM tests, lint and Android builds.
- Capture the current phone screenshot and exact model/export diagnostics as the baseline.

### Phase B: Renderer and GLB correctness
- Wire AvatarForge's scene/visibility layer into actual backend draw submission, or explicitly mark the unused pieces experimental until that is done.
- Fix the blown-out viewer defaults and add repeatable exposure/white-balance test scenes.
- Compare AvatarForge Vulkan vs GLES and the 3DDD Filament reference with identical GLB fixtures on target hardware.
- Port only test-proven fixes for GLB parsing, texture color spaces, tangents/normals, skinning, animation and morph targets.
- Add automated round-trip GLB fixtures and image/render regression captures.

### Phase C: Real character generation
- Separate procedural base-mesh generation from image-to-3D and text-to-3D model providers.
- Improve anatomy/face geometry only with measured mesh/landmark checks and reference renders.
- Require a provider capability/status report: model present, backend reachable, job completed, output downloaded, GLB validated.
- Never label a procedural silhouette as photo reconstruction.

### Phase D: Product and phone reliability
- Add the Aaa-style resource governor and persistent library where compatible.
- Exercise camera/orbit/pose/face/material/inspect/export controls.
- Verify lifecycle, orientation changes, low memory, thermal throttling, GPU fallback and malformed GLB failures on a physical Samsung.
- Produce AAB primary and APK secondary; store signing remains separate from debug installability.

## Merge policy

- No destructive repo/branch changes.
- No blind copy of whole projects or duplicated renderer stacks.
- For each donor change, record source repo + branch + commit SHA + license + tests + destination commit.
- Preserve attribution and license notices; if a component's license is not verified, do not copy its code.
- No `REAL + VERIFIED` label until the Android build succeeds and the installed app is exercised on the phone.
- If the build or phone is unavailable, use `UNVERIFIED`, `PARTIAL` or `BLOCKED`.

## Current status

- Repository inventory: **completed for 95 listed repositories**.
- Branch-name inventory: **queried across the listed repositories**.
- Default-branch tree scan: **broad scan completed; some non-main/inaccessible trees need follow-up**.
- Targeted source inspection: **completed for the donor files listed above**.
- Actual donor code merged into AvatarForge by this audit: **none yet**.
- Android build from this audit branch: **not run**.
- Installed-phone verification: **not run**.
- Overall consolidation: **PARTIAL / UNVERIFIED**, not finished.
