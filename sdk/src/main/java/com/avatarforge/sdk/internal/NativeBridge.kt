/*
 * AvatarForge :: NativeBridge.kt
 *
 * The one and only place where Kotlin meets C++. Every declaration here has a matching
 * `JNIEXPORT` in native/src/android/af_jni.cpp, and the pair is checked by the app's
 * instrumentation-free smoke test (sdk tests + the native harness).
 *
 * Rules this file follows, which the C++ side guarantees in return:
 *
 *   * handles are opaque Long values (0 = invalid). They are not garbage collected; the owner
 *     (AvatarEngine / AvatarRenderer) must call the matching destroy.
 *   * every call returns an af_result code: 0 = OK, negative = failure. The JNI layer throws an
 *     IllegalArgumentException / IllegalStateException instead of returning a code for programmer
 *     errors (null arrays, closed handles), so failures here are always real engine failures.
 *   * byte[] style copies are never made implicitly: vertex, index and texture transfers take a
 *     **direct** ByteBuffer, so a 34 MB mesh moves exactly once.
 *   * nothing in here touches the network, the file system or the JVM's global state.
 */
package com.avatarforge.sdk.internal

import android.view.Surface
import java.nio.ByteBuffer

internal object NativeBridge {

    /** True when libavatarforge.so (and its dependencies libafcore/libafrender) loaded. */
    val available: Boolean
    val loadError: String?

    init {
        var ok = false
        var err: String? = null
        try {
            System.loadLibrary("avatarforge")
            ok = true
        } catch (t: Throwable) {
            err = t.message ?: t.javaClass.simpleName
        }
        available = ok
        loadError = err
    }

    /* ------------------------------------------------------------------ library -------------- */

    external fun abiVersion(): Int
    external fun engineVersion(): String

    /* ------------------------------------------------------------------ engine lifetime ------ */
    /*
     * The specification travels as two flat arrays plus the prompt text - the same layout as
     * af_spec, in field order, so adding a field is a two-line change on each side. See
     * AvatarSpec.pack() for the authoritative writer.
     */
    external fun engineCreate(ints: IntArray, floats: FloatArray, prompt: String?, callbacks: Any?): Long
    external fun engineDestroy(handle: Long)
    external fun engineReset(handle: Long, ints: IntArray, floats: FloatArray, prompt: String?)
    external fun engineSetMemoryBudget(handle: Long, bytes: Long)
    external fun engineLastError(handle: Long): String

    /* ------------------------------------------------------------------ inputs --------------- */

    /** Returns the number of prompt attributes that matched (>= 0), or a negative af_result. */
    external fun engineAnalyzePrompt(
        handle: Long,
        prompt: String,
        outInts: IntArray,
        outFloats: FloatArray,
        outPrompt: Array<String?>,
    ): Int

    /**
     * 1..4 reference images. `pixels` must be **direct** ByteBuffers of width*height*channels bytes
     * in RGBA order (the kernel reads R,G,B and ignores A). `outAnalyses` receives 22 floats per
     * image - see ImageReference.Analysis for the field order.
     */
    external fun engineAnalyzeImages(
        handle: Long,
        widths: IntArray,
        heights: IntArray,
        channels: IntArray,
        roles: IntArray,
        pixels: Array<ByteBuffer?>,
        outAnalyses: FloatArray,
    ): Int

    /* ------------------------------------------------------------------ pipeline ------------- */

    external fun engineStage(handle: Long, stage: Int): Int
    external fun engineRunAll(handle: Long): Int
    external fun engineDirtyMask(handle: Long): Int

    /* ------------------------------------------------------------------ posing --------------- */

    external fun engineApplyPose(handle: Long, quats: FloatArray): Int
    external fun engineSampleClip(handle: Long, clip: Int, time: Float, outQuats: FloatArray): Int
    external fun engineApplyClip(handle: Long, clip: Int, time: Float): Int
    external fun engineApplyBlendshapes(handle: Long, weights: FloatArray): Int

    external fun engineBlendshapeNames(): Array<String>
    external fun engineClipCount(): Int
    external fun engineClipName(clip: Int): String
    external fun engineClipDuration(handle: Long, clip: Int): Float
    external fun engineRegionName(region: Int): String

    /* ------------------------------------------------------------------ mesh views ----------- */

