# Feature traceability

| Feature | UI entry | SDK/JNI | Native execution | Verification status |
|---|---|---|---|---|
| Text → avatar | InputScreen generate | `AvatarEngine.generateFromText/generate` | `af_engine_analyze_prompt`, `af_engine_run_all` | PARTIAL: host kernel and exact-head Android APK CI run 41002485082 passed; installed-device generation and visual confirmation remain unverified |
| Image cues → avatar | InputScreen images | IO-dispatched decode/analysis, cancellable and guarded against stale selection; `analyseImages`, `specFromImages` | bounded `af_engine_analyze_images` | PARTIAL: coarse classical cues only; decode/dispatch regression tests added; not identity reconstruction |
| GLB export | Viewer export | `engineExportGlb`, SAF copy | `af_engine_export_glb` | REAL + VERIFIED by user GLB files and validator; current-head device export pending |
| 3D viewer | ViewerScreen surface + native Vulkan default | `RenderSurfaceView`, `rendererFrame`, actual-backend diagnostics | Vulkan API 1.3 loader/device gates, enumerated required extensions, capability-checked queues/formats/limits; bounded GLES 3.1 fallback | PARTIAL: host selector/version tests, shader validation and ARM64 compile/link pass; AvatarForge Vulkan pixels/device runtime unverified |
| Camera orbit/zoom/panel | ViewerScreen gestures/chips/sliders | `CameraGesture`, `rendererSetCamera` | `CameraState` UBO | PARTIAL: JVM gesture tests + CI; device movement unverified |
| Animation clips | Viewer pose/clip controls | `rendererSetClip`, `engineApplyClip` | clip sampling/skinning | PARTIAL: device reports show triangles; clip correctness needs visual matrix |
| Blendshapes | Viewer expression controls | `engineApplyBlendshapes` | morph bake/apply | PARTIAL: 52 morphs in GLB census; visual quality unverified |
| Diagnostics/self-test | MENU → Self diagnostics | device report + stats + journal | renderer stats/info | REAL + VERIFIED present in source; current device interaction pending |
| Safe recovery | Diagnostics button | `RendererRecoveryPolicy` | backend recreate/options | PARTIAL: bounded policy tests; visible recovery unverified |
| Settings persistence | Settings panel | `SettingsStore` | none | PARTIAL: codec tests added; Android SharedPreferences proof pending |
| Thermal adaptation | Automatic | `AndroidThermalMonitor` | quality cap via options | PARTIAL: policy tests added; device thermal proof pending |
| GLB import | **removed dead VIEW filter** | no JNI import method | `af_engine_import_glb*` exists | MISSING end-to-end; native limits added |
| AI/LLM avatar generation | none | none | none | MISSING by design; stability path is no-AI |
| Network/server | none | none | none | MISSING by design; manifest has no INTERNET |
