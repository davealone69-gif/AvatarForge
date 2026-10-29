# Architecture audit

Status: **PARTIAL** — source trace is real; Android/device execution remains CI/device-gated.

## Modules and ownership

| Layer | Real entry point | Ownership / threading | Audit result |
|---|---|---|---|
| App UI | `app/.../MainActivity.kt`, `ui/ViewerScreen.kt` | Compose main thread; owns one `RenderSurfaceView` | REAL + VERIFIED by source; camera device proof still missing |
| SDK | `sdk/.../AvatarEngine.kt`, `AvatarForge.kt` | Process singleton; native kernel handle | PARTIAL: generation now serialised on one engine thread; UI-thread parse/export and render-thread reads still need a single ownership/lock discipline |
| Render view | `sdk/.../render/RenderSurfaceView.kt` | Dedicated `HandlerThread`; Surface create/resize/destroy generation tokens | REAL + VERIFIED by source and host sequencing tests; physical Surface lifecycle unverified |
| JNI | `native/src/android/af_jni.cpp` | Translates Kotlin arrays/direct buffers; callback global refs | PARTIAL: bounds checks exist; callback exception cleanup added; raw handle validity still relies on Kotlin lifetime |
| Kernel | `native/src/af_engine.cpp` + geometry/rig/UV/texture/GLB | Thread-affine handle; borrowed pointers valid until next mutation | REAL + VERIFIED by host harness; concurrent renderer reads are not fully fenced |
| Renderer | `native/src/render/af_render.cpp` → `af_vk.cpp` / `af_gl.cpp` | One renderer per Surface, one owning thread | PARTIAL: frame errors now propagate; Vulkan device path unverified |
| Rust | `rust/af`, `rust/af-core`, `rust/af-sys` | Optional RAII binding; not a Gradle module | UNVERIFIED: no Rust toolchain in this environment; renderer C ABI telemetry not yet mirrored |

## Critical execution paths

1. Generation: UI → `AvatarEngine.generate` → JNI `engineReset/engineRunAll` → kernel stages → `engineExportGlb` → `EngineResult`.
2. Viewing: `EngineResult` → `RenderSurfaceView.attachEngine(engine.nativeHandle)` → JNI `rendererAttachEngine` → backend uploads borrowed kernel buffers → frame loop.
3. Camera: Compose gesture → `CameraGesture` → `CameraSettings` → `RenderSurfaceView.applyCamera` → JNI `rendererSetCamera` → `CameraState` → backend UBO.
4. Diagnostics: MENU → `DiagnosticsDialog` → device report + renderer stats + recovery journal → copy/SAF export.
5. Recovery: frame/startup/resize exception → classifier → bounded safe settings/backend attempt → submission probe. Probe is **not** pixel proof.

## Architectural defects found

- The kernel advertises thread affinity, but the renderer reads kernel buffers on the render thread while UI/SDK calls can mutate the same handle. Serial generation fixes job-vs-job races, not all renderer-vs-mutation races.
- `EngineResult` is a file plus metadata, not an immutable native snapshot. Exporting an older result rebuilds the single-slot engine; this is honest but expensive and can surprise callers.
- The native GLB importer exists, but there was no JNI/UI import path. The manifest previously advertised a dead `ACTION_VIEW` target; that advertisement was removed.
- Renderer C ABI (`af_render_capi.cpp`) and Rust `RenderStats` do not yet expose the new frame-attempt/present counters. JNI/Kotlin do. This is a documented binding gap, not a silent claim of parity.
