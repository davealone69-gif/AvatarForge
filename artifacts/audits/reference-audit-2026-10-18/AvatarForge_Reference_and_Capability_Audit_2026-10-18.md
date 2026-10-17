# AvatarForge Reference and Capability Audit

**Audit date:** 2026-10-18 (Australia/Perth)  
**Project state audited:** AvatarForge source tree identifying as 0.10.1; latest 0.10.1 Android package/device validation is not complete  
**Dropbox mode:** read-only audit; no Dropbox file was edited, moved, copied, renamed, or deleted

## Status legend

- **VERIFIED** — directly established from decoded media, exact Dropbox content hashes, source code, or a completed test.
- **PARTIAL** — useful evidence exists, but the inspection or validation is incomplete.
- **UNVERIFIED** — not established by the available references or not run on the current build.
- **BLOCKED** — cannot currently work as claimed because an implementation or validation prerequisite is missing or defective.

---

## 1. Executive verdict

1. **Canonical identity anchor — VERIFIED:** `/FavouritesMYLOVE/Love.jpg` is the strongest available face-identity reference. It must outrank every other red-haired image.
2. **Supporting still — VERIFIED with caveat:** `/FavouritesMYLOVE/love-2.jpg` is the best secondary angle/hair/body reference, but its green/grey eye treatment and softer facial rendering drift from the canonical red-eye design. It is supporting evidence, not an equal identity source.
3. **Motion reference — PARTIAL:** `/FavouritesMYLOVE/love2.mp4` appears to be a same-design animation, but only nine evenly spaced frames were inspected. Temporal/generative drift is visible; it must not be treated as nine independent identity views.
4. **No reliable standing full-body or true profile reference exists — UNVERIFIED:** every favourite is posed, perspective-affected, cropped at the feet, or identity-drifting. Exact stature, inseam, limb ratios, rear hair shape, and profile depth cannot be recovered reliably from this set.
5. **All other audited female, fantasy, redhead, armor, cyborg, Random, and `/avatar/new` media are not reliable same-character identity references — VERIFIED.** They may support style, clothing, or mood only.
6. **Inventory — VERIFIED:** the authoritative CSV records **257 Dropbox paths representing 166 unique file payloads**. It contains 174 JPEG paths, 48 WebP paths, 11 PNG paths, 17 MHTML archives with inspected embedded images, and 7 MP4 paths. There are 91 exact duplicate/mirror paths. All 250 non-video records are VERIFIED; all seven video paths are honestly marked PARTIAL.
7. **Pixel ingestion — VERIFIED:** Android decodes real selected pixels and sends direct RGBA buffers to native code.
8. **Identity preservation — BLOCKED/unsupported:** the engine has no identity embedding, dense face landmarks, 3D morphable-model fit, monocular face reconstruction, reference-conditioned mesh generator, texture projection, or multi-view identity fusion.
9. **Public reference-image generation — BLOCKED:** Kotlin and JNI allocate 22 floats per analysis but currently read/write 26. A complete encoding of the native struct would require 28 floats because `background_lab` has three components. The observed `length=44; index=44` failure in `/fault` is consistent with this defect.
10. **Text prompts do not preserve identity:** the prompt below is a verified-detail specification, not an identity guarantee. It becomes useful only after real image conditioning is implemented and measured.

---

## 2. Audit scope and method

### Covered reference locations

- `/FavouritesMYLOVE` and `/FavouritesMYLOVE/semi favs`
- `/fault`
- Dropbox root files relevant to the reference set
- `/avatar` root videos
- `/avatar/new` (all three listing pages)
- `/avatar/female` and `/avatar/female/Females`
- `/avatar/females2`
- `/avatar/armor`
- `/avatar/cyborgs`
- `/avatar/guides`, `/avatar/guides/body`, and `/avatar/guides/tats`
- `/avatar/icon` and `/avatar/intro`
- `/avatar/avatar2/Avatar`, including `Armor`, `Cyborgs`, `Females`, `Random`, and `Random/Bodys`
- `/avatar/ramdom/Random` — verified empty

Targeted searches under `/check` surfaced predominantly software/UI screenshots, especially under `/check/new/GOLDMINE V1`. Those broad search results were truncated and are **not** represented as a complete account-wide `/check` inventory. No candidate capable of displacing the canonical favourite was found there.

### Inspection standard

