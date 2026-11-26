# Rust Generation Gateway Migration Design

**Status:** proposal only. This document does not migrate generation or alter the current SDK/JNI/native path.

## Recommendation

Add a separate, versioned Rust command gateway for Android generation, and keep the existing `afcore` ABI v1 and SDK/JNI route as the compatibility baseline. The gateway should coordinate one generation request through the existing native C ABI; C++ remains the implementation of geometry, UV unwrap, texture baking, rigging, blendshapes, animation, and GLB writing.

The first gateway should coordinate **request-level operations**—configure/reset, forward the already-normalized SDK request and any existing reference images, call the native pipeline, and collect copied results. It should call `af_engine_run_all()` rather than reimplementing its dirty-stage rules. Those rules are private to `native/src/af_engine.cpp`; duplicating them in Rust would create two competing pipeline definitions.

This is a separate architecture proposal, not part of the current repairs. Do not change the Android generation route, `AF_ABI_VERSION`, export ownership, the governor checkpoint, or import functionality under this proposal.

## What exists today

The Android SDK creates and owns candidate engine handles, serializes generation on a single worker, handles job status/cancellation and active-engine promotion, and exports the result. `NativeBridge` calls `native/src/android/af_jni.cpp`, which resolves opaque registry tokens and forwards to the C ABI. `af_engine_run_all()` in native C++ executes the dirty stages in order and emits native progress/cancellation events. The C ABI documents engine-owned borrowed views and thread-affine handles in `native/include/afcore.h`.

The Rust `af::Engine::run_all()` is a safe wrapper around the same native `af_engine_run_all()` function; it does not make the Android SDK use Rust, and it is not a unified generation gateway. Rust’s `af-core` exports are a separate, partial implementation and should not be treated as a substitute for the native processing library. The current native CMake target also does not link a Rust gateway.

Relevant evidence: [`AvatarEngine.kt`](../sdk/src/main/java/com/avatarforge/sdk/AvatarEngine.kt), [`NativeBridge.kt`](../sdk/src/main/java/com/avatarforge/sdk/internal/NativeBridge.kt), [`af_jni.cpp`](../native/src/android/af_jni.cpp), [`afcore.h`](../native/include/afcore.h), [`af_engine.cpp`](../native/src/af_engine.cpp), and [`rust/af/src/engine.rs`](../rust/af/src/engine.rs).

## Proposed boundary

```text
Public Kotlin SDK / JobHandle / active-vs-candidate policy
        │ existing JNI token only; no native pointer exposed to Kotlin
        ▼
JNI adapter resolves token through the existing handle registry
        │ short-lived borrowed af_engine*; one worker thread at a time
        ▼
Versioned Rust generation command ABI (request coordination + typed result)
        │ existing af-sys bindings; no retained pointers or native allocations
        ▼
Existing afcore C ABI v1 → C++ stage graph and specialized processors
        │ same engine-owned buffers and existing export implementation
        ▼
Renderer attaches the same promoted candidate handle as today
```

Give the gateway its own namespace and version, for example `AFG_ABI_VERSION` and `afg_v1_run`. Do not bump or silently repurpose `AF_ABI_VERSION` 1. Define fixed-layout `afg_request_v1` and `afg_result_v1` contracts. Each carries `struct_size`, `abi_version`, and a nonzero fixed-width `uint64_t request_id`; the caller allocates output structs and declares their size, and the callee validates version and size before doing work. The ID is unique among outstanding requests in the gateway process, is never reused while active, and is echoed in progress, cancellation, result, and error events. A duplicate active ID is rejected deterministically. Every field has an explicit fixed-width C representation; no Rust-specific layout or exceptions cross the boundary. Keep the current `af_spec` and native enums canonical for core semantics; generate/check any gateway manifest rather than maintaining a second hand-edited ABI inventory.

The Rust entry point should be synchronous on the SDK’s existing worker thread. JNI resolves the existing engine token and passes a **borrowed** `af_engine*` only for the duration of the call. Rust must not store, destroy, transfer, or expose that pointer; the existing registry/SDK ownership and renderer attachment remain unchanged. Request and image bytes are borrowed only until the synchronous call returns. The call must not retain Kotlin `ByteBuffer` addresses.

Kotlin remains the public API and owns `JobHandle`, candidate/active-engine promotion, UI state, and the current export call. The gateway takes over the sequence currently performed around reset, memory budget, optional image analysis, native pipeline invocation, and stats collection. Preserve existing prompt/image-to-spec normalization and input acceptance exactly in the first increment; do not silently move policy or change the supported image flow. No GLB/Android import is added.

## Errors, callbacks, and synchronization

Use an output `afg_error_v1` with `struct_size`, `request_id`, a stable gateway error code, underlying `af_result`, failing stage/operation, retryability, and a caller-owned fixed-size message buffer. Do not return a Rust `String`, heap pointer, or a pointer into `af_engine_last_error()`. Map native failures without losing the native code/message; keep the old C ABI error contract intact. Treat non-negative prompt/image analysis counts as successful values, not error codes.

