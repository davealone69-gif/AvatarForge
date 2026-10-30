# Architecture audit

Status: **PARTIAL** — source trace is real; Android/device execution remains CI/device-gated.

## Modules and ownership

| Layer | Real entry point | Ownership / threading | Audit result |
|---|---|---|---|
| App UI | `app/.../MainActivity.kt`, `ui/ViewerScreen.kt` | Compose main thread; owns one `RenderSurfaceView` | REAL + VERIFIED by source; camera device proof still missing |
| SDK | `sdk/.../AvatarEngine.kt`, `AvatarForge.kt` | Process singleton; native kernel handle | REAL + VERIFIED by source: generation is serialised and SDK mesh mutation/render reads are fenced by `internal/KernelAccess.kt`; device stress proof still missing |
| Render view | `sdk/.../render/RenderSurfaceView.kt` | Dedicated `HandlerThread`; Surface create/resize/destroy generation tokens | REAL + VERIFIED by source and host sequencing tests; physical Surface lifecycle unverified |
| JNI | `native/src/android/af_jni.cpp` | Translates Kotlin arrays/direct buffers; callback global refs | PARTIAL: image sizes/counts and GLB ownership are guarded; callback exception cleanup added; raw pointer-valued handles still lack stale/double-destroy validation |
| Kernel | `native/src/af_engine.cpp` + geometry/rig/UV/texture/GLB | Thread-affine handle; borrowed pointers valid until next mutation | REAL + VERIFIED by host harness; SDK calls are fenced, while direct C/JNI callers must still obey the thread-affine ABI contract |
| Renderer | `native/src/render/af_render.cpp` → `af_vk.cpp` (default native Vulkan) or `af_gl.cpp` (GLES 3.1 fallback/explicit choice) | One renderer per Surface, one owning thread | PARTIAL: Vulkan API 1.3 loader/device gates, extension/queue/format checks, GLES fallback selector and ARM64 link are host/build-verified; app pixels/device runtime remain unverified |
| Rust | `rust/af`, `rust/af-core`, `rust/af-sys` | Optional RAII binding; not a Gradle module | Default workspace tests are locally verified; optional renderer feature mirrors telemetry and statically asserts layouts. Renderer-to-engine lifetime is now tied in safe Rust; GPU execution remains unverified |

## Critical execution paths

1. Input/generation: image picker → cancellable IO decode/analysis → correction card → UI → `AvatarEngine.generate` → JNI `engineReset/engineRunAll` → kernel stages → `engineExportGlb` → `EngineResult`.
2. Viewing: `EngineResult` → `RenderSurfaceView.attachEngine(engine.nativeHandle)` → JNI `rendererAttachEngine` → backend uploads borrowed kernel buffers → frame loop.
3. Camera: Compose gesture → `CameraGesture` → `CameraSettings` → `RenderSurfaceView.applyCamera` → JNI `rendererSetCamera` → `CameraState` → backend UBO.
4. Diagnostics: MENU → `DiagnosticsDialog` → device report + renderer stats + recovery journal → copy/SAF export.
5. Recovery: frame/startup/resize exception → classifier → bounded safe settings/backend attempt → submission probe. Probe is **not** pixel proof.

## Architectural defects found

- The SDK's `KernelAccess` fence serializes its renderer reads and mesh mutations, but direct native/JNI callers can still violate thread affinity; raw JNI handle validation remains an open boundary.
- `EngineResult` is a file plus metadata, not an immutable native snapshot. Exporting an older result rebuilds the single-slot engine; this is honest but expensive and can surprise callers.
- The native GLB importer exists, but there was no JNI/UI import path. The manifest previously advertised a dead `ACTION_VIEW` target; that advertisement was removed.
- Renderer C ABI and Rust `RenderStats` expose frame-attempt/present counters with matching size assertions; the POD declarations are still private to implementation files rather than one public shared header, so broader ABI/layout coverage remains a follow-up.
