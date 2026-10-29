# Audit pass — 2026-10-30 (field evidence triage and renderer repair)

Branch: `arena/aaf85469-avatarforge`. `main` stays at `40ef65b` and is untouched. This pass was
triggered by two pieces of evidence the user supplied:

- **"This is current shit!!"** — a Google Drive folder of six device screenshots and four
  in-app recovery reports from a Samsung SM-S921B, captured 2026-10-29 23:53 to 2026-10-30 00:02.
- **"Use these as guides"** — a Dropbox folder `avatar/guides/body` with nine body and proportion
  references.

Labels: **REAL + VERIFIED** (reproducible here), **PARTIAL**, **UNVERIFIED**, **MISSING**,
**BLOCKED**.

## 1. What the evidence actually says

The four recovery reports are internally consistent and are the first correctly functioning
diagnostic output this campaign has seen from hardware:

```
Kernel 0.10.1 (ABI 1), Native loaded, samsung SM-S921B, API 37, arm64-v8a, RAM 7.1 GB
Backend gles31 | GPU ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304) | GLES 3.2, driver 25.6.4
Frames 3154/3154/3154 accepted presents | 412266 tris, 13 draw calls | Textures 90.7 MiB
Settings requested GLES + MEDIUM | Effective GLES + LOW | Thermal severe
```

Three things follow, and one does not:

1. **The GLES path runs, presents and is stable.** Every frame attempted was accepted by
   `eglSwapBuffers`, over 1619 to 4851 frames per session, with `Last error: none`. The
   black-screen / zero-FPS class of fault is not what these reports show.
2. **The thermal cap is doing more than it advertises.** Every report is `THERMAL_LIMIT` /
   `severe` and every report is capped from MEDIUM to LOW. That is the policy working as written
   (`RenderThermalPolicy`), but a phone that reports severe at 23:53 in an air-conditioned room
   while the app renders at 28-47 FPS is worth a second look — recorded as an open question, not
   a finding, because the app cannot distinguish "the phone is hot" from "a previous owner of the
   thermal listener left it capped".
3. **`ANGLE ... on Vulkan 1.3.304` is the driver's own translation layer**, not this project's
   renderer. Vulkan was removed from this codebase in `ff7c4d1`; the string is what Samsung ships
   underneath GLES, and it will keep appearing. It is not evidence of a Vulkan code path.
4. **The reports cannot tell us which build produced the screenshots.** Both the shipped 0.10.1
   APK and every commit in this campaign print `Kernel 0.10.1 (ABI 1)`. There is no commit, build
   time or version code in the header. This is now AF-039 and the reason the visual claims below
   are made against a *reproduced* build rather than assumed against the user's.

## 2. Reproducing the defect instead of guessing

The screenshots could not be diagnosed from the code by inspection, so the geometry was measured
directly through the public C ABI with throwaway probes against the shipped kernel (`5a81c11`):

| Probe | Question | Answer |
|---|---|---|
| `geom_probe` | Is the uploaded vertex data sane? | 266 212 verts, 1 103 094 indices, 0 out-of-range, 0 non-finite, 0 UVs outside [0,1], 0 bad weight sums, max edge 13.4 mm |
| `atlas_probe` | What does the baked atlas look like? | Dumped all five maps; the base colour was a striped/mottled mosaic |
| `uv_probe` | Where does the mesh lose texel area? | 17 271 triangles with zero UV area; `teeth` and `skin` destroyed |
| `where_clamped` | Which vertices were folded onto the atlas border? | **10 793 pinned to u = 1**, 9 996 of them below 100 cm — legs, knees, shins, ankles |
| `pose_time` | What does a posed frame cost? | **10.14 ms** of CPU per frame on a desktop core |
| `comp_probe` / `stage_comp` | Is the mesh fractured? | 7 components after the geometry stage, 19 766 after the unwrap — every split is a UV seam, i.e. correct |

The decisive step was building a **reference rasteriser** (`preview.cpp`) that draws the engine's
own vertex buffer with its own baked atlas, so the model can be looked at without a phone. That
turned "the screenshots look wrong" into a before/after pair that can be re-run at any commit.

## 3. Root causes found and repaired

### AF-034 — the atlas overflow (Critical, fixed)

For a rotated shelf placement the unwrapper divided *rotated* local coordinates by the
*un-rotated* chart extents. Tall thin charts — legs, arms, fingers, hair strands, cuffs — therefore
generated UVs outside their own cell, and `clampf()` folded those vertices onto the atlas border
instead of failing. Every triangle touching them sampled the atlas edge instead of its own texels.

Measured on `5a81c11`: **10 793 of 266 212 vertices pinned to u = 1**, distributed as 9 996 below
100 cm, 583 on the shirt, 74 on the hair. The reference render puts the damage exactly where the
field screenshots do: blue-grey smears down the legs and along the upper arm.

After the fix: **0 pinned vertices, 30 zero-area triangles** (from 17 271). The clamp is replaced
by a hard invariant that fails the unwrap and names the count, because a chart outside its cell
cannot be repaired by clamping.

### AF-035 — the frame rate (High, fixed)

`Rig::skin()` built the joint skinning matrix `jointWorld * inverseBind` **inside the per-vertex
loop**, once per influence: 266 212 vertices × up to 4 influences ≈ 1.06 M 4×4 products per frame
where 75 are needed. On top of that, `applyCurrentPose()` ran a full `areaWeightedNormals()`
triangle pass (367 698 triangles, 3.2 MB scratch allocated every frame) and streamed the 23 MB
vertex buffer twice to write positions and normals separately.