- Still images were downloaded, decoded, and inspected directly or on labelled contact sheets.
- MHTML files were not judged from filenames. Their image MIME parts were parsed, the target embedded image was extracted by content location/title matching, decoded, and inspected. The embedded targets are low resolution, approximately 239–360 pixels per side.
- Exact mirrors were established with Dropbox content hashes. The unique underlying pixels were inspected once; mirror aliases were not falsely counted as independent visual evidence.
- Videos were decoded with OpenCV and sampled at evenly spaced timestamps. They were **not** reviewed frame by frame and remain **PARTIAL**.
- Same red hair, pale skin, fangs, gothic setting, or similar body type was not accepted as proof of the same face.

### Authoritative inventory artifacts

- `AvatarForge_Reference_Inventory_2026-10-18.csv` — one row per exact Dropbox media/archive path, with type, source/mirror role, identity relation, inspection status, duplicate/derivative relation, and notes.
- `AvatarForge_Source_Evidence_0.10.1.txt` — numbered source excerpts supporting the capability findings.

---

## 3. Inventory summary

| Location | Path count | Result |
|---|---:|---|
| `/FavouritesMYLOVE` including `semi favs` | 12 | 11 stills + 1 MP4; one canonical, one best supporting still, one same-design motion reference, remaining files variants/style-only |
| `/fault` | 7 | Failure/UI evidence, not identity references |
| Dropbox root relevant files | 10 | 8 exact armor aliases + 2 derived MP4s |
| `/avatar` root | 2 | Unrelated beach video and unrelated pink-haired motorcycle video |
| `/avatar/new` | 63 | 62 unique hashes; no reliable canonical match; one exact `valery... (1)` alias |
| `/avatar/female` | 20 | 17 unique hashes; unrelated people/designs; three filename aliases |
| `/avatar/female/Females` | 20 | Exact mirror set of `/avatar/female` |
| `/avatar/avatar2/Avatar/Females` | 20 | Exact mirror set of `/avatar/female` |
| `/avatar/females2` | 16 | Different DeviantArt character designs |
| `/avatar/armor` | 8 | Generic armor/body references |
| Armor mirrors: root, guides/body, avatar2/Armor, Random/Bodys | 32 | Four 8-file exact mirror sets |
| `/avatar/cyborgs` | 12 | Unrelated cyborg references |
| `/avatar/avatar2/Avatar/Cyborgs` | 13 | 12 mirrors + one suffixed duplicate alias |
| `/avatar/avatar2/Avatar/Random` | 21 | 17 MHTML embedded targets + 4 stills; unrelated/alternate body and character references |
| `/avatar/guides/tats` | 2 | Tattoo design references only |
| Gemini video mirrors | 3 total paths | One unique unrelated video at three exact paths |
| `/avatar/intro` and `/avatar/icon` | 5 | App UI/branding screenshots |
| `/avatar/ramdom/Random` | 0 | Empty |

**Total:** 257 paths, 166 unique payloads, 91 exact aliases/mirrors, and two non-identical derived animations.

---

## 4. Canonical ranking and selected paths

### Tier A — use

#### 1. Face identity anchor

**Path:** `/FavouritesMYLOVE/Love.jpg`  
**Status:** **VERIFIED — CANONICAL**  
**Dimensions:** 768 × 1152

Why it wins:

- clearest combined evidence for face shape, eyes, nose, lips, jaw, cheek structure, pallor, red-eye treatment, fangs, and dark burgundy-red hair;
- face is comparatively sharp and unobscured;
- body remains visible enough to support only broad build classification;
- its face must not be averaged with the unrelated redheads elsewhere in Dropbox.

Limitations: posed body, feet absent, no neutral expression, no true profile, low-key coloured lighting, and apparent generative/retouching artifacts.

#### 2. Supporting alternate angle, hair, body, and expression

**Path:** `/FavouritesMYLOVE/love-2.jpg`  
**Status:** **VERIFIED — SUPPORTING, NOT EQUAL IDENTITY TRUTH**  
**Dimensions:** 768 × 1152

Use it for:

- a different near-frontal head orientation;
- long loose hair volume and length;
- broad confirmation of a very slender build;
- parted-lip/fang expression.

Do not use it to override the canonical eye design or to average the face. Its green/grey eye treatment and softer facial rendering are identity/design drift.

#### 3. Same-design motion cue

**Path:** `/FavouritesMYLOVE/love2.mp4`  
**Status:** **PARTIAL**  
**Properties:** 768 × 1168, 145 frames, 24 FPS, 6.042 seconds

