# Remediation plan

## Done this checkpoint

- Remove Vulkan implementation, SPIR-V table, backend preference/fallback, JNI/Rust fields and link dependency; renderer is GLES-only.
- Replace backend-switch recovery with one bounded safe-GLES retry; keep old journal records decode-only.
- Add GLES-only linker guard for prebuilt/APK/AAB/AAR and remove stale tracked Vulkan-era binaries.
- Compact viewer header so the face/hands are not covered by default; camera gesture path unchanged.
- Fix world-axis palm blocks, reduce interdigital blending, raise garment minimum layer to two lattice cells, and stop the lower-garment arm cut from removing thigh cloth.
- Add focused model-detail host regressions and CI gates.

## P0 — safety and truth

1. Device-retest the GLES-only build and capture diagnostics before/after; camera is user-confirmed but exact-build evidence is still needed.
2. Add a full engine read/write fence or ownership transfer so render-thread reads cannot race UI mutations/generation.
3. Implement diagnostic scene ladder and label submission vs visible output separately.
4. Supply production signing credentials to unblock signed AAB/APK release.

## P1 — mobile stability

5. Device-verify thermal cap and settings persistence; add current-session recovery advisor scoping.
6. Add GPU timestamp queries where supported, with explicit unknown fallback.
7. Soak test memory/thermal/FPS; add adaptive texture/LOD only after measurement.
8. Wire GLB import end-to-end with bounded copy, JNI import, viewer attach, and malformed-file tests; only then restore a share target.

## P2 — quality and supply chain

9. Matched device pixel comparison for hands/garments/moiré; then target remaining eye/hair detail defects.
10. Benchmark meshoptimizer/KTX2/ozz-style improvements; adopt only with measured device gains.
11. Add LICENSE, SBOM, SHA-pinned actions, wrapper checksum verification, and artifact provenance policy.
12. Build/test Rust bindings on a Rust toolchain; C/Rust renderer telemetry layout is now mirrored and statically asserted.
