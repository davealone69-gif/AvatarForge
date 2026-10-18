/*
 * AvatarForge :: AvatarRenderer.kt
 *
 * Kotlin wrapper over the custom renderer's C ABI (native/src/render/af_render_capi.cpp). The
 * renderer itself is C++ (Vulkan first, OpenGL ES 3.1 fallback) and shares nothing with any game
 * engine - see native/src/render/af_render.h for the architecture.
 *
 * Ownership rule: one renderer per Surface, created/used/destroyed on ONE thread. The app satisfies
 * that with RenderSurfaceView, which owns a dedicated GL/Vulkan thread. Calling any method here
 * from another thread is a bug and the kernel will notice (the backends assert the thread).
 */
package com.avatarforge.sdk.render

import android.view.Surface
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
    val isVulkan: Boolean get() = backend.equals("vulkan", ignoreCase = true)
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
) {
    val meetsTarget: Boolean get() = fps >= 30f
}

/** Everything the viewer's settings panel can change. Defaults match RendererOptions in C++. */
data class RenderSettings(
    val quality: Quality = Quality.AUTO,
    val preferVulkan: Boolean = true,
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
    val distance: Float = 3.0f,
    val targetY: Float = 0.95f,
    val fovDeg: Float = 32f,
)

class AvatarRenderer private constructor(private var handle: Long) {

    companion object {
        /**
         * Creates a renderer for a live Surface. Throws when neither Vulkan nor GLES 3.1 can be
         * initialised - there is no software fallback and no "maybe it will work" path, because the
         * app's contract is that a button either does the real thing or is not there.
         */
        fun create(surface: Surface, settings: RenderSettings = RenderSettings()): AvatarRenderer {
            check(NativeBridge.available) { "the native library is not loaded" }
            /* Sizing is driven by the surface, but the kernel needs the requested targets up front
               so it can pick its HDR format and sample count honestly. */
            val handle = NativeBridge.rendererCreate(
                surface,
                settings.quality.value,
                settings.preferVulkan,
                settings.msaa,
                settings.shadowMapSize,
                settings.bloomLevels,
            )
            check(handle != 0L) { "no GPU backend could be initialised (Vulkan and GLES 3.1 both refused)" }
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
        val floats = FloatArray(4)
        val ints = IntArray(7)
        NativeBridge.rendererStats(handle, floats, ints)
        return RendererStats(
            frameMs = floats[0], cpuMs = floats[1], fps = floats[2], textureMiB = floats[3],
            drawCalls = ints[0], triangles = ints[1], vertices = ints[2], shadowMapSize = ints[3],
            msaaSamples = ints[4], bloomLevels = ints[5], qualityLevel = ints[6],
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
        val rc = NativeBridge.rendererAttachEngine(handle, engineHandle)
        check(rc == 0) { "renderer refused the engine (${NativeBridge.engineLastError(engineHandle)})" }
        if (invalidate) NativeBridge.rendererInvalidateMesh(handle)
    }

    fun invalidateMesh() = NativeBridge.rendererInvalidateMesh(handle)

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
            newSettings.preferVulkan,
            newSettings.msaa,
            newSettings.shadowMapSize,
            newSettings.bloomLevels,
            newSettings.shadowSoftness,
        )
    }

    /** Inspection overlays: skeleton lines, flat shading (material check) and region highlight. */
    fun setOverlays(skeleton: Boolean, flatShading: Boolean, highlightRegion: Int) =
        NativeBridge.rendererSetOverlays(handle, skeleton, flatShading, highlightRegion)

    /** Plays a library clip. [loop] is meaningful for the looping clips (idle, walk, run...). */
    fun setClip(clip: Int, speed: Float = 1f, loop: Boolean = true) =
        NativeBridge.rendererSetClip(handle, clip, speed, loop)

    /** Draws one frame. Must be called on the render thread. */
    fun frame(deltaSeconds: Float) = NativeBridge.rendererFrame(handle, deltaSeconds)

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
        val region = NativeBridge.enginePickRegion(
            engineHandle,
            camera.yawDeg, camera.pitchDeg, camera.distance, camera.targetY, camera.fovDeg,
            ndcX, ndcY, viewWidth, viewHeight, point,
        )
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
