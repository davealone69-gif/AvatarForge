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
- Dead GLB VIEW intent filter removed so the app no longer advertises an unimplemented share target.

## Remaining risks

- Raw native handles are `Long` values; Kotlin lifetime discipline prevents use-after-free, not a malicious in-process caller.
- Renderer reads borrowed kernel buffers without a full read/write fence across UI mutation and render threads.
- No LICENSE; reuse/redistribution rights are undefined.
- CI actions are tag-pinned, not SHA-pinned; wrapper checksum authenticity is not CI-verified.
- No fuzzing campaign was run against the GLB/PNG importer in this environment.
