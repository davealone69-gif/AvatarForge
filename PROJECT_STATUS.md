# Project status

## Current branch — 2026-10-30 (`fix/verify-clean-checkout-output-dir`; prior pushed tip `f6929e98d23a7e45c31d7f6bac35f7560e4fcf9a`; recorded CI run on `401a005eac045371c0d3625baf4c523a55288435`)

The owner-approved engineering directive was applied on the existing branch, preserving the three
pre-existing local commits (clean-checkout verifier log, shoulder-width/image-role fixes, and stale
image-analysis guard). The branch started at `187fa25`, based on common commit `1dfcf40`; the arena
remote was at `f82287b6e13c1dc131c5d1b5d4566b30d9e74d62` (two commits past that base). The repair/audit
commit `401a005eac045371c0d3625baf4c523a55288435` was pushed normally to the new
`origin/fix/verify-clean-checkout-output-dir` branch. No merge, rebase, reset, force-push, or direct
push to `arena` was performed; the arena ref remains unchanged.

The renderer defaults to native Vulkan with explicit and bounded OpenGL ES 3.1 fallback. The requested
target label is Vulkan `1.3.304`; the app declares `VK_API_VERSION_1_3` (standard patch-zero encoding)
and requires loader/device API 1.3. Shaders compile/validate for `vulkan1.3`. The installed NDK
26.1.10909125 headers are revision 1.3.237; these and the supplied SM-S921B checker label are not
AvatarForge runtime versions. The post-fix host verifier passed 423 kernel checks, 87 renderer
fault checks plus 7 API-version checks, and 33 model regressions; all 12 GLES and 13 Vulkan shader
modules validate. Android APK workflow run [41002485082](https://github.com/davealone69-gif/AvatarForge/actions/runs/41002485082)
succeeded on this exact commit and staged, verified and uploaded artifact
`AvatarForge-Android-401a005eac045371c0d3625baf4c523a55288435` (ID `12954506148`, 18,048,697 bytes).
The ARM64 native build, 3 app / 38 SDK JVM tests and compile-only artifacts pass. The SM-S921B runtime,
visible pixels, validation-layer output, thermal soak and production signing remain **UNVERIFIED/BLOCKED**. See
[Vulkan target audit](docs/audit/VULKAN_TARGET_AUDIT.md), [device checklist](DEVICE.md), and the
current exact-head test record in `docs/audit/VERIFICATION_RESULTS.md`.

## Follow-up validation — 2026-10-30 (based on prior pushed tip `f6929e98d23a7e45c31d7f6bac35f7560e4fcf9a`)

Repository: `/workspace/AvatarForge`; branch: `fix/verify-clean-checkout-output-dir`. The latest
validation was performed on a working tree based on the prior pushed tip `f6929e98d23a7e45c31d7f6bac35f7560e4fcf9a`.
The public workflow run below is on earlier commit `401a005` and does not verify this follow-up; this
record relies on local test/build evidence, not a CI or device result.

- **AF-038 — REAL + VERIFIED (host metric):** a 2048-only 1.01-texel interior floor leaves the
  1024 behavior unchanged. The final 2048 fixture reports **0/14,412** cells below one interior
  texel, packed-cell coverage **0.82**, and the complete kernel suite passes **438/438**. The first
  full run at the 1.00-texel floor failed on **19/14,412** cells; the 1.01 margin fixed that failure.
  Device texture appearance remains unverified.
- **AF-040 — REAL + VERIFIED (numeric host fixture):** the unclothed 1.72 m fixture measures
  shoulder/waist **1.346**, waist/shoulder **0.743**, and shoulder/hip **1.052**; the focused model
  suite passes **40/40**, with exact 68 cm waist and 96 cm hip inputs preserved within 0.01 cm. The
  1.48 lower bound was removed because it contradicts the inverse reference target; finite upper cap
  1.67 remains. Broader visual, garment and device acceptance is still open.
- **Renderer — PARTIAL:** optional GLES `EXT_disjoint_timer_query` timing is host-tested (**102/102**)
  and the Android NDK compiles the Vulkan and GLES renderer translation units for arm64-v8a and
  x86_64. There is still no selectable visible diagnostic scene ladder; its eight rungs are metadata.
  The host runner skipped its pinned-header Vulkan API-version helper. No device is connected, so
  Vulkan/GLES runtime, GPU timings, pixels, fallback and thermal behavior are unverified.
- **App/build:** forced JVM tests pass (**3 app + 38 SDK**). `:app:assembleDebug` succeeds and
  packages both native ABIs. `/workspace/AvatarForge/app/build/outputs/apk/debug/app-debug.apk` is
  11,771,041 bytes, verified with the debug APK v2 signature, and contains no INTERNET permission;
  this is compile/package evidence only, not an installed or launched app.
- **Other requested risks:** typed JNI handle registry checks are included in the passing 438-check
  host suite and NDK build; stale reference-analysis guards are covered by the JVM suites. Rust ABI
  parity remains **PARTIAL** at 15/47 symbols (32 unported). The release-workflow source contract
  passes locally, and run [41002485082](https://github.com/davealone69-gif/AvatarForge/actions/runs/41002485082)
  succeeded on `401a005`; signed-tag production output remains unverified/blocked without release
  credentials.

## Historical snapshot — prior `arena/aaf85469-avatarforge` branch (2026-10-29)
PR #20 remained draft when recorded below; `main` was then at `40ef65b`. Statements that Vulkan
was removed or linkage was GLES-only describe that earlier checkpoint, **not this branch**.

## Active checkpoint — zero-trust audit and repair pass (2026-10-29)

Full report: `docs/audit/AUDIT_2026-10-29_ZERO_TRUST.md`; new findings AF-020 … AF-033 in
`docs/audit/FINDINGS.md`. The previous checkpoint (Vulkan removal + model details) is below and is
unchanged by this pass.

Repaired in this pass (source-level evidence only where noted):

- **AF-020 (Critical) — ownership fence.** One `KernelAccess` lock now separates mesh mutation
  (expression/pose/clip application, generation, export, prompt parse, image analysis, engine
  destroy) from renderer reads (`frame`, `attachEngine`, `invalidateMesh`, `pickRegion`). Blendshape
  application moved to the render thread; picking uses a bounded wait. Compilation is CI's job;
  absence of a race requires a device soak.
- **AF-021 — face panel lied.** Channels overwrote each other; weights are now host state, the whole
  array is applied per edit, chips show applied values and an active count.
- **AF-022/AF-033 — measurements unreachable.** Reference-image analysis now runs when the image is
  picked (off the main thread), fills the pre-build card, updates the spec, and is reused by the
  build; failure is stated instead of swallowed.
- **AF-025 — unbounded GPU allocation.** `sanitizeOptions()` clamps/validates every renderer option
  (MSAA 1–4, shadow map 512–2048, bloom 0–5, environment ranges, NaN rejection) and the settings codec
  mirrors the bounds. New host regression.
- **AF-024 — Compose/lifecycle.** No state writes during composition, stable remembered fallbacks,
  lifecycle-aware stats collection.
- **AF-026…AF-028, AF-031, AF-023, AF-029, AF-030** — dependency hygiene, R8 rule correction,
  overflow/config-checked image buffers, honest export panel, corrected signing docs, Rust metadata,
  Vulkan-free current docs.

## Previous checkpoint

User direction implemented: **remove Vulkan**, preserve the now-working camera, and fix visible model details. The renderer is **GLES 3.1 only**. Vulkan implementation, SPIR-V table, backend preference/fallback, JNI/Rust fields and the Vulkan link dependency are deleted. Android's own GLES driver may still report an internal translation layer; that is not an AvatarForge backend.

## Verified now (local, this pass)

- Exact-head CI for this pass: run https://github.com/davealone69-gif/AvatarForge/actions/runs/40900908327
  **succeeded** on `d7822b7` in 12m00s — shader compile/diff, model + kernel + renderer host tests,
  ARM64 native build, SDK JVM tests, debug APK, explicitly UNSIGNED release AAB, SDK AAR and the GLES
  artifact guard. Artifact `12910680693` (17,657,509 bytes): `AvatarForge-Android-d7822b7…`.
  The signed-release job was skipped (no credentials).
- Previous checkpoint CI: run 40895347516 succeeded on `39c8cf6` in 7m52s.
- Renderer host fault injection: **64 checks, 0 failures** (was 57; seven new option-sanitisation checks).
- Focused model-detail regressions: **33 checks, 0 failures** (eyes, extracted eye symmetry, chest plate, hair eye-line, palms, garments).
- Full kernel host harness: **406 checks, 0 failures**.
- `git diff --check`: clean. `tools/embed_shaders.py --check`: exit 1 locally with
  `--check requires glslc; no compiler validation is claimed` (expected; CI compiles the shaders).
- Local host export of the current device spec: **412,266 triangles**, validator OK; eye patches **2,359 / 3,148** triangles, chest plate front **−0.139 m**, ponytail top **crown + 0.024 m**.
- Camera: **user-confirmed working**; exact build SHA/counters still missing.

## Model-detail fixes in this checkpoint

- Palms are built in the anatomic hand frame instead of a world-axis box; interdigital blend reduced to avoid mitten fusion.
- Garment minimum layer is two lattice cells, and the lower-garment arm cut no longer removes thigh cloth below the crotch.
- Viewer header is compact by default so it no longer covers the face/hands; full build/driver details remain expandable. Camera gesture code is unchanged.
- Eye materials no longer swap anatomic sides; eyeballs use a hard union, and red-green refinement reclassifies new midpoints. The current device GLB's 1,637-vs-156 eye imbalance is now 2,359-vs-3,148 in the local export.
- The armour chest plate is a torso-profile shell instead of a solid ellipsoid; its front no longer protrudes as a metal ball.
- Ponytail/short hair bulk follows the cranium instead of floating a helmet above the crown, with explicit orbital clearance so hair cannot become a visor. The afro still clears the skull by 8.5 cm.
- Stale Vulkan-era tracked prebuilt/APK/AAR/sample GLB binaries were removed. CI exact-head artifacts are authoritative, and a linker guard rejects Vulkan dependencies.

## CI artifact

Exact-head artifact: `AvatarForge-Android-d7822b751fd79f1defc50264d6a70edb16d08410` (id 12910680693,
17,657,509 bytes): debug APK, explicitly UNSIGNED release AAB, SDK AAR, SHA256SUMS, PROVENANCE.json.
The unsigned AAB is compile proof only, not an installable production release. The older
`39c8cf6` artifact (12905983840) is superseded.

## Still not proven

- Device pixel confirmation of the new eye/hair/chest/hand/garment/header fixes. The current device report still shows the pre-fix GLB and severe thermal state.
- Device thermal cap effect; settings persistence on Android; sustained performance/memory; moiré pixel comparison.
- Production signed AAB/APK: **BLOCKED**, no credentials.
- The ownership fence (AF-020) is implemented and compiled, but its absence of races is
  **UNVERIFIED** until a device soak runs camera drags + clip playback + expression edits + picking.
- AF-016 remains **PARTIAL**: Vulkan timestamp recording and the optional GLES `EXT_disjoint_timer_query`
  ring have host-tested contracts, but device execution is unverified and no selectable diagnostic
  scenes/UI exist. The diagnostic scene ladder remains metadata only.
- `CustomizationSchemas` is loaded but unused (AF-032): integration or deletion is still a decision.
- Rust crates remain **UNVERIFIED** by compilation (no toolchain here); metadata now matches 0.10.1.
- Lash geometry, strand-level hair, facial texture smudges and teeth cost (38,965 tris) remain unaddressed; no superiority claim.

## Next exact steps

1. Install the exact-head CI debug APK on the SM-S921B; capture front/side eye, hair, chest, hand and
   garment views plus a diagnostics export.
2. Soak that build for ≥10 minutes with clip playback, camera orbiting, two simultaneous expression
   channels and repeated tap-picking, then export the recovery report.
3. Supply `AF_RELEASE_*` secrets so the tag-only `signed-release` job can produce a signed AAB, and
   implement the AF-016 diagnostic scene ladder.
