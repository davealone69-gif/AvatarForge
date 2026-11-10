# GLB import pipeline integration

## Purpose

AvatarForge remains the standalone engine/SDK and test lab. This integration exposes the existing native GLB importer through the Android SDK and connects the imported engine buffers to the existing custom renderer. It does not create a second app or replace the renderer.

## Existing native machinery

- `native/src/af_glb_read.{h,cpp}`: bounded GLB 2.0 reader, accessor decoding (including sparse accessors), shared vertex pools, index validation, node/skin/animation metadata, metallic-roughness material data and embedded PNG decoding.
- `native/src/af_engine.cpp`: transactional `af_engine_import_glb` / `af_engine_import_glb_from_memory` entry points. Import is assembled beside live state and swapped only after successful validation.
- `native/src/render/af_gl.cpp` and `af_vk.cpp`: upload the engine's vertex/index/material/texture buffers to the active GPU backend.
- `native/src/af_glb.cpp`: self-contained GLB 2.0 exporter.
- `native/tests/host_tests.cpp`: importer round-trip and hostile-input regression coverage already present in the native host harness.

## Android SDK path added

1. `NativeBridge.engineImportGlb(handle, path)` declares the JNI entry point.
2. `af_jni.cpp` forwards it to `af_engine_import_glb` through the existing guarded exception/error path.
3. `AvatarEngine.importGlb(File)` validates the input file, imports it and returns the real engine counters.
4. `AvatarRenderer.importGlb(engine, file)` performs the import and reattaches/invalidates the engine buffers on the renderer's owning thread.

Example:

```kotlin
// Call on the renderer's owning thread, with exclusive ownership of the engine handle.
val counts = renderer.importGlb(engine, selectedGlbFile)
// counts = [vertices, indices, submeshes, joints, materials]
```

A Storage Access Framework URI must first be copied to an app-readable cache file; the SDK method intentionally accepts a File and does not pretend a content:// URI is a filesystem path.

## Explicit support boundary

The current engine adoption path applies static renderable geometry, indices, metallic-roughness material values and embedded PNG base-colour textures. The lower-level reader carries additional node, skin and animation metadata, but the engine import path does **not** activate a foreign skeleton, foreign animation clips or morph-target playback. It warns when foreign rig/clip data is skipped and leaves the imported model unrigged until a compatible rig is explicitly applied. Do not claim full foreign-avatar animation support from this import path.

The existing renderer consumes the engine's flattened geometry buffers. Full multi-instance scene-graph rendering with per-node transforms, imported skin deformation, morph evaluation and animation playback remains a separate integration phase; it is not silently inferred from the parser's ability to read metadata.

## Verification

- Source changes are committed on `work/glb-measurement-pipeline`.
- GitHub Actions status for the latest commit has not yet reported a run.
- Android compilation, APK assembly, installation and physical-device rendering are **UNVERIFIED** until actual evidence is collected.
- Existing native host-test claims in `STATUS.md` are historical repository evidence, not a fresh test run of these SDK/JNI additions.
