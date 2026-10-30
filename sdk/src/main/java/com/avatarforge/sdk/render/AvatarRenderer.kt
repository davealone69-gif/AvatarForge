/*
 * AvatarForge :: AvatarRenderer.kt
 *
 * Kotlin wrapper over the JNI renderer bridge. Native C++ implements Vulkan by default and GLES 3.1
 * without a game engine; see native/src/render/af_render.h for backend selection.
 *
 * Ownership rule: one renderer per Surface, created/used/destroyed on ONE thread. The app satisfies
 * that with RenderSurfaceView's dedicated render thread. Calling any method here
 * from another thread is a bug and the kernel will notice (the backends assert the thread).
 */
package com.avatarforge.sdk.render

import android.view.Surface
import com.avatarforge.sdk.internal.KernelAccess
import com.avatarforge.sdk.internal.NativeBridge
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Quality preset, matching `af_quality` in af_render_capi.h (0 = AUTO steps down under load). */
enum class Quality(val value: Int) {
    AUTO(0), LOW(1), MEDIUM(2), HIGH(3);

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: AUTO
    }
}

/** What the renderer is running on. Shown in the app's stats drawer, never guessed. */
data class RendererInfo(
    val backend: String,
    val deviceName: String,
    val apiVersion: String,
    val driver: String,
    val supportsShadows: Boolean,
    val supportsMsaa: Boolean,
    val supportsFloatHdr: Boolean,
    val supportsAnisotropy: Boolean,
) {
    val isGles: Boolean get() = backend.startsWith("gles", ignoreCase = true)
}

/** Live frame statistics. */
data class RendererStats(
    val frameMs: Float,
    val cpuMs: Float,
    val fps: Float,
    val textureMiB: Float,
    val drawCalls: Int,
    val triangles: Int,
    val vertices: Int,
    val shadowMapSize: Int,
    val msaaSamples: Int,
    val bloomLevels: Int,
    val qualityLevel: Int,
    /** -1 means GPU timing is unsupported, unavailable, pending, or invalidated; never show it as measured. */
    val gpuMs: Float = -1f,
    val frameAttempts: Int = 0,
    val frameSubmissions: Int = 0,
    val presentRequestsAccepted: Int = 0,
) {
    /** True only after a finite, positive caller-supplied frame-loop delta has been observed. */
    val hasValidFrameTiming: Boolean
        get() = frameMs.isFinite() && frameMs > 0f && fps.isFinite() && fps > 0f

    val meetsTarget: Boolean get() = hasValidFrameTiming && fps >= 30f
}

/** App-level graphics API; GLES through ANGLE still counts as GLES, not native Vulkan. */
enum class RenderBackend { GLES, VULKAN }

/** Everything the viewer's settings panel can change. Defaults match RendererOptions in C++. */
data class RenderSettings(
    val quality: Quality = Quality.AUTO,
    val backend: RenderBackend = RenderBackend.VULKAN,
    val shadows: Boolean = true,
    val bloom: Boolean = false,
    val sss: Boolean = true,
    val groundPlane: Boolean = true,
    val exposure: Float = 0.65f,
    val sunAzimuthDeg: Float = 42f,
    val sunElevationDeg: Float = 38f,
    val sunIntensity: Float = 1.5f,
    val turbidity: Float = 2.6f,
    val environmentIntensity: Float = 0.65f,
    val shadowSoftness: Float = 1.0f,
    val msaa: Int = 4,
    val shadowMapSize: Int = 2048,
    val bloomLevels: Int = 4,
)

/** Orbit camera state. Metres, degrees; yaw 180 looks at the model's front (-Z forward). */
data class CameraSettings(
    val yawDeg: Float = 180f,
    val pitchDeg: Float = 4f,
    val distance: Float = 3.5f,
    val targetY: Float = 0.95f,
    val fovDeg: Float = 32f,
)

