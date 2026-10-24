# Renderer initialisation fix (2026-10-24)

## Symptom

On the device report (Samsung Galaxy S24, Xclipse 940 / ANGLE) the viewer never leaves
**"initialising GPU…"**, the frame counter stays at **0 FPS**, and no avatar is ever drawn.
Generation, rigging and GLB export all succeed, so the failure is confined to renderer start-up.

The renderer has never been run on physical hardware in this repository (see
[RENDERER_RECOVERY_2026-10-21.md](RENDERER_RECOVERY_2026-10-21.md)); three concrete defects that
each independently produce this symptom were found by source audit and are fixed below. None of
the fixes is device-verified yet — the retest checklist at the bottom is the gate.

## Defects and fixes

### 1. GLES window surface never matched the EGL config's native visual (ANGLE rejects this)

`GlRenderer::createContext()` chose an EGL config but never read `EGL_NATIVE_VISUAL_ID` and never
called `ANativeWindow_setBuffersGeometry()`. A fresh `SurfaceView` window keeps its default buffer
format; lenient legacy drivers tolerate the mismatch, but strict EGL implementations — ANGLE on
the reported device — reject the following `eglCreateWindowSurface()` with `EGL_BAD_MATCH`.
That makes the GLES fallback fail immediately after a Vulkan failure, so both backends are lost
and the viewer is stuck at 0 FPS.

`native/src/render/af_gl.cpp` now performs the standard Android EGL sequence between
`eglChooseConfig` and `eglCreateWindowSurface`:

1. `eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)`;
2. `ANativeWindow_setBuffersGeometry(window, 0, 0, nativeVisual)`;
3. `eglBindAPI(EGL_OPENGL_ES_API)` is now called explicitly instead of relying on the default
   client API.

Every EGL failure in context creation also carries its `eglGetError()` code in the message, so a
future refusal is diagnosable from the viewer's error card instead of a generic string.

### 2. Vulkan hard-coded an optional depth format

The Vulkan backend allocated every depth/shadow target with `VK_FORMAT_D32_SFLOAT`. D32_SFLOAT is
**not** a mandatory Vulkan 1.0 format — only D16_UNORM is — so a strict driver can fail
`vkCreateImage` for the depth targets during `init()`, taking the whole Vulkan backend down.

`native/src/render/af_vk.cpp` gains `VulkanRenderer::pickDepthFormat()`, called once in `init()`
before any target is allocated. It probes, best first, D32_SFLOAT → D24_UNORM_S8_UINT →
D32_SFLOAT_S8_UINT → D16_UNORM for both `DEPTH_STENCIL_ATTACHMENT` and `SAMPLED_IMAGE` support
(the shadow map is read through a comparison sampler), and only falls back to the legacy value if
the probe reports nothing usable.

### 3. A failed create permanently stranded the viewer — no retry, no fallback, no restart

`RenderSurfaceView.surfaceCreated()` ran exactly one create attempt. If it threw, the render
thread set the error state and the frame loop kept re-posting itself against a null renderer
forever; nothing ever re-ran creation. Toggling "Prefer Vulkan" did nothing on a null renderer
either, so the only recovery was leaving the viewer and regenerating the avatar.

`sdk/src/main/kotlin/com/avatarforge/sdk/render/RenderSurfaceView.kt` now has a bounded, honest
recovery sequence per surface:

1. create with the user's settings (Vulkan preferred);
2. on failure, retry with Vulkan switched off so GLES 3.1 gets a **clean** attempt rather than a
   process that already ran a failed Vulkan initialisation;
3. one delayed retry (250 ms) for a surface that was not fully ready;
4. after three attempts the error card stays up and no further attempts run automatically.

A public `restart()` re-runs the whole sequence, wired to a new **Retry renderer** button in the
viewer's error card (`app/.../ui/ViewerScreen.kt`, `app/.../MainActivity.kt`). Pending retries are
cancelled in `surfaceDestroyed()` and `release()` so they never touch a released window.

### 4. Viewer header no longer reports a performance verdict before the first frame

The header showed "below 30 FPS" in red while the renderer was still initialising (0 FPS is not a
measurement yet). While `rendererInfo == null` it now reads "waiting for the first frame" in the
neutral colour, and fixed (non-AUTO) presets get an honest label — the budget controller only
steps such presets down under load, never back up.

## Files changed

| File | Change |
|---|---|
| `native/src/render/af_gl.cpp` | EGL native-visual matching, explicit `eglBindAPI`, EGL error codes |
| `native/src/render/af_vk.cpp` | `pickDepthFormat()` probe used by all depth/shadow targets |
| `sdk/src/main/kotlin/com/avatarforge/sdk/render/RenderSurfaceView.kt` | bounded create retries, GLES-forced second attempt, `restart()` |
| `app/src/main/java/com/avatarforge/app/ui/ViewerScreen.kt` | Retry button, honest initialising/fixed-preset wording |
| `app/src/main/java/com/avatarforge/app/MainActivity.kt` | wires Retry to `RenderSurfaceView.restart()` |

## Rebuild requirement (important)

The APK workflow builds with `-Paf.prebuiltJni=prebuilt`, i.e. it packages the checked-in
`prebuilt/arm64-v8a/*.so` instead of compiling native code. The two C++ fixes therefore do **not**
reach an APK until the native libraries are rebuilt in an NDK environment and committed:

```bash
tools/build_android.sh                 # needs ANDROID_HOME with NDK 26.1.10909125 + CMake 3.22
git add prebuilt/arm64-v8a
```

The Kotlin recovery (retries, fallback, Retry button, header wording) ships through the normal
Gradle build regardless. A build without the native rebuild still improves recovery and
diagnosability, but the ANGLE surface-format and depth-format fixes are not on the device until
the `.so` files are refreshed.

## Device retest checklist (still the gate — nothing here is hardware-verified)

1. Fresh install on the Xclipse 940 device; open the viewer after a generation.
2. The header must leave "initialising GPU…" and name a real backend (`vulkan` or `gles31`) with
   a non-zero, rising FPS counter and visible geometry.
3. If Vulkan refuses, the error text must name the Vulkan reason, then switch to the GLES attempt
   automatically; the viewer must recover without leaving the screen.
4. Kill the GLES path deliberately (toggle "Prefer Vulkan" off, use Retry) and confirm the GLES
   window surface creates — this directly exercises the `ANativeWindow_setBuffersGeometry` fix.
5. Exercise rotation, background/foreground, and Retry after a forced failure; confirm no frame is
   drawn into a released surface and no render thread is leaked.
6. Capture the full error string from the card if all three attempts fail, and record it against
   this document.