Use only for coarse motion/expression intent. Do not extract multiple frames and treat them as independent calibrated views; sampled frames exhibit temporal drift.

### Tier B — style/design variants only

- `/FavouritesMYLOVE/love-1.jpg`
- `/FavouritesMYLOVE/love3.jpg`
- `/FavouritesMYLOVE/love-3.jpg`

These share the intended pale red-haired vampire concept but vary facial proportions, jaw/cheek treatment, eyes, expression, or rendering. They may inform mood and hair styling only after the canonical identity is locked.

### Tier C — exclude from identity fitting

- all six `/FavouritesMYLOVE/semi favs/*.png` files;
- both Dropbox-root UUID animations;
- every audited file under `/avatar/new`, `/avatar/female`, `/avatar/females2`, Random, armor, cyborg, guide, icon, and intro sets.

The two root UUID videos are derived animations, not independent evidence:

- `/a18028a1-6e99-451a-bbba-6bf6f43bbd11.mp4` derives from the `love3.jpg` design variant;
- `/9403568a-f4ef-4d09-9f79-26afd8b0300e.mp4` derives from `/FavouritesMYLOVE/semi favs/17906205023c3f.png`.

### Requested category selections

| Category | Exact selected path | Status |
|---|---|---|
| Clearest face | `/FavouritesMYLOVE/Love.jpg` | **VERIFIED** |
| Canonical hair colour/style | `/FavouritesMYLOVE/Love.jpg` | **VERIFIED** |
| Additional hair volume/length | `/FavouritesMYLOVE/love-2.jpg` | **VERIFIED**, supporting only |
| Broad body build | `/FavouritesMYLOVE/Love.jpg` | **PARTIAL evidence within a verified still**; posed and feet cropped |
| Alternate angle | `/FavouritesMYLOVE/love-2.jpg` | **VERIFIED**, with eye/face drift caveat |
| Expression/fangs | `/FavouritesMYLOVE/Love.jpg`, then `/FavouritesMYLOVE/love-2.jpg` | **VERIFIED** |
| Motion | `/FavouritesMYLOVE/love2.mp4` | **PARTIAL** |
| Standing full body | none | **UNVERIFIED / missing** |
| True side profile | none | **UNVERIFIED / missing** |
| Rear hair view | none | **UNVERIFIED / missing** |

---

## 5. Character Identity Reference Profile

This profile separates what is visibly established from what the current references cannot establish.

### Identity and face

| Feature | Finding | Status |
|---|---|---|
| Age class | Adult-coded female character; no exact age should be inferred | **VERIFIED** at broad class only |
| Overall face | Narrow, elongated oval with a softly heart-like taper toward the chin | **VERIFIED** from canonical view |
| Forehead/hairline | Medium-to-high visible forehead; hairline partly affected by loose hair and pose | **VERIFIED**, incomplete |
| Eyes | Almond-shaped, moderately spaced, with a slightly lifted outer contour and heavy upper-lid treatment | **VERIFIED** |
| Eye colour/design | Canonical image has a red iris/inner-eye treatment. The natural underlying iris colour is not separable from styling, lighting, or post-processing | Red treatment **VERIFIED**; underlying colour **UNVERIFIED** |
| Brows | Dark burgundy-brown, medium thickness, gently arched rather than flat | **VERIFIED** |
| Nose | Narrow, comparatively straight bridge; compact tip and narrow alar width in the available near-front view | **VERIFIED** in frontal projection; profile depth **UNVERIFIED** |
| Lips | Moderately full, defined upper bow, fuller lower lip, dark burgundy-red treatment; usually parted | **VERIFIED** |
| Fangs | Two prominent upper canine fangs are part of the canonical design | **VERIFIED**; exact dental geometry **UNVERIFIED** |
| Cheeks | High/prominent cheek structure with restrained cheek fullness and some sub-cheek hollowing | **VERIFIED** |
| Jaw | Narrow, tapered jaw; not broad, square, or heavy | **VERIFIED** |
| Chin | Compact, low-tapered, softly pointed/rounded rather than wide or cleft | **VERIFIED** in near-front view; profile projection **UNVERIFIED** |
| Ears | Mostly hidden by hair | **UNVERIFIED** |
| Facial asymmetry | Pose, expression, and generative rendering prevent a reliable asymmetry map | **UNVERIFIED** |

### Skin and surface