class AvatarRenderer private constructor(private var handle: Long) {

    companion object {
        /**
         * Creates the selected native renderer for a live Surface. Vulkan is the default; if its
         * initialization fails, the native factory tries GLES 3.1 and exposes the actual backend
         * in info(). Neither API ever falls back to a software success.
         */
        fun create(surface: Surface, settings: RenderSettings = RenderSettings()): AvatarRenderer {
            check(NativeBridge.available) { "the native library is not loaded" }
            /* Sizing is driven by the surface, but the kernel needs the requested targets up front
               so it can pick its HDR format and sample count honestly. */
            val handle = NativeBridge.rendererCreate(
                surface,
                settings.quality.value,
                settings.msaa,
                settings.shadowMapSize,
                settings.bloomLevels,
                settings.backend == RenderBackend.VULKAN,
            )
            check(handle != 0L) { "Neither selected GPU renderer could be initialised" }
            val renderer = AvatarRenderer(handle)
            renderer.settings = settings
            renderer.applyOptions(settings)
            return renderer
        }
    }

    private var settings: RenderSettings = RenderSettings()

    val isValid: Boolean get() = handle != 0L

    fun info(): RendererInfo {
        val strings = arrayOfNulls<String>(4)
        val flags = IntArray(4)
        NativeBridge.rendererInfo(handle, strings, flags)
        return RendererInfo(
            backend = strings[0] ?: "unknown",
            deviceName = strings[1] ?: "unknown GPU",
            apiVersion = strings[2] ?: "",
            driver = strings[3] ?: "",
            supportsShadows = flags[0] != 0,
            supportsMsaa = flags[1] != 0,
            supportsFloatHdr = flags[2] != 0,
            supportsAnisotropy = flags[3] != 0,
        )
    }

    fun stats(): RendererStats {
        val floats = FloatArray(5)
        val ints = IntArray(10)
        NativeBridge.rendererStats(handle, floats, ints)
        return RendererStats(
            frameMs = floats[0], cpuMs = floats[1], fps = floats[2], textureMiB = floats[3],
            drawCalls = ints[0], triangles = ints[1], vertices = ints[2], shadowMapSize = ints[3],
            msaaSamples = ints[4], bloomLevels = ints[5], qualityLevel = ints[6],
            gpuMs = floats[4], frameAttempts = ints[7], frameSubmissions = ints[8],
            presentRequestsAccepted = ints[9],
        )
    }

    fun resize(width: Int, height: Int): Boolean =
        width > 0 && height > 0 && NativeBridge.rendererResize(handle, width, height)

    /**
     * Hands the engine's current mesh to the GPU. Call after every stage that changes geometry.
     *
     * Takes the raw kernel handle rather than the Kotlin wrapper because the renderer reads the
     * engine's buffers directly over the C ABI; the wrapper is not involved and passing it would
     * only suggest the renderer could call back into the SDK.
     */
    fun attachEngine(engineHandle: Long, invalidate: Boolean = true) {
        /* The renderer keeps a pointer to the engine's arrays, so the pointer swap is fenced:
           otherwise a generation job could free the old mesh between attach and the first upload. */
        val rc = KernelAccess.withKernel { NativeBridge.rendererAttachEngine(handle, engineHandle) }
        check(rc == 0) { "renderer refused the engine (${NativeBridge.engineLastError(engineHandle)})" }
        if (invalidate) invalidateMesh()
    }

    fun invalidateMesh() = KernelAccess.withKernel { NativeBridge.rendererInvalidateMesh(handle) }

    fun setCamera(camera: CameraSettings) =
        NativeBridge.rendererSetCamera(handle, camera.yawDeg, camera.pitchDeg, camera.distance, camera.targetY, camera.fovDeg)

