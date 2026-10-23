# Perfetto profiling for AvatarForge

Perfetto is the system-level performance trace path for diagnosing the renderer's zero-FPS / zero-triangle stalls, frame pacing, scheduling delays, and CPU contention. This setup adds low-overhead Android trace slices and allows shell profiling of release builds on Android 10+.

## Instrumentation added

The SDK emits these slices on the dedicated `avatarforge-render` thread:

- `AvatarForge.frame`: total scheduled frame work, including exception paths.
- `AvatarForge.camera`: camera state submission.
- `AvatarForge.renderer.frame`: renderer call, including JNI/native CPU submission time.
- `AvatarForge.renderer.stats`: telemetry collection and stall detection.

These slices measure CPU-side duration. They do **not** prove GPU completion or physical display/presentation. Pair them with Android graphics/GPU tracks and AvatarForge's FPS, triangle, and draw-call telemetry.

The app manifest enables `<profileable android:shell="true" />` for shell-driven profiling on Android 10+. This does not make the app debuggable and does not add network permission.

## Capture on the Samsung phone

1. Install the APK built from the commit being investigated.
2. Reproduce the issue, preferably with the avatar visible and after the renderer begins slowing down.
3. Open [Perfetto UI](https://ui.perfetto.dev/) on a computer and connect the Android device using its supported device-recording workflow, or use Android Studio's **Profiler > System Trace**.
4. Record 15–30 seconds. Enable scheduling/CPU frequency and Android app trace events; select package `com.avatarforge` when prompted. Include graphics/GPU tracks if the device exposes them.
5. Save the trace as `.pftrace` and open it in Perfetto UI.

## ADB capture (optional)

When ADB is available, this captures 20 seconds of system scheduling plus app trace events:

```sh
adb shell perfetto \
  -o /data/misc/perfetto-traces/avatarforge.pftrace \
  -t 20s \
  sched freq idle am wm gfx view binder_driver hal dalvik \
  -a com.avatarforge
adb pull /data/misc/perfetto-traces/avatarforge.pftrace .
```

If the device rejects a data source or category on its Android build, remove only the unsupported item and retry. Do not treat a failed capture as a renderer failure.

## What to inspect

- Find the `avatarforge-render` thread and expand `AvatarForge.frame` slices.
- Long `AvatarForge.renderer.frame` with high CPU activity points toward native CPU work, driver submission, or a blocking renderer call. The slice alone cannot distinguish those causes.
- Long gaps between `AvatarForge.frame` slices with the thread runnable but not scheduled suggest CPU contention or scheduling delay.
- Short frame slices but poor FPS / late presentation require graphics/GPU tracks and frame-timeline evidence; CPU duration alone is not GPU frame time.
- If frame slices continue while app telemetry remains at zero FPS/triangles, inspect native early-return/error paths and EGL/Vulkan acquire-submit-present results.
- Compare a normal run with a stalled run using the same device, scene, quality tier, and capture duration.

## Evidence checklist

Record the APK's source commit SHA, Android version, GPU/backend, scene/triangle count, quality/MSAA settings, thermal status, capture duration, and whether the avatar was visible. Keep the original trace. Redact unrelated sensitive activity before sharing traces publicly.

**Verification status:** source instrumentation is committed, but an Android build, on-device trace capture, and physical-device renderer diagnosis remain unverified until performed on the exact APK.