| Feature | Finding | Status |
|---|---|---|
| Skin tone | Very pale, cool-to-neutral appearance with low apparent melanin | **VERIFIED** as rendered appearance |
| Neutral albedo | Low-key lighting and grading prevent a trustworthy neutral skin-albedo sample | **UNVERIFIED** |
| Freckles, scars, moles | No stable defining marks are established | **UNVERIFIED**, not proven absent |
| Makeup | Dark/red eye treatment and burgundy lip styling are visible | **VERIFIED** |
| Texture realism | Smooth stylized/retouched photorealism; do not infer pores or biological skin microstructure from it | **VERIFIED** limitation |

### Hair

| Feature | Finding | Status |
|---|---|---|
| Base appearance | Very dark wine/burgundy red; deep shadows approach near-black | **VERIFIED** as rendered appearance |
| Exclusions | Not bright orange/copper, bubble-gum pink, or blonde | **VERIFIED** relative to canonical image |
| Length | Very long, extending past the chest and toward waist/hip in the supporting views | **VERIFIED** broadly |
| Shape | Loose, voluminous, wavy, with irregular strands rather than tight curls or a sleek straight sheet | **VERIFIED** |
| Parting | Loose off-centre/irregular part affected by pose | **VERIFIED**, not a rigid styling constraint |
| Rear silhouette and density | No rear view exists | **UNVERIFIED** |
| Neutral-light colour | Red grading and low-key illumination prevent an exact neutral sRGB/PBR albedo | **UNVERIFIED** |

### Body

| Feature | Finding | Status |
|---|---|---|
| Build | Very slender/ectomorphic presentation with narrow shoulders and ribcage, a defined waist, moderate hip width, and long slim limbs | **VERIFIED** broadly |
| Muscularity | Low visible bulk; avoid athletic or heavily muscular interpretation | **VERIFIED** broadly |
| Height | No scale object, known camera, or standing full-body image | **UNVERIFIED** |
| Exact shoulder/waist/hip ratios | Posed torso, foreshortening, and generative drift make precise ratios unreliable | **UNVERIFIED** |
| Leg length/inseam | Bent/cropped legs and missing feet prevent a coherent exact measurement | **UNVERIFIED** |
| Hands | Slim hands and long fingers are visible, but generative images may distort anatomy | **PARTIAL** |
| Clothing/footwear | The favourite set establishes no canonical outfit or footwear | **UNVERIFIED / missing** |
| Equipment/tattoos | None is established for the canonical identity | **UNVERIFIED**, not proven absent in unseen areas |

### Expression, angles, lighting, and quality

| Category | Finding | Status |
|---|---|---|
| Primary expression | Intense/direct predatory gaze, slightly parted lips, visible fangs; restrained rather than a broad smile | **VERIFIED** |
| Secondary expression | `love-2.jpg` supports a calmer parted-lip fang expression but drifts in eye treatment | **VERIFIED** with caveat |
| Canonical angle | Near-front to slight three-quarter head/torso orientation with pose and some perspective | **VERIFIED** |
| Missing angles | Calibrated front-neutral, true left/right profile, back of head, standing front/side/back | **UNVERIFIED** |
| Lighting | Low-key gothic interior; dark red/black surroundings; directional, non-neutral illumination | **VERIFIED** |
| Resolution | Canonical is 768 × 1152; enough for broad traits, not for high-frequency skin or dental reconstruction | **VERIFIED** |
| Source nature | Images appear generative/stylized and include a Grok mark; they are not calibrated multi-camera biological ground truth | **VERIFIED** visual limitation |

### User-approved provisional base specification

The following values were supplied by the user after the visual audit. They are **USER-SPECIFIED APPROXIMATIONS**, not measurements recovered from the images:

| `af_spec` field | Value |
|---|---:|
| `sex` | `AF_SEX_FEMALE` |
| `build` | `AF_BUILD_SLIM` |
| `hair_style` | `AF_HAIR_LONG` |
| `outfit` | `AF_OUTFIT_NONE` |
| `footwear` | `AF_FOOTWEAR_NONE` |
| `height_m` | `1.68` |
| `age_years` | `22.0` |
| `muscle` | `0.25` |
| `body_fat` | `0.18` |
| `bust` | `0.65` |
| `skin_melanin` | `0.08` |
| `hair_tone` | `(0.35, 0.05, 0.05)` linear sRGB |
| `seed` | arbitrary; use a fixed value such as `1337` only for reproducibility |

`OUTFIT_NONE` and `FOOTWEAR_NONE` describe the anatomical fitting base. They do not satisfy the standing requirement for a final fully clothed and shod avatar; production wardrobe must be selected separately.

