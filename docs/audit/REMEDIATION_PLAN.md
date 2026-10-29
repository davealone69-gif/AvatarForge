# Remediation plan

## P0 — safety and truth

1. Finish exact-head CI for fail-closed signing, unsigned AAB compile proof, shader compile/diff, JVM tests, ARM64 native build.
2. Device-retest camera and frame-error recovery on SM-S921B; capture diagnostics before/after.
3. Add a full engine read/write fence or ownership transfer so render-thread reads cannot race UI mutations/generation.
4. Implement diagnostic scene ladder and label submission vs visible output separately.

## P1 — mobile stability

5. Device-verify thermal cap and settings persistence; add current-session recovery advisor scoping.
6. Add GPU timestamp queries where supported, with explicit unknown fallback.
7. Soak test memory/thermal/FPS; add adaptive texture/LOD only after measurement.
8. Wire GLB import end-to-end with bounded copy, JNI import, viewer attach, and malformed-file tests; only then restore a share target.

## P2 — quality and supply chain

9. Matched pixel comparison for moiré; geometry fixes for hands/eyes/hair/garments.
10. Benchmark meshoptimizer/KTX2/ozz-style improvements; adopt only with measured device gains.
11. Add LICENSE, SBOM, SHA-pinned actions, wrapper checksum verification, and prebuilt provenance policy.
12. Build/test Rust bindings and mirror renderer telemetry through the C ABI.
