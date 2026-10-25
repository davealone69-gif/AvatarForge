# Writing input code — AvatarForge

*How to add or change anything on the input side of a generation: UI controls, spec fields,
prompt keywords, and reference images. Every step names the exact file that owns the change,
because the input pipeline is a chain of fixed contracts — break one link and the build either
throws at runtime or silently generates a different avatar.*

## 1. The pipeline at a glance

```
InputScreen.kt (Compose)
   │  prompt: String            imageA/imageB: Bitmap?        spec: AvatarSpec
   ▼
AvatarForge.begin() (MainActivity)
   │  AvatarEngine.generateFromText(prompt, spec)   /  .generateFromImage(...)
   ▼
AvatarEngine.start(request)
   │  NativeBridge.engineAnalyzeImages(...)   ← optional photos, direct ByteBuffers
   │  NativeBridge.engineReset(handle, spec.packInts(), spec.packFloats(), prompt)
   ▼
af_jni.cpp  (Java_com_avatarforge_sdk_internal_NativeBridge_*)
   │  kSpecIntCount = 7, kSpecFloatCount = 33  — sizes and order are checked
   ▼
af_spec  (native/include/afcore.h)
   │  af_engine_analyze_prompt() fills defaults from the prompt text
   │  af_engine_reset() honours explicit spec values over parsed ones
   ▼
anatomy → SDF field → isosurface → UV → texture bake → rig → blendshapes → GLB
```

Three input channels, each with one owner:

| Channel | UI owner | Wire owner | Native owner |
|---|---|---|---|
| Text prompt | `app/.../ui/InputScreen.kt` | `spec.prompt` (char[512]) | `native/src/af_prompt.cpp` |
| Reference photos | `InputScreen.kt` pickers | `NativeBridge.engineAnalyzeImages` (direct `ByteBuffer`s) | `native/src/af_image.cpp` |
| Explicit spec | `InputScreen.kt` spec panel | `AvatarSpec.packInts()/packFloats()` | `af_spec` in `native/include/afcore.h` |

## 2. The spec wire contract (read this before touching any field)

The spec crosses JNI as **two flat arrays in `af_spec` field order** — no structs, no
marshalling library. Three places must agree exactly, and they are checked at runtime when they
drift:

1. `native/include/afcore.h` — `struct af_spec` (the field order is the wire order).
2. `native/src/android/af_jni.cpp` — `kSpecIntCount = 7`, `kSpecFloatCount = 33`.
3. `sdk/.../AvatarSpec.kt` — `INT_FIELDS = 7`, `FLOAT_FIELDS = 33`, and the
   `packInts()/packFloats()/unpack()` sequences.

The leading int (`struct_size`) is a forwards-compatibility slot: the kernel's reader uses it to
accept specs built against a newer header. The JNI layer throws
`IllegalArgumentException` on a size mismatch, and `packFloats()` has a `check(i == FLOAT_FIELDS)`
that fails a test the moment packing drifts. **Do not remove those checks.**

Invariants that are part of the contract:

- **Seed reproducibility**: identical spec + seed ⇒ byte-identical geometry (harness test [6]).
  A new field that feeds the RNG or the geometry must keep this property.
- **`0 = derive`** for tape measurements, colours and overrides: a zero means "let the
  anthropometry solver decide". A new numeric field should follow the same convention (or say
  explicitly why it can't).
- **Safety gate**: `AvatarSpec.requireGeneratable()` rejects `Outfit.NONE` (nude) with any
  minor/teen prompt marker or `ageYears < 18`. It is called in the UI *and* re-checked below the
  UI before every build/export. New fields that widen the nude surface must pass through it.

## 3. Adding a new spec field (worked example: `skin_pore_scale`)

**Step 1 — native ABI.** `native/include/afcore.h`, `struct af_spec`: append the field at the end
of the float group (never in the middle — every middle insertion shifts the wire order):

```c
float     cloth_roughness;
float     gear_metalness;
float     skin_pore_scale;   /* NEW: 0..1, <0 => material default */
```

**Step 2 — JNI.** `native/src/android/af_jni.cpp`: bump `kSpecFloatCount` from 33 to 34 and
extend `fillSpecFromArrays()` with the matching read.

**Step 3 — Kotlin.** `sdk/.../AvatarSpec.kt`:
- add `val skinPoreScale: Float = -1f` to `AvatarSpec` (with a doc comment stating units and the
  `0/negative = default` convention),
- bump `FLOAT_FIELDS` to 34,
- append it in `packFloats()` (order = struct order) and read it back in `unpack()`.
  The trailing `check(i == FLOAT_FIELDS)` catches a forgotten slot.

