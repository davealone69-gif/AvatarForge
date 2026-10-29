# Project status

Updated: 2026-10-30. Branch: `arena/aaf85469-avatarforge`. PR #20 remains draft. `main` is untouched at `40ef65b`.

## Active checkpoint — field-evidence triage and renderer repair (2026-10-30)

Full report: `docs/audit/AUDIT_2026-10-30_FIELD_EVIDENCE.md`; new findings AF-034 … AF-040 in
`docs/audit/FINDINGS.md`; art-direction criteria in `docs/audit/ART_DIRECTION_TARGETS.md`.

This pass started from the user's own device captures ("This is current shit!!") and their
reference set ("Use these as guides"), and it reproduced the defect instead of reading the code and
guessing. A reference rasteriser (`/home/user/fault-review/probe/preview.cpp`) draws the engine's
own vertex buffer with its own baked atlas, so the model can be looked at without a phone; that is
what turned "the screenshots look wrong" into a before/after pair.

Repaired in this pass:

- **AF-034 (Critical) — the atlas overflow that corrupted every baked texture.** Rotated shelf
  placements scaled rotated local coordinates by the un-rotated chart extents, so tall thin charts
  (legs, arms, fingers, cuffs) wrote UVs outside their own cell and `clampf()` folded them onto the
  atlas border. Measured on the shipped kernel: **10 793 of 266 212 vertices pinned to u = 1**,
  9 996 of them below 100 cm. After the fix: **0 pinned, 30 zero-area triangles** (from 17 271).
- **AF-035 (High) — frame cost.** `Rig::skin()` built a 4×4 skinning matrix per vertex per
  influence (~1.06 M per frame where 75 are needed), and `applyCurrentPose()` ran a full triangle
  normal pass plus two 23 MB vertex-buffer streams. Measured **10.14 ms → 4.65 ms per frame**.
- **AF-036 (High) — a test that could not fail.** The UV assertion ran after the clamp, so a
  packing overflow was invisible. The clamp is gone and the test now checks texel-density
  percentiles and zero-texel footprint.
- **Harness crash** — `testTexturing` segfaulted instead of reporting when the unwrap failed.

Recorded and deliberately **not** shipped, with numbers: AF-037 (chart merging — 5× density loss)
and AF-038 (6-texel chart floor — needs 136 % of a 1024 atlas at this chart count). The remaining
texture softness is chart fragmentation (14 460 charts for 367 698 faces), and the fix belongs in
the charter and packer.

Newly found, open: **AF-040 — proportions.** Shoulder : hip measures **0.65** here against
**1.05-1.10** in both supplied references; shoulder : waist 0.64 against 0.74-0.76. Acceptance
criteria are written; not changed in this pass.

### Verified now (local, 2026-10-30)

- Exact-head CI: run https://github.com/davealone69-gif/AvatarForge/actions/runs/40919320455 **succeeded** on
  `af8d14e` — shader compile, model + kernel + renderer host tests, ARM64 NDK build, SDK JVM tests, debug
  APK, explicitly UNSIGNED release AAB, SDK AAR, GLES artifact guard. Artifact `12918316597`
  (17,672,820 bytes): `AvatarForge-Android-af8d14ee…`. The signed-release job was skipped (no credentials).
  The earlier run 40915039614 also passed, on `eb1bb60`.
- Kernel host harness: **411 checks, 0 failures** (was 406; five are new AF-034 assertions).
- Renderer host fault injection: **64 checks, 0 failures**. Model detail: **33 checks, 0 failures**.
- `git diff --check`: clean. `tools/embed_shaders.py --check`: exit 1 locally (`requires glslc`);
  CI compiles the shaders.
- Reference render before/after: `/home/user/fault-review/probe/before.png`, `after.png`.

### Not verified

- **No device ran this build.** The user's four recovery reports and the shipped APK both print
  `Kernel 0.10.1 (ABI 1)`, so the captures cannot be attributed to a commit (AF-039). Nothing in
  this pass is a device-confirmed rendering claim.
- Thermal: every report is `severe` and MEDIUM is capped to LOW. Whether that is honest sensor
  data or a sticky listener is unresolved.
- Signed release: BLOCKED (no keystore). Rust crates: uncompiled, UNVERIFIED.

## Previous checkpoint — zero-trust audit and repair pass (2026-10-29)

Full report: `docs/audit/AUDIT_2026-10-29_ZERO_TRUST.md`; findings AF-020 … AF-033.

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
