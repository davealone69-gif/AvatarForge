# AvatarForge SDK transplant audit — 2026-10-24

Repository: https://github.com/davealone69-gif/AvatarForge
Working branch: `work/android-engine-dev`
Default/stable branch: `main` (not modified)
Scope: source/configuration audit only. No Android device or emulator is available in this audit session.

## Evidence inspected

- `settings.gradle.kts`: includes `:sdk` and `:app`; app depends on SDK as a Gradle project.
- `sdk/build.gradle.kts`: Android library module, minSdk 26, compileSdk 34, NDK 26.1.10909125, CMake native build, default ABIs arm64-v8a and x86_64, JNI packaging configuration, consumer ProGuard rules, and config assets.
- `native/CMakeLists.txt`: defines native core, renderer, and JNI bridge libraries; Vulkan and GLES dependencies are described in source comments.
- `sdk/src/main/AndroidManifest.xml`: FileProvider declared; no INTERNET permission or background service declared in this manifest.
- `sdk/src/main/java/com/avatarforge/sdk/AvatarEngine.kt`: documented public engine entry points include health/capabilities, text/image generation, job status/cancel/result, and GLB export.
- `sdk/src/main/java/com/avatarforge/sdk/render/AvatarRenderer.kt` and `RenderSurfaceView.kt`: renderer wrapper and dedicated render-thread/surface lifecycle exist in source.
- `app/build.gradle.kts`: app consumes `:sdk`; release tasks fail closed when root `keystore.properties` is absent/incomplete.

## Findings

### Confirmed from source

1. Engine and renderer source are separated from the Compose app UI in a dedicated Android library module.
2. The SDK module is configured to build/package native code via CMake or accept prebuilt JNI libraries via `-Paf.prebuiltJni`.
3. Public Kotlin entry points and a SurfaceView renderer host exist.
4. Consumer keep rules exist for JNI bridge/callback names.
5. The SDK manifest itself does not request INTERNET permission; host-app merged manifest and APK still need inspection in a consumer build.
6. Release signing is configured to fail closed without a valid keystore. A debug build is not a signed production release.

### Gaps / blockers

1. **Independent consumer smoke test: NOT PRESENT IN EVIDENCE.** No clean, separate Android application was built against a produced SDK AAR in this audit. The current app's `:sdk` project dependency does not prove that an externally consumed AAR is complete.
2. **AAR distribution: NOT ESTABLISHED.** The inspected `sdk/build.gradle.kts` configures an Android library, but no Maven publication/repository contract was established by the inspected files. A consumer may build an AAR task locally, but versioned external distribution and dependency metadata need explicit verification.
3. **Runtime renderer: UNVERIFIED.** Source-level lifecycle and recovery code cannot prove a Surface initializes or frames are submitted on a device.
4. **Native payload consistency: UNVERIFIED.** When `-Paf.prebuiltJni` is used, the resulting libraries must be proven to match the audited native source and include the required symbols/ABIs.
5. **Consumer app compatibility: UNVERIFIED.** Need a separate app with a different applicationId to verify manifest merge/FileProvider authority, native library loading, R8 consumer rules, lifecycle and export.
6. **Checklist paths:** `TEST_MATRIX.md`, `RELEASE_CHECKLIST.md`, `docs/TEST_MATRIX.md`, and `docs/RELEASE_CHECKLIST.md` were not found at those paths on `work/android-engine-dev`. This does not rule out renamed files or files on another branch. README points to `DEVICE.md` and `docs/RENDERER_FIX.md` instead.

## Required next steps

- [ ] Locate/reconcile the canonical 10-step device matrix and release checklist; do not replace or weaken the user's existing test requirements.
- [ ] Add a minimal, independent consumer sample app/module with a distinct applicationId.
- [ ] Build the SDK AAR from the exact audited working-branch SHA and inspect its contents for Kotlin classes, manifest/resources/assets, and `jni/<abi>/libafcore.so`, `libafrender.so`, `libavatarforge.so`.
- [ ] Consume that AAR from the separate app without `implementation(project(":sdk"))`; build debug APK and verify merged manifest and ABI payload.
- [ ] Add a runtime test protocol for native load/health, Surface create, avatar attach, visible frame counters, pause/resume, resize/recreate, renderer failure/restart, and GLB export/share.
- [ ] Run the exact 10-step device matrix, including the Xclipse 940 mip retest and white-head-cap hypothesis, when a physical device is available. Until then, keep device runtime UNVERIFIED.
- [ ] Keep release packaging BLOCKED until the owner provisions a valid keystore and all required signing fields are validated. Keep debug artifacts explicitly labelled DEBUG.
- [ ] Keep prompt/photo persistence, ADPF thermal adaptation, GLB import, phantom workflows/repo-admin, and instrumented tests tracked as deferred work.

## Status

- Source architecture audit: **PARTIAL, source inspected**
- SDK transplant readiness: **NOT YET PROVEN**
- Independent AAR consumer build: **NOT RUN**
- Device runtime: **UNVERIFIED**
- Signed release: **BLOCKED**
- `main`: **UNCHANGED**
