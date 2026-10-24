# AvatarForge SDK — host-app integration guide

How to embed the AvatarForge generation engine and renderer inside another Android app. Every
signature in this document is taken from the current source of `:sdk`; nothing here is
aspirational. Verification status: the SDK **builds and its tests pass in CI** (see
`docs/audit/VERIFICATION_RESULTS.md`), but integration into a third-party app has never been
performed — that layer is **UNVERIFIED** and the device checklist at the end is the way to
close it.

## 1. What you get

An Android library (Maven coordinates `com.avatarforge:avatarforge-sdk`, packaging `aar`)
containing:

- `AvatarEngine` — procedural avatar generation (anatomy → geometry → UV/PBR bake → rig →
  blendshapes → animation → glTF binary export), running in-process on SDK-owned threads;
- `RenderSurfaceView` — a plain `android.view.SurfaceView` (not Compose) with Vulkan-preferred
  / GLES 3.1 fallback rendering, adaptive quality, and diagnostics flows;
- three native libraries (`libavatarforge.so`, `libafcore.so`, `libafrender.so`) inside the
  AAR under `jni/<abi>/`;
- an offline customization schema shipped as AAR assets;
- a manifest that contributes a non-exported FileProvider and **no permissions**.

## 2. Host requirements

| Requirement | Value | Where enforced |
|---|---|---|
| minSdk | 26 (Android 8.0) | `sdk/build.gradle.kts` |
| compileSdk | 34 | same |
| JDK | 17 | same |
| Build tooling | Gradle 8.9 / AGP 8.5.2 / Kotlin 1.9.24 (newer may work, UNVERIFIED) | wrapper + root build file |
| Primary ABI | `arm64-v8a`; `x86_64` exists for emulators in non-single-ABI builds | `abiFilters` |
| Native lib extraction | app manifest must keep `android:extractNativeLibs="true"` (the sample app sets it) | `System.loadLibrary` needs real files |
| Runtime memory | default kernel budget 1.5 GB during a generation; low-RAM presets via `AvatarForge.recommendedSettings` | `GenerationRequest.memoryBudgetBytes` |
| Network | none. No INTERNET permission is declared; the SDK opens no sockets | manifest merge |
| Engine instances | **one per process** — singleton, one 34 MB mesh budget and one GPU context | `AvatarEngine.get()` |

The SDK surface exposes `suspend` functions and `StateFlow`, so a Kotlin/coroutines host is
the natural fit. The public `JobHandle` surface is exactly `status: StateFlow<JobStatus>`,
`suspend await(): EngineResult`, `cancel()` and `isCancelled` — there is no callback or
blocking `result()` method; a Java-only host would need a coroutines bridge (for example
`kotlinx-coroutines-jdk8`'s `future { job.await() }`).

## 3. Three ways to consume it

### Option A — Maven repository (recommended; transitive dependencies automatic)

The SDK now publishes a real POM (`maven-publish`), so its three runtime dependencies resolve
transitively. Coordinates:

```
com.avatarforge:avatarforge-sdk:0.10.1   (packaging: aar)
```

From a CI run, download the `AvatarForge-Android-<sha>` artifact and use the staged
`maven-repo/` directory as a file-backed repository. In the host's `settings.gradle.kts`:

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri("${rootDir}/local-repos/avatarforge") } // the unzipped maven-repo/
    }
}
```

then in the app module:

```kotlin
dependencies {
    implementation("com.avatarforge:avatarforge-sdk:0.10.1")
}
```

For a developer machine, publish straight into the local Maven cache and add `mavenLocal()`:

```bash
./gradlew :sdk:publishReleasePublicationToMavenLocal
```

GitHub Packages is also configured as the `GitHubPackages` publication target
(`https://maven.pkg.github.com/davealone69-gif/AvatarForge`); publishing needs `gpr.user` /
`gpr.token` Gradle properties (or `GITHUB_ACTOR` / `GITHUB_TOKEN` in CI). Note: the repository
has not chosen a LICENSE yet, so the POM deliberately declares none — legal terms must be
settled before any public redistribution.

### Option B — raw AAR in `app/libs/`

Drop the CI-built `AvatarForge-sdk.aar` into the host's `libs/` and declare the SDK's
dependencies explicitly, because a bare AAR carries no POM:

```kotlin
dependencies {
    implementation(fileTree(mapOf("dir" to "libs", "include" to listOf("*.aar"))))
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
```

The JNI keep rules are inside the AAR (`proguard.txt`, generated from
`sdk/consumer-rules.pro`) and AGP applies them automatically; R8 cannot rename `NativeBridge`.

### Option C — source/composite build

Include the module (`includeBuild("../AvatarForge")` with a substituted `:sdk`, or copy the
module) and depend on `project(":sdk")`. This is exactly how the sample `:app` consumes it;
there is no private API path, so a source consumer has the same surface as the AAR consumer.

## 4. Manifest merge effects

The SDK manifest contributes one FileProvider:

- authority: `${applicationId}.avatarforge.fileprovider` — namespaced by the host's application
  id, so it cannot collide with another embedded SDK;
- `exported="false"`, `grantUriPermissions="true"`, paths limited to app-scoped directories
  (`res/xml/avatarforge_file_paths.xml`);
- no permissions, no services, no receivers. The merged manifest therefore stays offline.

