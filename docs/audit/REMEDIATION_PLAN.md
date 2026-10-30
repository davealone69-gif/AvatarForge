# Remediation plan

**Current-branch override (2026-10-30):** The GLES-only plan below is a historical snapshot.
The current renderer defaults to native Vulkan, with explicit GLES selection and bounded safe-GLES
fallback. The requested Vulkan target is `1.3.304`; the app encodes API 1.3 using `VK_API_VERSION_1_3`
(patch zero) and gates the loader/device separately. Shaders target `vulkan1.3`; the pinned NDK header
revision is 1.3.237. These are distinct from runtime loader/device reports. Local ARM64 compile/link
and shader checks pass; actual phone initialization/rendering remains unverified. See
[the restoration report](../VULKAN_RESTORE_2026-10-30.md) and
[the Vulkan target audit](VULKAN_TARGET_AUDIT.md).

## Done in the 2026-10-29 zero-trust pass

- Close the read/write ownership hole (was P0 #2): one `KernelAccess` fence around every mesh-mutating
  SDK entry point and every renderer read, plus render-thread blendshape application (AF-020).
- Fix the face panel that reset other channels on every edit (AF-021).
- Make the reference-image measurements reachable before a build, and state analysis failure
  explicitly (AF-022, AF-033).
- Clamp every renderer option before it can size a GPU allocation, in both the native sanitizer and the
  Kotlin settings codec; new host regression (AF-025).
- Remove composition-time state writes and make viewer flow collection lifecycle-aware (AF-024).
- Remove the dead SDK EXIF dependency, use the lifecycle artifact, fix the invalid R8 rule, and harden
  the image-buffer contract (AF-026, AF-027, AF-028).
- Correct the signing documentation, Rust version/repository metadata, and stale Vulkan text in
  current-facing docs (AF-023, AF-029, AF-030).
- Make the export panel's storage action and its joint/clip counts tell the truth (AF-031).

## Done in the previous checkpoint

- Remove Vulkan implementation, SPIR-V table, backend preference/fallback, JNI/Rust fields and link dependency; renderer is GLES-only.
- Replace backend-switch recovery with one bounded safe-GLES retry; keep old journal records decode-only.
- Add GLES-only linker guard for prebuilt/APK/AAB/AAR and remove stale tracked Vulkan-era binaries.
- Compact viewer header so the face/hands are not covered by default; camera gesture path unchanged.
- Fix world-axis palm blocks, reduce interdigital blending, raise garment minimum layer to two lattice cells, and stop the lower-garment arm cut from removing thigh cloth.
- Fix swapped eye material sides, asymmetric sclera extraction, the solid-ball cuirass and helmet-like hair bulk; add orbital clearance.
- Add focused model-detail host regressions and CI gates (33 checks).

## P0 — safety and truth

1. Device-retest the exact-head build with Vulkan default and explicit GLES during live walk/run clips; capture actual-backend diagnostics, matched paused/live PNGs, fallback/errors and thermal state. Camera is user-confirmed but exact-build evidence is still needed.
2. ~~Add a full engine read/write fence or ownership transfer so render-thread reads cannot race UI mutations/generation.~~ **DONE (AF-020)** — source + host compile proof; a device soak with clip playback, camera drags and expression edits still has to run.
3. **PARTIAL (AF-016):** diagnostic rung names and pass criteria exist, but the selectable rendered-scene ladder and per-rung submission/visible-output reporting are still missing.
4. **Production signing removed by owner direction:** no signed AAB/APK release or publication path is planned; retain only the debug test APK and explicitly unsigned compile-only AAB/AAR.

## P1 — mobile stability

5. Device-verify thermal cap and settings persistence; add current-session recovery advisor scoping.
6. ~~Add GPU timestamp queries where supported, with explicit unknown fallback.~~ **HOST-VERIFIED** —
   the GLES `EXT_disjoint_timer_query` ring is optional and non-blocking; actual device support,
   timings and the Vulkan device path remain unverified.
7. Soak test memory/thermal/FPS; add adaptive texture/LOD only after measurement.
8. Wire GLB import end-to-end with bounded copy, JNI import, viewer attach, and malformed-file tests; only then restore a share target.
8a. Decide the fate of `CustomizationSchemas` (AF-032): wire the JSON schema into the shipped editor, or
    delete it with its assets. Until then it is an unverified second source of truth.

## P2 — quality and supply chain

9. Matched device pixel comparison for eyes/hair/chest/hands/garments/moiré; then target lash geometry, strand-level hair, facial texture smudges and teeth cost.
10. Benchmark meshoptimizer/KTX2/ozz-style improvements; adopt only with measured device gains.
11. Add LICENSE, SBOM, SHA-pinned actions, wrapper checksum verification, and artifact provenance policy.
12. Keep Rust bindings in the acceptance gate: default workspace tests are verified; test the optional renderer feature's compile-fail lifetime guard and keep C/Rust telemetry layouts aligned.
13. ~~Replace or formally constrain raw pointer-valued JNI handles.~~ **DONE (AF-043, host-verified):** typed slot-generation tokens and synchronized shared leases reject malformed, wrong-kind, stale and repeated values; the full current host suite passes **438/438**, and the Android NDK build compiles the JNI libraries for both ABIs. Device lifecycle stress remains open.
14. **Source policy implemented:** Vulkan 1.3 target gates and `vulkan1.3` shader validation are in place. Capture the exact `AF_VK_CONTEXT` log on the SM-S921B, verify loader/device API ≥1.3, and run validation layers plus visible-frame tests; devices below the gate must be seen to fall back to GLES correctly.
