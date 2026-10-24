# Architecture audit

Scope: every module in the shipped pipeline and the directories that sit outside it.
Evidence is from source inspection at commit `1aa73de` and the commands noted per section.

## 1. Module map (what is actually built)

```
:app (Compose UI, single Activity, no business logic beyond orchestration)
  └─ :sdk (Kotlin façade, JNI bridge, renderer wrapper, diagnostics, resource governor)
        ├─ libavatarforge.so  JNI (native/src/android/af_jni.cpp, 1026 lines)
        ├─ libafcore.so       generation kernel + GLB read/write (native/src/*.cpp)
        └─ libafrender.so     Vulkan + GLES renderers, budget controller (native/src/render)
```

- Gradle includes exactly `:sdk` and `:app` (`settings.gradle.kts`). Dependency direction is
  one-way: `app → sdk → native`. The SDK's public surface is the only API the app (or any AAR
  consumer) can reach — there is no private path, matching the module's documented contract.
- Static STL (`ANDROID_STL=c++_static`, `tools/build_android.sh`): three `.so` files, no
  `libc++_shared.so`, removing a class of runtime link failures.
- Native packaging uses `useLegacyPackaging=true` + `extractNativeLibs=true` because
  `System.loadLibrary` needs a real file; this is deliberate and internally consistent.

## 2. Outside the shipped pipeline (dead/alternate surface)

| Path | What it is | Build status | Verdict |
|---|---|---|---|
| `engine/src/main/kotlin/...` | 2 math/noise Kotlin files | not a Gradle module, zero imports | dead code (AF-005) |
| `rust/af-core`, `af-sys`, `af` | Rust reimplementation of the ABI | no cargo step in Gradle or CI | alternate implementation, unverified today (AF-006) |
| `native/src/af_glb_read.cpp` | bounds-checked GLB reader | compiled into libafcore, host-tested | real code, but no app caller (AF-015) |
| `artifacts/` | 2026-10-18 binaries + sample GLB | historical | must not be treated as current-build proof |
| `prebuilt/` | committed arm64 `.so` | packaged by `-Paf.prebuiltJni` | now auto-refreshed by CI when C++ changes |

## 3. Execution architecture, layer by layer

### 3.1 App layer (`app/src/main/java/com/avatarforge/app`)

- Single `MainActivity` (ComponentActivity + Compose, no fragments, no navigation library).
  Screen state is an in-memory enum (`INPUT/PROGRESS/VIEWER`); process death returns to input —
  see AF-004.
- Three screens (`InputScreen`, `ProgressScreen`, `ViewerScreen`) plus `DiagnosticsPanel` and
  `AppBitmaps`. Compose state hoisting is consistent; the `RenderSurfaceView` is hoisted out of
  recomposition and created once per viewer entry via `AndroidView(factory=…)`.
- All long work goes through SDK `JobHandle` coroutines; cancellation is wired to the progress
  screen and delivers a `CancellationException` result (`AvatarEngine.kt:647`).
- No networking code exists, and the manifest requests no `INTERNET` permission — verified both
  in source and by the CI permission audit that fails the build if it appears.

### 3.2 SDK layer (`sdk/src/main/java/com/avatarforge/sdk`)

- `AvatarEngine` is the façade over native generation; `NativeBridge` is the single JNI
  declaration point; `RenderSurfaceView` owns a dedicated `HandlerThread` for all renderer
  ownership (create/frame/destroy on one thread, matching the native thread assertion).
- `DeviceResourceGovernor` centralises RAM/storage/thermal admission; `Diagnostics.kt`
  (`collectDiagnostics`) produces one structured report used by both the screen panel and the
  clipboard text, so the two cannot drift.
- Error propagation is explicit: native result codes become JVM exceptions at the JNI boundary
  (`throwIllegalArgument`/`throwIllegalState` in `af_jni.cpp`); the app catches only at
  user-action boundaries. One inappropriate swallow was found and fixed (AF-003).

### 3.3 JNI / ABI (`native/src/android/af_jni.cpp`, `native/include/afcore.h`)

- Handles are opaque `jlong` with an `asHandle` validity check; closed/null handles throw
  `IllegalArgumentException` instead of dereferencing.