`AvatarForge.fileProviderAuthority(context)` returns the runtime authority string.

## 5. Quick start (text generation)

```kotlin
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.AvatarSpec
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

val engine = AvatarEngine.get(context)          // process-wide singleton

// Always check health first: ABI mismatch or a missing .so is reported here, not as a crash.
val health = AvatarEngine.health()
if (!health.nativeLibraryLoaded || health.problem != null) {
    // surface health.problem to the user; do not start a generation.
}

val scope = CoroutineScope(Dispatchers.Main)    // or lifecycleScope in an Activity
scope.launch {
    val job = engine.generateFromText(
        prompt = "a tall muscular ranger in leather armor",
        spec = AvatarSpec(),                    // all fields have defaults; seed 1337 is deterministic
    )
    // job.status is a StateFlow<JobStatus> with stage, 0..1 progress and message text.
    val result = job.await()                    // suspends; cancellation is cooperative at stage bounds
    // result.glbFile is already written in app-scoped storage, ready to share.
}
```

`EngineResult` exposes `glbFile`, `glbBytes`, `vertexCount`, `triangleCount`, `submeshCount`,
`jointCount`, `blendshapeCount`, `textureBytes`, per-stage `timings`, and region names.

Photo-guided generation uses `ImageReference.fromBitmap(bitmap, ImageRole.IDENTITY)` (plus an
optional `ImageRole.CLOTHING` image) and either `engine.generateFromImage(identity, detail)`
or the explicit `analyseImages` → `specFromImages` → `generate(GenerationRequest(...))`
flow. The image path contributes framing, stature and sampled colours — it does **not**
reconstruct a person's identity or garment geometry.

## 6. Embedding the renderer

`RenderSurfaceView` is a View, so it works in any View hierarchy and in Compose through
`AndroidView` — this is the same sequence the sample app uses:

```kotlin
AndroidView(
    modifier = Modifier.fillMaxSize(),
    factory = { ctx ->
        RenderSurfaceView(ctx).also { view ->
            view.bind(engine)
            view.attachEngine(engine.nativeHandle)
            view.applySettings(AvatarForge.recommendedSettings(ctx))
            view.applyCamera(CameraSettings())      // yaw 180°, pitch 4°, distance 3 m
            view.setFrameLoopEnabled(true)
        }
    },
)
```

Public control surface: `applySettings(RenderSettings)`, `applyCamera(CameraSettings)`,
`attachEngine(Long)`, `invalidateMesh()`, `setClip(clip, speed, loop)`,
`setOverlays(skeleton, flatShading, highlightRegion)`, `pick(x, y)`,
`setFrameLoopEnabled(Boolean)`, `restart()`, `release()`. Observable state is exposed as
StateFlows: `stats` (frame time, FPS, triangle/draw counts, adaptive `qualityLevel`),
`rendererInfo`, `error`, and `diagnostics` (start-up attempts, surface counters, quality tier
transitions). Render settings can be persisted across process death with `SettingsStore` in
`com.avatarforge.sdk.render`.

## 7. Export and share

```kotlin
// FileProvider share sheet, offline, temporary read grant only:
val share = AvatarForge.shareIntent(context, result)
context.startActivity(Intent.createChooser(share, "Share avatar"))

// Or let the user pick a destination via SAF (no storage permission needed):
AvatarForge.copyToDestination(context, result.glbFile, destinationUri)

// Or embed the bytes directly in another container:
val bytes: ByteArray = engine.exportGlbBytes(result)
```

`AvatarForge.hasRoomForExport(context)` checks app-scoped free space before a build;
`AvatarForge.deviceReport(context)` returns the plain-text diagnostics report.

## 8. Constraints and honest limitations

- One engine per process; do not create per-Activity instances.
- The kernel handle is thread-affine; stay on the SDK's `JobHandle` API rather than caching
  `nativeHandle` for anything other than renderer attachment.
- The engine is offline by construction. If a future SDK version requested INTERNET, the host
  merge would fail/stand out — treat that as a breaking security change.
- No release keystore or Maven Central publishing exists yet (audit findings AF-009); version
  0.10.1 artifacts come from CI/GitHub, and the archived `artifacts/avatarforge-sdk-arm64.aar`
  is a historical snapshot that may lag HEAD — prefer the CI artifact of the commit you use.
- Third-party integration is **UNVERIFIED**: no external app has consumed the SDK, and the
  maven-publish metadata is verified by CI POM-content checks, not by an external resolution.

## 9. Integration acceptance checklist (maps to `docs/audit/TEST_MATRIX.md` §3)

1. Clean host build resolves the dependency without hand-adding the three runtime libraries
   (Option A) or with them explicitly (Option B).
2. Merged manifest contains no INTERNET permission and exactly one namespaced FileProvider.
3. Release build with R8 enabled runs a generation end to end (validates the consumer keep
   rules — an UnsatisfiedLinkError here means the rules were lost).
4. `AvatarEngine.health()` reports no problem on an arm64 device; renderer first frame within
   5 s; no red diagnostics card.
5. Process death with the viewer open restores render settings when `SettingsStore` is used.
6. Share via FileProvider opens in another app; SAF export round-trips through
   `tools/validate_glb.py`.
7. `RenderSurfaceView` works inside the host's actual UI toolkit (View and/or Compose),
   including background/foreground cycling.