    /** out = [vertexCount, indexCount, submeshCount, jointCount, materialCount]. */
    external fun engineCounts(handle: Long, out: IntArray): Int
    external fun engineVertexBytes(handle: Long): Long
    external fun engineCopyVertices(handle: Long, dst: ByteBuffer, rest: Boolean): Int
    external fun engineCopyIndices(handle: Long, dst: ByteBuffer): Int
    external fun engineSubmeshInfo(handle: Long, outInts: IntArray, outFloats: FloatArray): Int
    external fun engineMaterialInfo(handle: Long, outNames: Array<String?>, outFloats: FloatArray): Int
    external fun engineJointInfo(handle: Long, joint: Int, outInts: IntArray, outFloats: FloatArray, outName: Array<String?>): Int
    external fun engineJointMatrices(handle: Long, out: FloatArray): Int
    external fun engineTextureInfo(handle: Long, slot: Int, out: IntArray): Long
    external fun engineCopyTexture(handle: Long, slot: Int, dst: ByteBuffer): Int

    /* ------------------------------------------------------------------ output --------------- */

    external fun engineExportGlb(handle: Long, path: String): Int

    /** Pass a null destination to ask for the size, then a sized direct buffer to receive it. */
    external fun engineExportGlbBytes(handle: Long, dst: ByteBuffer?): Long

    external fun engineStats(handle: Long, outFloats: FloatArray, outLongs: LongArray): Int

    /** Camera-space picking: returns an af_region id, or -1 when the ray misses the avatar. */
    external fun enginePickRegion(
        handle: Long,
        yawDeg: Float,
        pitchDeg: Float,
        distance: Float,
        targetY: Float,
        fovDeg: Float,
        ndcX: Float,
        ndcY: Float,
        width: Int,
        height: Int,
        outPoint: FloatArray,
    ): Int

    /* ------------------------------------------------------------------ renderer ------------- */
    /*
     * One renderer per Surface. The renderer owns the GPU resources for that surface's lifetime
     * and must be created, used and destroyed from the same thread - the app does that on its
     * dedicated render thread (see RenderSurfaceView).
     */
    external fun rendererCreate(
        surface: Surface,
        quality: Int,
        preferVulkan: Boolean,
        msaa: Int,
        shadowSize: Int,
        bloomLevels: Int,
    ): Long

    external fun rendererDestroy(renderer: Long)
    external fun rendererResize(renderer: Long, width: Int, height: Int): Boolean
    external fun rendererAttachEngine(renderer: Long, handle: Long): Int
    external fun rendererSetCamera(
        renderer: Long,
        yaw: Float,
        pitch: Float,
        distance: Float,
        targetY: Float,
        fov: Float,
    )
    external fun rendererSetOptions(
        renderer: Long,
        quality: Int,
        shadows: Boolean,
        bloom: Boolean,
        sss: Boolean,
        exposure: Float,
        sunAzimuth: Float,
        sunElevation: Float,
        sunIntensity: Float,
        turbidity: Float,
        environmentIntensity: Float,
        groundPlane: Boolean,
        preferVulkan: Boolean,
        msaa: Int,
        shadowSize: Int,
        bloomLevels: Int,
        shadowSoftness: Float,
    )
    external fun rendererSetOverlays(renderer: Long, skeleton: Boolean, flat: Boolean, highlightRegion: Int)
    external fun rendererSetClip(renderer: Long, clip: Int, speed: Float, loop: Boolean)
    external fun rendererInvalidateMesh(renderer: Long)
    external fun rendererFrame(renderer: Long, deltaSeconds: Float)
    external fun rendererWaitIdle(renderer: Long)

    /** outStrings = [backend, device, apiVersion, driver]; outFlags = [shadows, msaa, hdr, aniso]. */
    external fun rendererInfo(renderer: Long, outStrings: Array<String?>, outFlags: IntArray): Int

    /** outFloats = [frameMs, cpuMs, fps, textureMiB]; outInts = [draws, tris, verts, shadow, msaa, bloom, quality]. */
    external fun rendererStats(renderer: Long, outFloats: FloatArray, outInts: IntArray): Int

    external fun rendererCurrentClip(renderer: Long): Int
    external fun rendererClipTime(renderer: Long): Float
}
