# Project status

## Current branch — 2026-10-30 (`arena/c87f5c50-avatarforge`)

The user reversed the GLES-only direction and requested **a separate native Vulkan renderer**,
with blur occurring during **live animation clips**. GLES remains the default and a safe fallback;
Vulkan is an explicit viewer toggle requesting **Vulkan 1.3 core** on a 1.3-capable physical device,
*not* the OS's ANGLE-on-Vulkan GLES implementation. `.304` is a reported driver patch, not an
application-requested feature. UV atlas packing (AF-034) and animation skinning (AF-035) fixes
were ported from the newer upstream branch.
Host verification: kernel **411/0**, renderer GLES/selector/version gate **87/0**, linkage-guard tests
**3/0**, SPIR-V provenance regressions **7/0**, Vulkan source host C++ syntax check passed.
Check the Android APK workflow for the **current commit's** shader/NDK/Kotlin/artifact outcome;
its status cannot be inferred from a prior commit. Samsung GPU/pixel evidence is still missing;
neither backend is claimed visually correct on the phone. See [restoration report](docs/VULKAN_RESTORE_2026-10-30.md)
and [device checklist](DEVICE.md). The older branch snapshot below is historical.

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
- Diagnostic scene ladder and GPU timestamps (AF-016): **MISSING**.
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
