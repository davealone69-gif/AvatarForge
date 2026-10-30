# Security / privacy audit

Status: **PARTIAL**.

## Verified source properties

- App manifest requests no INTERNET, network state, storage, or camera permission.
- Single exported launcher Activity; FileProvider is non-exported and grant-uri based.
- `allowBackup=false`; generated files live in app-scoped storage; export uses SAF.
- No HTTP client, WebView, analytics, or AI runtime found in app/SDK source.
- Recovery journal is local and bounded; report export is user-initiated.

## Fixes

- Release signing is fail-closed. CI unsigned AAB is explicitly labelled and never published. Tag release requires production secrets and verifies APK/AAB signatures.
- JNI callback exceptions are described/cleared; a broken cancellation hook fails closed by requesting cancellation.
- Native GLB/PNG import limits added: 128 MiB GLB, 16 MiB JSON, 2 M vertices, PNG 8192²/256 MiB decoded/64 MiB IDAT. These are defence-in-depth; the user-facing import path is still missing.
- Native image analysis now rejects non-positive/oversized dimensions, unsupported channels, overflowing or over-cap packed buffers, and image counts beyond the four declared roles; JNI validates parallel arrays/direct-buffer capacities. GLB byte-export allocation now has RAII cleanup on all return paths.
- `af_spec.struct_size <= 0` now fails with `AF_ERR_ABI_MISMATCH` before a full-struct copy; the native host harness passes the regression. The generated ABI manifest was refreshed for the public `AF_MAX_REFERENCE_IMAGES` constant.
- Dead GLB VIEW intent filter removed so the app no longer advertises an unimplemented share target.

## Remaining risks

- JNI still trusts raw pointer-valued `jlong` engine/renderer handles. The normal Kotlin wrappers keep these internal and serialize ordinary teardown, but stale/double-destroy values are not checked by a native token registry; this remains an open in-process memory-safety risk.
- SDK renderer reads and mesh mutations are fenced by `KernelAccess` (AF-020); direct C/JNI callers still have to obey the thread-affine contract, and device stress has not been run.
- No LICENSE; reuse/redistribution rights are undefined.
- CI actions are tag-pinned, not SHA-pinned; wrapper checksum authenticity is not CI-verified.
- No fuzzing campaign was run against the GLB/PNG importer in this environment.
- Rust `Renderer` now ties an attached engine borrow to the renderer type; the optional-feature compile-fail regression is pending execution.
