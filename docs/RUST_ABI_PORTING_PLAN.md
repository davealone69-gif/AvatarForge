# AvatarForge Rust Porting Plan — Demand-Led Input Safety

## Baseline and decision rule

The source audit at base commit `79e6898801ef3d2e6970aaeef4e7cd68031633ab` inventories 47 public C ABI functions: 15 Rust-core exports are present and 32 are missing. These counts describe symbol coverage; they are not a mandate to implement every export. An `af-sys` import or safe `af` wrapper is not a Rust-core implementation, and neither proves native/Rust behavioral parity. Keep ABI version 1 and do not change C signatures, struct layouts, ownership, or native engine behavior as part of input hardening.

The existing worktree also contains the earlier, separate `af_engine_set_gpu_hint` Rust-core export and `tools/check_gpu_hint_parity.py`. Preserve that work; it is a narrow ABI implementation, not evidence that the rest of the Rust library is a drop-in replacement.

Prioritize work by a concrete consumer and its failure risk:

1. Validate caller-owned data at a safe Rust boundary before allocation or FFI dispatch, using native limits and preserving native parsing/analysis as the authority.
2. Close documented semantic gaps in existing Rust-core exports before describing Rust as an alternative runtime. In particular, cover bounded `struct_size` handling in create/reset, reset and cancellation invalidation, prompt dirty-state behavior, guarded-call error clearing, and clip metadata edge cases (`native/src/af_engine.cpp` versus `rust/af-core/src/engine.rs`; see the symbol audit for exact locations).
3. Implement Rust-core producer stages only when a product flow requires the Rust implementation. Keep dependency order and ownership explicit: geometry before UV/textures, rig before posing/animation, and real populated buffers before borrowed-view exports. Do not add getters by fabricating state.
4. Treat the remaining export inventory as a compatibility backlog, not as a completion percentage or a sequence to exhaust without a consumer.

## Current consumers and implementation boundaries

The production Android image path is Kotlin/JNI: `app/src/main/java/com/avatarforge/app/ReferenceImageAnalysis.kt` dispatches decoded references to `AvatarEngine`, and `native/src/android/af_jni.cpp` validates count, dimensions/channels/64 MiB input size, role range, and direct-buffer capacity before calling `af_engine_analyze_images`. The documented Android feature is still coarse classical image cues, not identity reconstruction (`docs/audit/FEATURE_TRACEABILITY.md`).

The Rust safe API has `ImageRef` and `Engine::analyze_images` in `rust/af/src/engine.rs`, but the only repository call found is the ignored two-image integration test in `rust/af/tests/engine.rs`. GLB import is present in `af-sys` as raw native bindings; it has no prior safe Rust method, and the application feature trace says import is missing end-to-end (`docs/audit/FEATURE_TRACEABILITY.md`). The safe wrappers added in this increment therefore improve the Rust API boundary; they do not integrate a new Android product flow and do not implement Rust-core ABI exports.

## First bounded slice — safe image and GLB inputs

### Reference images

`af::Engine::analyze_images` and `ImageRef::as_raw` now check the native contract before building the FFI array: 1–4 images; positive dimensions no larger than 16,384 on either axis; RGB or RGBA packed pixels; checked byte-count arithmetic; no more than 64 MiB per input; and a pixel slice at least as long as the packed image. The role must be one of the four public `af_image_role` values, matching the Android JNI boundary. Extra slice bytes remain allowed because the C descriptor has no length field and only the packed prefix is read. Pixel bytes remain borrowed only for the synchronous native call.

These are argument checks, not an image-analysis port. Native `af_engine_analyze_images` and `analyseImage` still decide whether a valid image can be analysed, skip unanalysable inputs, associate results with roles, mutate engine analysis state, and report native errors. Do not duplicate segmentation, color analysis, or role-to-spec behavior in Rust.

### GLB files and buffers