The exact machine-readable record is `Love_Provisional_Base_Spec_2026-10-18.json`.

### Eyes, teeth, and fangs

- The engine contains generic upper/lower dental-arch geometry, but **no dedicated elongated canine/fang geometry**.
- The 52 facial channels are the standard ARKit-compatible set. None is a fang channel.
- A fixed vampire design needs custom canine geometry. Retractable fangs additionally need custom, non-ARKit morph targets and matching export/runtime support.
- The current blendshape implementation stores/generated per-vertex deltas and linearly combines weighted deltas on CPU when expression weights change; no neural expression synthesis occurs.
- Source inspection currently shows CPU linear-blend skinning followed by addition of the cached morph deltas. Therefore “morph before skinning” is not yet the implemented order and needs a separate correction if strict glTF deformation equivalence is required.

---

## 6. Verified-detail generation prompt

> **Important:** This prompt is a descriptive constraint set. It cannot preserve facial identity by itself. Use it only with a real, measured identity-conditioning pipeline in which `/FavouritesMYLOVE/Love.jpg` is the sole hard face anchor.

```text
Create a complete, riggable, UV-mapped, full-body 3D avatar of the adult-coded female vampire in the identity-conditioned canonical reference `/FavouritesMYLOVE/Love.jpg`.

IDENTITY PRIORITY: preserve the canonical face before hair, and preserve hair before body proportions. Do not average the face with other red-haired women or with style references. Keep a narrow elongated oval-to-soft-heart face, prominent high cheek structure with restrained cheek fullness, a narrow tapered jaw, and a compact softly pointed/rounded chin. Keep almond-shaped moderately spaced eyes with the canonical subtle red iris/inner-eye treatment and dark red eye styling; do not substitute green, blue, or generic brown eyes. Keep the narrow comparatively straight nose, compact tip, moderately full burgundy lips with a defined upper bow and fuller lower lip, and two visible upper canine fangs.

HAIR: very long, loose, voluminous wavy hair in a dark wine/burgundy red. Deep shadow areas may approach near-black. Do not make it bright copper, orange, pink, blonde, short, tightly curled, or perfectly straight.

SKIN AND BUILD: very pale cool-to-neutral rendered skin appearance. Slender ectomorphic adult proportions, narrow shoulders and ribcage, defined waist, moderate hips, and long slim limbs with low muscular bulk. Do not exaggerate curves, musculature, head size, or childlike proportions. Place the chin lower enough to preserve the canonical long lower-face taper. Keep hip, knee, ankle, crotch, and inseam landmarks mutually coherent.

EXPRESSION: direct intense gaze, lips slightly parted, restrained predatory expression, two upper fangs visible. Avoid a broad smile, snarl, cartoon expression, or extra/misaligned teeth.

EVALUATION PRESENTATION: neutral A-pose, level head, neutral grey background, diffuse colour-neutral studio lighting, and a perspective suitable for identity comparison. Produce front, three-quarter, profile, and full-body turntable renders from the same fixed identity and mesh.

WARDROBE: because no canonical outfit is verified, use a deliberately neutral, fully modelled and textured matte charcoal fitted bodysuit plus simple flat ankle boots for the reconstruction/evaluation pass. Treat this as non-canonical evaluation wardrobe, not evidence from the reference. Add no armor, tattoos, jewellery, or equipment unless separately approved.

QUALITY: anatomically coherent face and hands; stable identity across yaw and expression; physically plausible PBR skin/hair/clothing materials; complete clothing and footwear; no missing body parts, geometry intersections, texture projection seams, generic beauty-face substitution, or view-to-view identity drift.
```

---

## 7. Source-level AvatarForge capability audit

### What is genuinely implemented

