# Feature traceability

Every user-visible capability traced from the control to its final output, with failure,
cancellation and test evidence. "Test evidence" distinguishes host/JVM checks (automated) from
device checks (none exist, AF-012).

## 1. Input screen

| Feature | Path (UI → handler → native) | Failure behaviour | Cancellation | Evidence |
|---|---|---|---|---|
| Text prompt → spec parsing | `InputScreen.onGeneratePrompt` → `AvatarEngine.parsePrompt` → `engineAnalyzePrompt` JNI | `runCatching` keeps the previous spec (default-safe); generation still starts | n/a | JVM `AdultSafetyTest` (age gate) |
| Reference photos (identity + clothing) | photo picker (`PickVisualMedia`-style launcher, `image/*`) → `ImageReference.fromBitmap` → `analyseImages` → 28-float packed analysis | **now surfaced** via `analysisNote` on null/throw (AF-003 fix); generation falls back to text path, never fakes analysis | n/a | JVM `ImageAnalysisPackingTest` (pack/unpack) |
| Customisation controls | normalized contracts in `config/customization/*.json` shipped as SDK assets; schema gate exposes only `VERIFIED` inputs | invalid contract entries cannot become controls | n/a | `tools/validate_custom_inputs.py` (manual/CI-able; now in host-tests job) |
| Generate button | enabled only when `health.usable`; `AvatarEngine.generate(request)` → `JobHandle` | native errors propagate as exceptions to the progress screen | progress Cancel → `JobHandle.cancel` → native cancel code + `CancellationException` | static trace; device UNVERIFIED |

## 2. Progress screen

| Feature | Path | Failure behaviour | Evidence |
|---|---|---|---|
| Stage progress | `JobStatus` callbacks from native stage transitions → Compose state | last error string shown; no silent success | static trace |
| Cancel | `JobHandle.cancel` sets `AtomicBoolean`, checked by `shouldCancel()` at stage boundaries | partial output is discarded; no GLB is written for a cancelled job (`AF_ERR_CANCELLED` contract) | ABI enum in manifest |

## 3. Viewer (3D)

| Feature | Path | Failure behaviour | Evidence |
|---|---|---|---|
| Surface creation | `RenderSurfaceView.surfaceCreated` → bounded 3-attempt `attemptCreate` (Vulkan → GLES-forced → delayed) | neutral "Starting GPU…" during fallbacks; red card + Retry only on terminal FAILED (AF-011) | device report shows attempt 1 Vulkan OK 495 ms |
| Frame loop | dedicated `HandlerThread`; 60 Hz budget sleep corrected for frame work; stats via `RendererStats` | frame exception → FAILED state, loop stops, Retry re-runs create | device: 58 FPS/17.3 ms measured once |
| Camera orbit/zoom/tap-pick | Compose gesture detector → `applyCamera`/`pick` (CPU ray-mesh on render thread) | pick miss returns null, UI shows nothing selected | static trace |
| Poses/clips | `setClip(clip, speed, loop)`; held pose = speed 0 on a real clip | unknown clip name → Toast, no state change | host harness pose-composition checks [12] |
| Blendshapes | `applyBlendshapes(weights)` → `invalidateMesh` (CPU skinning) | weights array length validated at JNI | JVM packing test; harness rig checks [7] |
| Render settings | `RenderPanel` → `applySettings` → native `setOptions`; **persisted** across process death after AF-004 | invalid enum coerced via `Quality.of` fallback | new `RenderSettingsCodecTest` |
| Diagnostics panel | `collectDiagnostics` → traffic-light rows + attempts card + Copy report; also reachable from global MENU dialog | every WARN/FAIL carries an action hint; no invented values | JVM test for report structure |

## 4. Export / share

| Feature | Path | Failure behaviour | Evidence |
|---|---|---|---|
| Save to SAF | `CreateDocument("model/gltf-binary")` → `AvatarForge.copyToDestination` | `runCatching` result checked; failure Toast (no false success) | static trace |
| Share | `FileProvider` (`${applicationId}.avatarforge.fileprovider`, declared in SDK manifest, `exported=false`) → chooser `model/gltf-binary` | no handler app → explicit Toast | FileProvider manifest + paths XML verified |
| Space gate | `hasRoomForExport` (StatFs, 64 MB requirement) | generation/admission governed before work starts | static trace |
| GLB validity | native writer → host validator `tools/validate_glb.py` | structural failures non-zero exit | **run this session: sample passes all structural checks** |

## 5. Platform contracts

| Contract | Advertised by | Implemented? | Evidence / finding |
|---|---|---|---|
| Launcher MAIN/LAUNCHER | app manifest | yes | standard Activity entry |
| Open `.glb` (ACTION_VIEW) | app manifest (pre-fix) | **was no** — filter removed in AF-002; feature tracked AF-015 | grep showed no intent handler |
| FileProvider share | SDK manifest | yes | authority + grantUriPermissions verified |
| Offline operation | README + no-INTERNET CI gate | yes (static) | no socket APIs; permission audit fails build if added |
| No background work | manifest comment | yes | no service/receiver/worker/AlarmManager references |

## 6. Traceability gaps (honest list)

1. No automated test proves a generated avatar reaches the GPU on a device — host tests cover
   geometry/rig/GLB; one device report proves a rendered frame for one generated avatar.
2. GLB import is implemented in native but unreachable from UI (AF-015).
3. Prompt/photo/spec state does not survive process death (AF-004 partial).
4. Export re-import round trip is host-tested only; cross-app open-from-share on a device is
   UNVERIFIED.