`af::Engine::import_glb` and `import_glb_bytes` are safe adapters over the existing `af-sys` imports. The path adapter rejects empty/NUL-containing paths; the memory adapter rejects empty data and buffers larger than the native 128 MiB import limit. The byte slice is borrowed only during the synchronous call. Both methods delegate all file access, GLB structure/version checks, supported-subset decisions, detailed error reporting, and engine-state updates to native code.

The native reader supports GLB v2 and a bounded subset documented in `native/src/af_glb_read.h`: it does not fetch external `.bin` or data URIs, rejects required extensions it does not implement, decodes embedded PNG, and warns/skips JPEG. The engine additionally rejects imports exceeding 2,000,000 vertices. It intentionally does not adopt a file's skeleton or animation; successful import clears incompatible generated state, while rejected imports preserve the live engine state (`native/include/afcore.h` and `native/src/af_engine.cpp`). The new Rust methods are wrappers, not Rust GLB parser/importer implementations and not new Rust-core ABI exports.

**Native follow-up risk:** `native/src/af_glb_read.cpp` declares `MAX_JSON_BYTES` as 16 MiB, but the current parser source does not use that constant to reject a larger JSON chunk. This slice enforces only the actual total-file 128 MiB limit, rather than silently adding a stricter format rule in Rust. Review/enforce the JSON chunk limit separately as a native hardening change with its own tests and compatibility decision.

## Verification gates

For this slice, test the safe boundary directly at minimum/maximum image count, dimensions, channels, packed-byte cap, short pixel storage, invalid role, and empty/maximum/oversized GLB buffer lengths. Run the `af` package tests and formatting check; run the `af-core` tests plus the native-vs-Rust GPU-hint parity script to ensure the preserved earlier increment still behaves as intended. Report exact commands and results.

A passing host test does not prove Android packaging, the production Kotlin/JNI image flow, native GLB import on device, or physical-device behavior. For any future Rust-core ABI implementation, first run focused differential tests against the native library, then inspect the built `cdylib` exports and run applicable ABI conformance checks. Keep Android/device verification separate; do not claim complete 47-symbol parity unless every relevant export and semantic contract has evidence.

## Validation recorded for this slice

Manual smoke validation used an owner-supplied private GLB with the host C++ library; its local path, identity, and model measurements are intentionally omitted. The path and byte-input adapters imported successfully, and malformed non-empty bytes were rejected without changing already imported geometry. This is API/state-preservation validation only, not a proportion assessment or visual/device acceptance.

The permanent `rust/af/tests/glb_import.rs` test rejects malformed bytes through the native-linked safe wrapper and confirms that a fresh engine remains empty. Unit tests cover image count 1–4, RGB/RGBA layout, accepted 16,384-pixel dimension and exact 64 MiB packed input boundary, invalid dimensions/channels/roles, short storage, allowed extra bytes, and GLB sizes 0, 1, exactly 128 MiB, and 128 MiB + 1. The oversized buffer is tested at the deterministic validation helper, without allocating or dispatching a 128 MiB + 1 buffer to native code.

With `AF_CORE_LIB_DIR=<build-host-dir>` and `LD_PRELOAD=<build-host-libafcore.so>`, `cargo test --workspace --offline -- --nocapture` passed: 13 `af` unit tests, the malformed-GLB integration test, and 32 `af-core` tests; the two existing full-avatar integration tests remained ignored. `cargo build --offline -p af-core` and `python3 tools/check_gpu_hint_parity.py <native-lib> <rust-lib>` passed. `cargo fmt --all -- --check` did not pass: rustfmt reported widespread formatting drift in multiple existing Rust files, including files outside this slice. No workspace-wide reformat was applied to avoid altering preserved work. No Android build/device or visual acceptance is claimed.

## Explicitly out of scope for this increment

No image-analysis or GLB parsing/import algorithm is copied into Rust; no new Rust-core import exports are added; no C ABI, ABI version, or native behavior changes; and no attempt is made to implement all 47 exports. The product demand for a production Rust runtime remains unproven, so further Rust-core ports should be tied to a concrete consumer and bounded testable behavior.