| Capability | Status | Evidence and actual scope |
|---|---|---|
| Decode real user-selected images | **VERIFIED** | `AppBitmaps.kt:39–57` decodes content URIs, applies EXIF orientation, caps the long edge at 1600, and keeps ARGB pixels. |
| Send real pixels to native code | **VERIFIED** | `ImageReference.fromBitmap` creates a direct RGBA `ByteBuffer`; JNI obtains its direct address and populates `af_image`. |
| Classical image analysis | **VERIFIED** | `af_image.h` documents deterministic border-background segmentation, width-profile landmarks, colour classification, median Lab colours, metalness, and key light on a ≤256-pixel analysis grid. |
| Coarse appearance/proportion influence | **VERIFIED, LIMITED** | `specFromAnalysis` may refine default stature from head count for a detected full body; sets skin melanin, hair colour, cloth/gear colour, a generic outfit class, and footwear class. |
| Procedural face/body geometry | **VERIFIED** | `af_anatomy.cpp` derives head, jaw, nose, lips, eyes, and body from fixed ratios and `af_spec`; it does not fit those dimensions to a face. |
| Procedural PBR texture baking | **VERIFIED** | `af_texture.cpp` creates base colour, normal, metallic/roughness, occlusion, and subsurface maps from the procedural mesh/spec/noise. |
| Offline manifest posture | **VERIFIED at source** | The app manifest declares no `INTERNET`, network-state, Wi-Fi, storage, or camera permission. Final merged-manifest/APK verification for 0.10.1 remains unrun. |

### What is not implemented

| Claimed/desired capability | Status | Finding |
|---|---|---|
| Face recognition or identity embedding | **BLOCKED / absent** | No embedding model, identity vector, similarity loss, or identity database exists. Repository scans found no TFLite, ONNX, PyTorch, MediaPipe, NCNN, MNN, or comparable runtime/model assets. |
| Dense facial landmarks | **BLOCKED / absent** | The analyser produces one coarse head box and vertical body rows, not stable eye/nose/lip/jaw landmarks. |
| 3D face reconstruction / 3DMM fitting | **BLOCKED / absent** | No morphable head model, shape/expression coefficients, camera fit, photometric fit, or differentiable rendering path exists. |
| Image-conditioned face mesh | **BLOCKED / absent** | Face dimensions are fixed anthropometric ratios; reference pixels do not set face-specific geometry. |
| Body mesh fitting to silhouettes/keypoints | **BLOCKED, except coarse estimates** | No optimizer fits the generated mesh to image keypoints or silhouettes. Only approximate vertical profile/head-count values influence a generic spec. |
| Reference-pixel texture projection | **BLOCKED / absent** | Texture baking is procedural. Reference skin/hair/garment pixels are not projected into UV space and no de-lighting or seam blending occurs. |
| Clothing geometry reconstruction | **BLOCKED / absent** | A material-confidence threshold selects a generic outfit enum. The referenced garment volume is not segmented and reconstructed despite SDK comments claiming geometric transfer. |
| Multi-view fusion | **BLOCKED / absent** | `AF_IMAGE_EXTRA_VIEW` is named “multi-view fusion”, but the implementation stores analyses and selects one identity plus optional clothing source. It does not jointly solve multiple cameras/views. |
| Text-prompt identity preservation | **BLOCKED / impossible in current design** | The prompt parser maps words to procedural spec values. It has no representation of the `Love.jpg` face. |
| End-to-end image generation through the public API | **BLOCKED by packing defect** | The 22/26-float mismatch causes native overflow/truncation and Kotlin out-of-bounds reads before a reliable reference-based generation can complete. |

### Documentation/API overclaims

The SDK comments say Image A “gives the face” and that Image B is transferred geometrically and texturally. The C header calls `AF_IMAGE_EXTRA_VIEW` multi-view fusion. The implementation does not support those claims. Until the missing systems are implemented and measured, the UI/API documentation should say:

- **Image A:** coarse appearance/proportion sampler;
- **Image B:** generic garment colour/material/outfit-class hint;
- **extra views:** accepted analyses, not fused identity views.

This wording change is required to satisfy the no-fakes rule.

---

## 8. Blocking 22/26/28-float defect

### Exact defect

- Kotlin declares `ANALYSIS_FLOATS = 22` and allocates `FloatArray(n * 22)`.
- JNI declares `perImage = 22` and allocates `count * 22` floats.
- JNI then writes **26** values per image:
  - subject coverage: 1
  - head box: 4
  - shoulder/hip/ankle/knee: 4
  - full-body flag: 1
  - pose confidence: 1
  - skin Lab: 3
  - hair Lab: 3
  - garment Lab: 3
  - metal confidence: 1
  - key-light direction: 3
  - key-light intensity: 1
  - only background L*: 1
- Kotlin’s unpacker also attempts to read those same **26** values while stepping each record by 22.
- The native C struct actually contains `background_lab[3]`. A complete, internally consistent schema therefore needs **28 floats**, not 26.

### Runtime consequence