**Step 4 — consumer.** Wherever the field affects the build (e.g. `af_texture.cpp` for a pore
scale) read it from the spec. If it is purely informational, store it in the GLB metadata like
`prompt`.

**Step 5 — UI (only if the user should set it).** Add a control in `InputScreen.kt` and plumb it
through `onSpecChange(spec.copy(skinPoreScale = it))`.

**Step 6 — verify.** `tools/build_host.sh` (kernel harness includes the packing-drift test), and
one device build for the UI.

## 4. Adding a prompt keyword

The prompt is parsed on-device by `native/src/af_prompt.cpp` into the same `af_spec` fields. The
UI promises the user that "build, stature, hair, outfit, footwear and colours are all real
fields" — so a new keyword must land on a real spec field, not a boolean that dies in the
solver.

1. Find the keyword table in `af_prompt.cpp` (sex/build/hair/outfit/footwear/colour lexicons).
2. Add the synonym(s); if the keyword implies a *range* (e.g. a height word), use the same
   "measured value with a confidence" pattern the other lexicons use — parsed values are
   *defaults* and any explicit spec value overrides them.
3. Add a case to the host prompt tests (`native/tests/host_tests.cpp`) asserting the parsed
   field. `tools/prompt_probe.cpp` prints what a given prompt resolves to, which is the fastest
   way to iterate.
4. If the app should *show* the new keyword in the UI copy (e.g. the spec panel label), that is a
   separate `InputScreen.kt` change.

## 5. Adding a UI input control

`InputScreen.kt` is a `LazyColumn` of `item { ... }` blocks; the pattern for every control is:

1. State lives in `MainActivity.kt` (`var spec by remember { mutableStateOf(AvatarSpec()) }` etc.)
   — the screen is stateless and gets `spec` + `onSpecChange`.
2. Render the control in `InputScreen.kt` (chips for enums — `FilterChip` — sliders for floats,
   text fields for prompt).
3. Route it: `onSpecChange(spec.copy(field = value))`.
4. Enums: new enum values must exist in **both** the Kotlin enum and the native `af_*` enum with
   the same numeric value; both sides have an `of(value)` that falls back to a default for
   unknown ints, so a version skew degrades instead of crashing — keep it that way.
5. If the control can produce an unsafe spec (e.g. picking `Outfit.NONE`), call
   `spec.requireGeneratable(prompt)` before enabling the build button, as the existing nude gate
   does.

## 6. Adding a reference-image role

Roles are ABI-stable: `af_image_role` in `afcore.h` (`IDENTITY, CLOTHING, FULL_BODY, EXTRA_VIEW`)
mirrored by `ImageRole` in `AvatarSpec.kt`. A new role is:

1. append to `af_image_role` (never renumber — the values are part of the ABI),
2. mirror in `ImageRole`,
3. decide what the analyser (`native/src/af_image.cpp`) reports for the role — the report is the
   `af_image_analysis` struct (`AF_IMAGE_ANALYSIS_FLOAT_COUNT = 28` floats, semantic field order;
   `ImageAnalysisPacking.kt` packs it and is checked against the native constant at runtime),
4. add a picker in `InputScreen.kt` only if the app exposes the role (roles exist for the
   contract even when the UI shows only A/B).

Photos travel as **direct `ByteBuffer`s** (w*h*channels bytes, RGB/RGBA, top-left origin, EXIF
orientation already corrected by the app) — never copy pixels into a `ByteArray` on this path;
a 34 MB mesh-scale photo moved twice is exactly the kind of thing the project refuses to do.

## 7. Verification checklist for any input change

- [ ] `bash tools/build_host.sh` — kernel harness (packing drift, prompt parsing, tape
      accuracy <0.5%, seed reproducibility, safety rejections).
- [ ] `bash tools/syntax_check/run.sh` — only if the change touched `native/` renderer/JNI
      (catches errors that CI's packaging used to miss).
- [ ] One device build from CI (the APK now compiles `native/` from source — verify the
      diagnostics panel's "Kernel" line shows the new ABI if you bumped `struct_size`).
- [ ] Reproducibility spot check: same prompt + seed ⇒ same GLB (compare file hashes).

## 8. Files to read, in order, before your first input change

1. `native/include/afcore.h` — `af_spec`, `af_image_role`, `af_image_analysis` (the ABI).
2. `sdk/.../AvatarSpec.kt` — the wire layout and the safety gate.
3. `native/src/android/af_jni.cpp` — `fillSpecFromArrays`, `engineReset`, `engineAnalyzeImages`.
4. `app/.../ui/InputScreen.kt` — the UI contract with the user (what the app promises).
5. `sdk/.../AvatarEngine.kt` — `generateFromText/generateFromImage/start` (the job lifecycle).
6. `native/src/af_prompt.cpp` — the prompt grammar.
