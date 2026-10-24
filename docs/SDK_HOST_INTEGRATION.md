# AvatarForge SDK: host-app integration

This guide describes the current source contract. The SDK has **not** yet been independently verified inside a separate host app on a device. Treat integration/runtime claims as UNVERIFIED until the smoke test below has real build and device evidence.

## Compatibility and architecture

- Android minimum: API 26 (Android 8.0).
- Kotlin/JVM target: 17. Use JDK 17 for building.
- Native ABIs: `arm64-v8a` by default and `x86_64` for emulator builds. An AAR only carries the ABIs included when it was built.
- Renderer: native Vulkan with OpenGL ES fallback. Actual GPU/backend behavior is device-dependent and not proven by packaging alone.
- Process model: **one process-wide engine/native pipeline**. Do not instantiate parallel isolated engines. Generation transactions are serialized. The GPU renderer/context and mesh budget are shared.
- Resource envelope: plan for approximately **1.5 GB peak memory during generation** and a **34 MB mesh budget**, as currently documented by the project. These are planning constraints, not a promise that every device can sustain generation.
- Offline behavior: the SDK is designed for on-device use. The host application owns its own permissions and network policy.

## Option A: publish to a local Maven repository (recommended)

From the AvatarForge repository root, with JDK 17, Android SDK platform 34 and NDK 26.1.10909125 configured:

```bash
./gradlew :sdk:publishReleasePublicationToLocalBuildRepository
```

The publication is written under `sdk/build/repo/com/avatarforge/avatarforge-sdk/0.10.2-SNAPSHOT/` by default. Override the version for a release candidate:

```bash
./gradlew :sdk:publishReleasePublicationToLocalBuildRepository -Paf.sdkVersion=0.10.2
```

This generates the AAR, a Maven POM and Gradle module metadata. It does not upload to a remote repository and does not sign a production app release.

In the host app's `settings.gradle.kts`, add the local repository (adjust the relative path to your checkout):

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri("../AvatarForge/sdk/build/repo") }
    }
}
```

Then in the host app module's `build.gradle.kts`:

```kotlin
dependencies {
    implementation("com.avatarforge:avatarforge-sdk:0.10.2-SNAPSHOT")
}
```

Use the same `af.sdkVersion` value for publishing and consuming. For a shared team repository, publish the generated Maven publication to an authenticated Maven host using credentials supplied by CI secrets or environment variables; do not commit credentials.

## Option B: raw AAR (manual dependency management)

Build the AAR:

```bash
./gradlew :sdk:assembleRelease
```

Copy `sdk/build/outputs/aar/sdk-release.aar` into the host app's `libs/` directory, then declare:

```kotlin
dependencies {
    implementation(files("libs/sdk-release.aar"))
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
```

A raw AAR has no POM; those dependencies must be declared by the host. Coroutines is a compile dependency because public SDK APIs expose `StateFlow` and suspend functions. The Maven publication should declare dependencies so Gradle can resolve them transitively.

## Manifest and packaging notes

- The SDK declares a `FileProvider`; inspect the merged manifest in the host app and ensure provider authorities do not collide with another provider. If the authority is fixed rather than applicationId-derived, a second app may require a code/config change before safe integration.
- The SDK manifest does not itself request `INTERNET`; the host app's own permissions remain its responsibility.
- Keep `consumer-rules.pro` in the AAR. Do not strip JNI bridge methods/classes with host R8 rules.
- Do not manually copy only one native library. Package the complete, matching `libafcore.so`, `libafrender.so`, and `libavatarforge.so` set for each intended ABI.
- Do not mix a stale AAR with native libraries from a different commit. Build and record the AAR from one exact commit SHA.

## Required API and lifecycle smoke test

The following is a test plan, **not a claim that these tests have passed**:

1. Create a clean Android app with a distinct applicationId; consume the published Maven coordinate, not `implementation(project(":sdk"))`.
2. Build a debug APK and verify dependency resolution, merged manifest, R8/consumer rules, and native libraries for the target ABI.
3. At startup, call the public health/capability API and record the exact result and engine version.
4. Create the renderer surface and confirm the backend initializes; do not treat a healthy native library as proof of rendering.
5. Load/generate an avatar and confirm visible geometry plus advancing frame/draw/triangle counters.
6. Pause/resume, rotate or resize, background/foreground, and destroy/recreate the surface.
7. Confirm failure/recovery paths report real state rather than success with zero frames.
8. Export a GLB and verify the output exists and parses.
9. Repeat on the target Samsung/Xclipse 940 device, including the project's mip retest and white-head-cap investigation.
10. Record app commit, SDK commit, AAR SHA-256, device/OS/GPU, ABI, diagnostics, and result for every step.

## Known limitations

- No independent host-app integration/device-runtime evidence exists yet.
- The archived `artifacts/avatarforge-sdk-arm64.aar` is stale relative to current source. Rebuild from the audited HEAD before distributing.
- The process-wide native pipeline, GPU context, mesh budget and generation memory peak are shared resources, not per-instance isolated engines.
- Prompt/photo persistence, ADPF thermal adaptation, GLB import, phantom workflows/repo-admin and instrumented tests remain deferred unless separately verified as completed.
- Signed production release remains blocked until a valid release keystore is provisioned. Debug artifacts must remain labelled DEBUG.
