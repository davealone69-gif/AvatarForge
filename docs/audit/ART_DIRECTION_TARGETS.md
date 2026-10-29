# Art-direction targets

Status: **reference set recorded, measured against the current build, gap open as AF-040.**
Source: the user supplied Dropbox folder `avatar/guides/body` on 2026-10-30 with the instruction
"Use these as guides". Nine images: two DAZ3D Genesis-8 marketing renders (Kira HD, Ms Lucy HD),
four studio body/pose references, two cropped shape references and one VRM/VTuber character sheet.

This file exists so that "make the model look right" becomes something that can be measured and
regressed, in the same way `docs/audit/TEST_MATRIX.md` turned "does it work" into checks. Nothing
here is copied from a licensed asset: the images supply *proportions* and *material intent*, and
every number below is a ratio that can be verified against the engine's own landmark output.

## 1. What the references establish

| Property | Reference reading | Why it matters |
|---|---|---|
| Silhouette | Athletic-adult female, adult proportions, roughly 7.5 heads tall | The current build is anatomically plausible in height but not in mass distribution |
| Shoulder : waist | 0.74–0.76 (both Genesis-8 references) | A waist this narrow relative to the shoulder is most of what reads as "athletic" |
| Shoulder : hip | ≈ 1.05–1.10 (shoulders just wider than hips) | The current build inverts this |
| Neck | Long, narrow, clearly separated from the trapezius | A wide neck reads as stylised immediately |
| Limbs | Deltoid and calf defined; forearm tapers hard to the wrist; ankle distinctly narrower than calf | Blocky limbs are the second-strongest stylisation cue after proportion |
| Hand | Relaxed, fingers slightly curled, visible knuckle line | The current build's fingers are straight rods |
| Chest / bust | Modest, supported, no separate sphere geometry | The current build's chest geometry reads as two attached spheres (see below) |
| Garment | Fitted top that follows the torso and ends at the waist; fabric reads as fabric (soft sheen, fold shading, hem shadow) | The current build's shirt is a flat slab with no visible hem/waist relationship |
| Skin | Subsurface warmth at ears/nose/knuckles, visible specular break-up, no plastic uniformity | Current bake is flat matte at any distance |
| Hair | Ponytail/long, strand clumps with a specular sheen line, scalp not visible through it | Current hair reads as a solid dark shell with a hard silhouette edge |
| Feet | Bare or shoe; arch and ankle bone visible | Current build has neither |

## 2. Measured gap, current build

Measured from the engine's own vertex buffer (`/home/user/fault-review/probe/geom_probe.cpp`,
1.72 m female athletic spec, t-shirt outfit):

| Metric | Reference target | Current | Verdict |
|---|---|---|---|
| Height | 1.72 m nominal | 1.743 m bounds | OK |
| Shoulder width (biacromial) | ≈ 0.38 m | 0.361 m | close |
| Waist width | 0.60–0.65 × shoulder | 0.231 m (0.64×) | **short of target** |
| Hip width | 0.92–0.98 × shoulder | 0.554 m (1.53×) | **wrong sign** |
| Shoulder : hip | 1.05–1.10 | 0.65 | **inverted** |
| Head height | ≈ 1/7.5 of stature | ≈ 0.23 m (1/7.6) | OK |
| Max triangle edge | any | 13.4 mm | OK (no spikes) |
| Non-finite vertices | 0 | 0 | OK |

The hip measurement includes the lower garment, which is why the ratio is so far off; the
underlying landmark solver is still the thing to change, not the garment.

## 3. Known geometry defects visible in the reference render

These come from the offline renderer (`/home/user/fault-review/probe/preview.cpp`, which
rasterises the engine's own vertex buffer and baked atlas - it is a diagnostic, not a claim about
the device):

1. **Shoulder pads.** The t-shirt's upper garment produces two distinct spheres at the deltoids.
   In the reference, fabric drapes over a deltoid; it never forms a ball. Root cause is the
   garment field's shoulder shell, not the shading.
2. **Bust as two spheres.** Same generator, same class of error: the bust is added as a pair of
   primitives instead of being part of the torso's field.
3. **Flat slab torso.** From waist to hem the shirt has no fold, hem or body-following curvature.
4. **Neck / trapezius blend.** The neck merges into the shoulders with a smooth-min radius that
   erases the sternocleidomastoid line.
5. **Hair as a shell.** The ponytail is a solid mass; no clump separation and no sheen line.

## 4. Acceptance criteria for the next art pass

A change to proportions or garment geometry is accepted only when all of the following hold, and
each one is either an existing host check or an added one:

- [ ] Shoulder : hip in 1.05–1.10 and shoulder : waist in 1.48–1.67, asserted in
      `native/tests/model_detail_tests.cpp` against the built mesh (not the spec).
- [ ] No garment component whose bounding-box aspect is within 1.25 of a sphere and whose volume
      exceeds 50 cm³ (catches the shoulder pads and the bust spheres).
- [ ] Upper-garment hem within 2 cm of the measured waist landmark; hem visible in the silhouette.
- [ ] Calf at least 1.35× the ankle circumference measured on the built mesh.
- [ ] `tools/test_model_host.sh`, `tools/test_renderer_host.sh` and the kernel suite stay at 0
      failures, and the three numbers above do not regress.
- [ ] A before/after offline render is attached to the change, because a proportion fix that only
      passes ratios can still look wrong.

## 5. What is explicitly out of scope

- Photorealistic skin: the references are offline-rendered marketing images with multi-bounce
  subsurface scattering. Matching that on an Xclipse 940 under thermal-throttle budget is not a
  claim this project makes (see the honesty rules in `PROJECT_STATUS.md`).
- Recreating any specific commercial model. The references supply proportions; the generator still
  produces its own mesh.