- Native writes run past the `std::vector<jfloat>` allocation: undefined behaviour/memory corruption.
- JNI copies only `n * 22` floats back, truncating each record.
- With one image, Kotlin eventually accesses index 22 of a 22-element array.
- With two images, the first 26-value unpack spills into the second 22-value record; the second unpack starts at index 44 and produces `length=44; index=44`, matching the inspected `/fault/Screenshot_20261018_014917_AvatarForge.jpg`.
- `MainActivity` hides the first failed pre-analysis with `runCatching`, but generation performs analysis again and fails before reliable generation.

### Required correction before any operational claim

1. Define one generated/shared analysis schema and set its stride to **28**.
2. Pack all three `background_lab` values.
3. Unpack `backgroundLab = floatArrayOf(f(), f(), f())`.
4. Reject any output array whose size is not exactly sufficient; never silently skip packing.
5. Assert `k == stride` in native debug/test builds.
6. Add 1-, 2-, 3-, and 4-image sentinel tests that verify every field and guard byte under ASan where possible.
7. Add an Android instrumentation test using real decoded bitmaps and the public `generateFromImage` path.
8. Run the corrected build on a physical target in airplane mode and verify GLB export, reload, renderer lifecycle, and memory use.

Until those steps pass on a real Android device, reference-image generation remains **BLOCKED**.

---

## 9. Smallest practical reconstruction set

### Best safe use of the files that already exist

There is **no fully consistent calibrated set** in Dropbox. The least-contradictory working set is:

1. `/FavouritesMYLOVE/Love.jpg` — sole hard face/eye/nose/lip/jaw identity anchor.
2. `/FavouritesMYLOVE/love-2.jpg` — soft constraint for alternate orientation, hair volume/length, broad build, and fang expression; mask or ignore its eye colour and do not average its face into the anchor.

Do not add the semi-favourites, Random redheads, or `/avatar/new` images merely to increase count. More unrelated images would reduce identity fidelity. Do not count frames from `love2.mp4` as independent cameras.

This two-file set is a **design-reference set**, not enough for reliable automatic 3D reconstruction.

### Minimum acquisition set for a credible automatic result

The smallest practical set is **three mutually consistent, identity-locked images**:

1. high-resolution neutral front face/head-and-shoulders view under diffuse neutral light;
2. high-resolution three-quarter or true profile view of the same fixed identity, expression, focal-length class, and lighting;
3. standing full-body front A-pose in fitted neutral clothing, with the entire head, hands, legs, and feet visible plus a known height or scale reference.

A **fourth rear full-body/head view is strongly recommended** for the very long hair and back silhouette. For a fictional character, these views must come from the same latent/3D source or be artist-approved against `Love.jpg`; independently prompted “similar redhead” generations are not valid.

---

## 10. Realistic offline-first missing-capability plan

All items in this section are **UNIMPLEMENTED** unless explicitly marked otherwise. Advanced identity work must remain gated behind the existing Phase 1 reliability work: current height, GLB, renderer lifecycle, frame pacing, build, and device tests first.

### Gate 0 — truthful and safe current pipeline

- Fix the 28-float bridge and add sentinel/bounds tests.
- Remove or correct SDK/UI claims about face reconstruction, garment transfer, and multi-view fusion.
- Finish clean 0.10.1 native and Gradle builds from current sources.
- Inspect the merged manifest and APK to prove no `INTERNET` permission or network component was introduced.
- Run physical-device generation/export/reload tests in airplane mode.

**Acceptance:** no out-of-bounds access; 1–4 real images complete analysis; generated GLB reloads; no network permission; measured peak RAM below 2 GB. This only validates coarse appearance sampling, not identity.

### Gate 1 — licensed on-device reference understanding

Bundle redistributable, checksummed model assets in the APK/AAB; never download them. Add:

- face detection;
- dense 2D/3D facial landmarks;
- face/hair/skin/garment segmentation;
- full-body keypoints and silhouette extraction;
- a compact identity embedding used for comparison/loss, not as a claim of reconstruction by itself.

Use a C/C++ inference path callable through the existing C ABI. Schedule one model at a time, memory-map weights, and provide NNAPI, Vulkan/GPU where appropriate, and CPU fallback. Candidate architectures and every dataset/model licence must be audited before redistribution; models such as FLAME/SMPL derivatives must not be bundled merely because research code is available.

**Estimated, not measured:** a carefully selected quantized model set may occupy roughly 75–250 MB and 200–600 MB transient working memory when loaded sequentially. These numbers remain **UNVERIFIED** until measured on target devices.

### Gate 2 — fixed-topology identity head and multi-view fit