| Change | Effect |
|---|---|
| Skinning matrices hoisted to one per joint per frame | 10.14 → 6.48 ms |
| Per-joint cofactor 3×3 for normal skinning instead of a triangle pass | 6.48 → 4.65 ms |
| The three writes fused into one pass | included above |

**10.14 → 4.65 ms per frame, 2.2×**, with the mesh still finite, welded and inside bounds. Device
frame rate remains UNVERIFIED. This is the mechanism behind the 16-18 FPS shown under clip
playback in the screenshots, and it is why the 30 FPS thermal pacing target was not being met.

### AF-036 — a test that could not fail (High, fixed)

`testTexturing` asserted "every UV lies inside the atlas" *after* the unwrapper had clamped
everything into [0, 1]. The assertion was unfalsifiable by construction, which is how AF-034
survived a green suite. The clamp is gone, and the test now also checks

- texel-density percentiles against the median (p01 and p99 within 400×), and
- that no triangle which could own a texel collapsed to zero UV area.

### Harness crash (fixed)

`testTexturing` indexed into empty chart and UV arrays when the unwrap failed and **segfaulted**,
turning a reportable failure into a crash with no output. It now reports and returns.

## 4. Measured, recorded, and deliberately not shipped

Both of these were implemented far enough to measure, and reverted. They are recorded so the next
attempt does not repeat the work.

- **AF-037 — chart merging.** Absorbing small charts into neighbours cut 14 460 charts to 9 144,
  but texel density fell five-fold: the shelf packer cannot place the enlarged rectangles. Reverted.
- **AF-038 — a per-chart minimum interior of 6 texels.** This fixes the "chart smaller than its
  own gutter owns no texels" problem at 2048 (densest 1 % of triangles 229 000× above the median →
  546×; area above 0.1 uv²/m²: 0 → 22 911 triangles), but with 14 460 charts it needs 136 % of a
  1024 atlas and, forced to fit, costs a third of the texel density and creates 37 texel-covering
  mirrored triangles. Reverted. The real fix is fewer, better charts plus a skyline packer.

## 5. Art direction (new)

`docs/audit/ART_DIRECTION_TARGETS.md` turns the nine reference images into measurable criteria.
The headline measurement is AF-040: shoulder : hip is **0.65** in this build against **1.05-1.10**
in both references, i.e. the hip is wider than the shoulder where the reference has the opposite,
and shoulder : waist is 0.64 against 0.74-0.76. Recorded with the acceptance criteria for the next
art pass; **not** changed in this pass, because a proportion change without a before/after render
is exactly the kind of unverified "improvement" this campaign exists to stop.

Visible geometry defects recorded from the reference render: shoulder pads (two spheres at the
deltoids), bust as two spheres, a flat slab torso with no hem, a neck that erases the
sternocleidomastoid line, and hair that reads as a solid shell.

## 6. Validation performed in this pass

| Suite | Result |
|---|---|
| Kernel host (`tools/build_host.sh test`) | **411 checks, 0 failures** (was 406; the 5 new ones are the AF-034 assertions) |
| Model detail (`tools/test_model_host.sh`) | **33 checks, 0 failures** |
| Renderer host (`tools/test_renderer_host.sh`) | **64 checks, 0 failures** (no physical GPU) |
| Android CI | run **40915039614 SUCCESS** on `eb1bb60` (8m10s), then run **40919320455 SUCCESS** on `af8d14e` — NDK build, JVM tests, GLES artifact verification, debug APK, unsigned release AAB, SDK AAR. Artifact `AvatarForge-Android-af8d14ee…`, 17,672,820 bytes, id 12918316597. The signed-release job was skipped (no credentials) |
| Reference render, before/after | `/home/user/fault-review/probe/before.png` and `after.png` |
| Device | **UNVERIFIED**. No hardware in this environment; the user's captures cannot be attributed to a build |

## 7. Remaining risks and blockers

1. **Device verification is still the gap that matters.** Every visual claim here is host-side.
   The next capture should be from a build that stamps its commit into the recovery header
   (AF-039), or the two cannot be reconciled.
2. **Thermal:** MEDIUM is capped to LOW on every report. Whether that is honest sensor data or a
   sticky listener is unresolved.
3. **Atlas quality:** 14 460 charts for 367 698 faces is the root of the remaining texture
   softness. AF-037/AF-038 record why the two obvious fixes fail.
4. **Proportions:** AF-040, open, with acceptance criteria written.
5. **Signed release:** BLOCKED, no keystore in this environment. Unchanged.
6. **Rust crates:** uncompiled, UNVERIFIED. Unchanged.

## 8. Changed files in this pass

```
native/src/af_uv.cpp             AF-034: rotated-chart scaling, fail-closed overflow check
native/src/af_rig.cpp            AF-035: skinning matrices hoisted out of the vertex loop
native/src/af_engine.cpp         AF-035: cofactor normal skinning, fused vertex write
native/tests/host_tests.cpp      AF-036: falsifiable UV density + zero-texel checks, clean bail-out
docs/audit/FINDINGS.md           AF-034 .. AF-040
docs/audit/TEST_MATRIX.md        exact counts and the pose-cost row
docs/audit/ART_DIRECTION_TARGETS.md   new: measurable art-direction criteria
docs/audit/AUDIT_2026-10-30_FIELD_EVIDENCE.md   this document
```

## 9. Next required action

Rebuild, re-capture on the SM-S921B, and compare against `after.png` at the same yaw the
screenshots used (they are near-front and near-profile). If the blue-grey leg and arm smears are
gone, AF-034 is device-confirmed and this defect class is closed. If they are still present, the
next suspect is the GLES texture-unit/format path (`uploadAtlas`, `GL_SRGB8_ALPHA8` on the base
colour) rather than the unwrapper, and the reference render says so because it will disagree with
the device.