- Large meshes move through **direct ByteBuffers with capacity checks**
  (`engineCopyVertices/CopyIndices` verify `GetDirectBufferCapacity >= needed` before memcpy).
- `GetStringUTFChars` is paired with `ReleaseStringUTFChars` on every path inspected;
  `engineAnalyzeImages` checks `ExceptionCheck` after callback work (`af_jni.cpp:414`).
- ABI contract is machine-readable: `abi/afcore.abi.json` (47 functions, 11 structs, 12 enums),
  regenerated from the header by `tools/abi_manifest.py`; regeneration produced no diff this
  session. Two implementations (C++ and Rust) are expected to pass `abi_conformance.py`.
- Residual risk (cannot be exercised without a device): thread-attachment edges for callbacks,
  and global-ref lifetime of the callback object — documented for device testing.

### 3.4 Native kernel (`native/src`)

- Generation pipeline: anatomy → SDF field → isosurface → UV → texture bake → rig →
  blendshapes → animation clips → GLB write. Determinism is asserted by the host harness
  (identical spec ⇒ byte-identical geometry), and adult-safety gates exist independently in
  Kotlin, the C ABI and Rust (`STATUS.md`, `AdultSafetyTest`).
- GLB reader enforces declared-length/chunk-length bounds and index bounds
  (`af_glb_read.cpp:565,579,635`) and collects warnings rather than crashing on malformed JSON;
  covered by harness cases [14]/[15] per `host_tests.cpp` header.
- Known compiler warnings listed in `STATUS.md` are unused-variable class only.

### 3.5 Renderer (`native/src/render`)

- Two backends behind one `Renderer` contract; `BudgetRenderer` wraps whichever backend was
  created and owns the quality ladder (4 tiers; MSAA/shadow/bloom only). Vulkan is primary,
  GLES 3.1 fallback; selection and both failure strings are surfaced to diagnostics.
- Vulkan resources: explicit swapchain/depth/HDR/shadow targets, per-frame UBO slices,
  descriptor sets rebuilt after atlas upload, pipeline creation centralised. The 2026-10-24
  barrier audit found and fixed one spec violation (AF-010); a full second-pass barrier audit
  remains recommended in RENDERER_AUDIT.
- GLES uses driver `glGenerateMipmap`; EGL native-visual binding and explicit `eglBindAPI` were
  fixed earlier (`RENDERER_FIX_2026-10-24.md`).
- No engine-global singletons in the renderer; destruction order is surface → renderer →
  thread, with a 2 s latch in `surfaceDestroyed`/`release`.

## 4. Cross-cutting verdicts

| Concern | Verdict | Evidence |
|---|---|---|
| Layering / dependency direction | REAL + VERIFIED (static) | one-way `app→sdk→native`; no cyclic package imports |
| JNI memory discipline | REAL + VERIFIED (static) | handle checks, buffer capacity checks, string release pairing |
| ABI contract discipline | REAL + VERIFIED | manifest regeneration no-diff; conformance tooling exists |
| Error honesty | PARTIAL → improved | AF-003 fixed; remaining `runCatching` sites reviewed (all show diagnostics or defaults intentionally) |
| Offline guarantee | REAL + VERIFIED (static + CI) | no network code; INTERNET absence enforced in workflow |
| Persistence / process death | PARTIAL after AF-004 | renderer settings persist; prompt/photos/spec still in-memory only |
| Dead code hygiene | PARTIAL | AF-005/AF-006 documented; no deletion performed without owner sign-off |
| Test gating | PARTIAL after AF-001 | JVM + host + ABI + GLB checks now run in CI; device tests absent (AF-012) |

## 5. Architectural decisions deliberately NOT changed

- Keeping the custom renderer rather than adopting Filament: justified in `STATUS.md` and
  re-examined in COMPETITIVE_ANALYSIS; the codebase now has device-validated Vulkan and a
  diagnostics contract that a swap would discard. No change recommended this session.
- Two-module Gradle layout: the app/SDK boundary is the product boundary (third-party AAR
  consumers); splitting into `core/feature` modules would add ceremony without a second
  consumer. No change.
- R8 off: correct while JNI lookup is name-based and keep rules are minimal; revisit only with
  an instrumentation test that proves the shrunken APK loads the native bridge.