The current procedural/SDF face is not a suitable identity carrier by itself. Introduce a fixed-topology head model with licensed identity and expression bases, then solve jointly for:

- identity shape coefficients shared by every view;
- per-view camera, pose, and expression;
- jaw/eye/lip landmarks;
- silhouette and photometric alignment;
- identity-embedding consistency across rendered yaw angles.

Stitch or blend this fitted head to the procedural body at a stable neck loop. Preserve a stable UV layout and facial blendshape topology. `AF_IMAGE_EXTRA_VIEW` should count as implemented only after all views participate in one joint objective.

### Gate 3 — reference-derived texture and hair

- Estimate and remove illumination before treating pixels as albedo.
- Project visible face/skin pixels into the fixed head UVs from all accepted views.
- Weight by view angle, occlusion, landmark confidence, and exposure.
- Seam-blend and fill unseen regions conservatively; mark unseen/filled texels in diagnostics.
- Derive PBR maps separately; do not bake directional shadows into base colour.
- Fit a parameterised long-hair template/groom to front/side/rear silhouettes. Hair colour sampling alone is already possible coarsely, but strand geometry and rear volume are not.

A single low-key canonical image cannot provide neutral albedo or unseen-side texture; the UI must expose that uncertainty instead of inventing “verified” detail.

### Gate 4 — body fit, clothing, and full avatar integration

- Keep user-entered height authoritative.
- Fit existing anthropometric body parameters to full-body landmarks/silhouettes rather than replacing the body with an unlicensed model.
- Solve front and side silhouettes together; enforce coherent hip, crotch, knee, ankle, and inseam landmarks.
- Implement actual garment geometry only through a template/fit system or an approved reconstruction model. A colour/metalness threshold must not be called garment transfer.
- Preserve complete clothing, footwear, equipment, UVs, rig, clips, and GLB export.

### Gate 5 — measurement and device validation

Pre-register acceptance metrics and test data:

- facial landmark normalised mean error on held-out approved views;
- identity-embedding similarity between references and neutral renders at front, ±30°, ±60°, and profile;
- silhouette IoU for head/hair/body;
- neutral-light hair/skin colour error after de-lighting;
- UV seam and unseen-texel coverage;
- GLB conformance and reload equivalence;
- peak RSS/GPU memory, generation time, sustained FPS, thermal throttling, and battery;
- airplane-mode operation and explicit absence of network permissions/traffic.

Manual approval against `Love.jpg` remains necessary because embedding models can score a generic attractive face too highly, especially for synthetic artwork.

---

## 11. Implementation, build, and runtime status

| Item | Result | Status |
|---|---|---|
| C++ host harness | 396 checks passed, 0 failed | **VERIFIED** |
| Rust `af-core` tests | 30 passed, 0 failed | **VERIFIED** |
| Android native compile before version bump | 27 targets compiled and populated `prebuilt/arm64-v8a` | **VERIFIED**, but may embed 0.10.0 |
| 0.10.1 post-bump Android native rebuild | Interrupted at `[1/19]` | **UNVERIFIED / incomplete** |
| Current 0.10.1 APK/AAR Gradle assembly | Not completed after latest fixes | **UNVERIFIED** |
| 0.10.1 physical-device generation | Not run | **UNVERIFIED** |
| Reference-image public path | 22/26/28 packing defect | **BLOCKED** |
| Face identity reconstruction | No implementation | **BLOCKED** |
| Renderer stability around 26–29 FPS/AUTO transitions | Failure screenshots exist; corrected current build not device-tested | **UNVERIFIED** |
| Height/jaw/inseam repairs | Source and host-test work exists; latest Android device result absent | **UNVERIFIED on device** |

Existing APK/AAR/`.so` artifacts must not be represented as validated 0.10.1 until a clean rebuild reports the embedded engine version and passes device tests.

---

## 12. Final decisions

- Lock `/FavouritesMYLOVE/Love.jpg` as the only canonical face anchor.
- Retain `/FavouritesMYLOVE/love-2.jpg` as a weighted supporting reference, never an equal face source.
- Keep videos PARTIAL and derivatives non-independent.
- Exclude all unrelated redheads and mirror duplicates from identity fitting.
- Do not claim that the current photo path, prompt path, “IDENTITY” enum label, or “EXTRA_VIEW” enum label preserves identity.
- Fix and test the analysis schema before enabling reference generation.
- Finish Phase 1 Android build/device reliability before adding the offline identity stack.
- Keep every private reference on device/local storage; do not upload it to an external model or service.