    fun applyOptions(newSettings: RenderSettings) {
        settings = newSettings
        NativeBridge.rendererSetOptions(
            handle,
            newSettings.quality.value,
            newSettings.shadows,
            newSettings.bloom,
            newSettings.sss,
            newSettings.exposure,
            newSettings.sunAzimuthDeg,
            newSettings.sunElevationDeg,
            newSettings.sunIntensity,
            newSettings.turbidity,
            newSettings.environmentIntensity,
            newSettings.groundPlane,
            newSettings.msaa,
            newSettings.shadowMapSize,
            newSettings.bloomLevels,
            newSettings.shadowSoftness,
        )
    }

    /** Inspection overlays: skeleton lines, flat shading (material check) and region highlight. */
    fun setOverlays(skeleton: Boolean, flatShading: Boolean, highlightRegion: Int) =
        NativeBridge.rendererSetOverlays(handle, skeleton, flatShading, highlightRegion)

    /** Selects a renderer-owned diagnostic fixture. Must be called on the render thread. */
    fun setDiagnosticRung(rung: Int) = NativeBridge.rendererSetDiagnosticRung(handle, rung)

    /** Returns the native renderer's currently selected rung. */
    fun diagnosticRung(): Int = NativeBridge.rendererDiagnosticRung(handle)

    /** Plays a library clip. [loop] is meaningful for the looping clips (idle, walk, run...). */
    fun setClip(clip: Int, speed: Float = 1f, loop: Boolean = true) =
        NativeBridge.rendererSetClip(handle, clip, speed, loop)

    /**
     * Draws one frame. Must be called on the render thread.
     *
     * Takes the kernel fence because a frame skins the mesh (clip playback) and then uploads the
     * engine's own vertex buffer: without it, an expression slider on the UI thread could be writing
     * vertices while the upload reads them.
     */
    fun frame(deltaSeconds: Float) = KernelAccess.withKernel { NativeBridge.rendererFrame(handle, deltaSeconds) }

    /** Blocks until the GPU is idle; used before destroying the renderer. */
    fun waitIdle() = NativeBridge.rendererWaitIdle(handle)

    val currentClip: Int get() = NativeBridge.rendererCurrentClip(handle)
    val clipTime: Float get() = NativeBridge.rendererClipTime(handle)

    /**
     * Picking: converts a tap in view coordinates into an `af_region` id.
     *
     * The ray is built on the GPU side's camera convention (af_render_screen_ray) so a tap and the
     * rendered image cannot disagree - the alternative, rebuilding the ray in Kotlin, is exactly the
     * kind of duplicated maths that drifts.
     */
    fun pickRegion(
        engineHandle: Long,
        viewX: Float,
        viewY: Float,
        viewWidth: Int,
        viewHeight: Int,
        camera: CameraSettings,
    ): RegionPick? {
        /* Normalised device coordinates, y up (the kernel flips for the tap's y-down origin). */
        val ndcX = (viewX / viewWidth) * 2f - 1f
        val ndcY = 1f - (viewY / viewHeight) * 2f
        val point = FloatArray(4)
        /* Raycast against the live vertex array, which the GL thread rewrites every animated frame.
           Bounded wait: a tap that cannot get the fence in 250 ms reports "nothing picked" rather
           than intersecting a mesh that is mid-upload. */
        val region = KernelAccess.tryWithKernel(250L) {
            NativeBridge.enginePickRegion(
                engineHandle,
                camera.yawDeg, camera.pitchDeg, camera.distance, camera.targetY, camera.fovDeg,
                ndcX, ndcY, viewWidth, viewHeight, point,
            )
        } ?: return null
        if (region < 0) return null
        return RegionPick(region, point[0], point[1], point[2], point[3])
    }

    fun destroy() {
        if (handle != 0L) {
            NativeBridge.rendererDestroy(handle)
            handle = 0L
        }
    }
}

/** Result of a tap: which `af_region` was hit, where, and how far along the ray. */
data class RegionPick(val region: Int, val x: Float, val y: Float, val z: Float, val distance: Float)