Cancellation is a request-scoped operation keyed by `request_id`. It is thread-safe to request from the UI/job thread and cooperative while native work runs: queued work can be cancelled before entering C++, while running work stops only at native safe points. It does not pause or resume a stage. Cancellation racing with completion yields exactly one terminal result: either success/failure wins and a later cancel reports already-terminal, or cancellation wins and the request reports cancelled; cancellation/failure never promotes the candidate. Reuse the existing native callback path for progress, logs, and cancellation. Callback context and request memory stay valid until return; callbacks are serialized on the worker thread and must not re-enter the gateway. No Rust-created background thread is introduced. The existing `KernelAccess` fence remains the synchronization boundary for every handle also observed by rendering. A candidate build must remain isolated from the active renderer and is destroyed once by its existing owner.

The gateway must not change the current ownership rules for exported bytes/files or engine-backed borrowed views. In particular, EXPORT-01 ownership remains untouched until separately measured and approved; the existing exporter/deallocator and renderer handoff remain the control.

## Compatibility test gate

Build a differential harness that runs the same fixed-seed request through (A) the current SDK/JNI/native path and (B) the gateway path, using fresh candidate engines sequentially. Do not dual-run generation in production: builds are expensive, and production shadowing would add memory/thermal cost. Host tests can compare C ABI behavior; an Android SDK integration/smoke test is still required to prove the JNI token, callback, and `KernelAccess` wiring.

Before switching any default, the suite must cover:

| Area | Required comparison |
|---|---|
| ABI | Gateway version negotiation, struct sizes/offsets, symbol presence, error layout, and unchanged `afcore` ABI v1 manifest. Reject unknown versions/sizes without entering native work. |
| Requests | Same valid/invalid spec decisions, defaulting, memory-budget handling, prompt behavior, reference-image roles/counts/dimensions/buffer lengths, and accepted/rejected input result. Use existing input limits; do not add an import case. |
| Success result | Effective spec, vertex/triangle/submesh/joint/blendshape counts, texture bytes, region names, and native statistics agree. Exclude wall-clock timings from exact equality. Compare GLB structure and payload semantics; require byte-for-byte file hashes only after confirming the exporter has no volatile fields. |
| Events | Same stage sequence, stage IDs, terminal status, callback thread, cancellation visibility, and error/log delivery. Compare event order and values, not callback wall-clock timing. |
| Failure/cancel | Invalid arguments, native stage failure, bounded-budget failure, export I/O failure, cancellation before work and at native safe points, and ABI mismatch produce equivalent public SDK outcomes. Inject failures in the host harness rather than exhausting a physical device. |
| Lifecycle/races | Candidate failure/cancellation leaves the active engine attached; successful activation and deferred retirement happen once; release waits for in-flight work; no handle is used concurrently across worker/render threads. Use sanitizers or race-focused host tests where available. |
| Ownership | Existing engine pointer lifetime, borrowed-view invalidation, memory export/deallocator, file export, and renderer attachment remain unchanged. The tests document current ownership; they do not propose a new owner or zero-copy export path. |

The host differential suite should link the Rust gateway to the same native `libafcore` build used by the baseline and use deterministic inputs. Add an Android integration test that switches between legacy and gateway implementations only in test configuration, runs each sequentially, and compares SDK-visible `JobStatus`, `EngineResult`, callbacks, and export semantics. Existing unit tests alone do not establish JNI or device parity.

## Migration sequence and stop conditions

1. **Design only (now):** keep the current Android path and all current edits unchanged; implementation is a separate project after the current boundary repairs.
2. **ABI and host harness:** add a new gateway namespace and host-linkable Rust target; verify the generated gateway manifest and differential tests. Do not change `afcore` v1 or its manifest.
3. **JNI adapter, test-only selection:** resolve the existing token in JNI and invoke Rust synchronously under the existing SDK fence. Keep the legacy path as the default and run Android integration checks.
4. **Parity review:** require the compatibility matrix to pass on host and Android. Investigate any behavioral difference rather than masking it with fallback. Measure performance/memory before making resource or performance claims.
5. **Separate cutover decision:** only after evidence, explicitly approve switching the SDK default. Keep rollback to the legacy path for a defined period; do not automatically run both implementations for one request.

A future proposal would be needed to move the stage dependency graph itself into Rust. That requires a versioned public stage/dirty-state contract and compatibility evidence; do not copy the current private dirty-bit values into Rust. Likewise, changing EXPORT-01 ownership, adding Android import, or altering the governor is outside this design.

## Verification status

This artifact records a design only. No generation, JNI, native, Rust, ABI, export, governor, or ownership code was changed, and no tests were run for a gateway that does not yet exist. The current Android SDK/JNI/native generation path remains the production path.
