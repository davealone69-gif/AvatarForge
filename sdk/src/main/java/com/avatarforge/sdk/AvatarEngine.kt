/*
 * AvatarForge :: AvatarEngine.kt
 *
 * The SDK entry point. This is the surface a third-party app (or our own Compose UI) codes against:
 *
 *     AvatarEngine.health()                       -> is the engine present and usable?
 *     AvatarEngine.capabilities()                 -> what can this device do?
 *     engine.generateFromText(prompt)             -> JobHandle
 *     engine.generateFromImage(identity, extra)   -> JobHandle
 *     job.status / job.cancel() / job.result()    -> EngineResult
 *     engine.exportGlb(destination)               -> File in app-scoped storage
 *
 * Everything runs on-device. There is no network call anywhere in this file, no remote model, no
 * telemetry: the app works with the radio off, and the manifest does not request INTERNET.
 *
 * Threading model. The heavy stages (geometry, UV unwrap, texture bake, rig) run on Dispatchers.IO;
 * the SDK serializes complete native transactions because its kernel handle is a single mutable
 * pipeline. The kernel fans work out to its internal worker threads within a call. Progress and log
 * callbacks are published through StateFlow; the Compose layer collects them on the main dispatcher.
 */
package com.avatarforge.sdk

import android.content.Context
import android.graphics.Bitmap
import android.os.Build as AndroidBuild
import android.os.Debug
import android.os.StatFs
import com.avatarforge.sdk.internal.NativeBridge
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/* CIE L*a*b* -> linear sRGB, matching native/src/af_image.cpp::labToLinearRgb. */
private fun labToLinear(lab: FloatArray): FloatArray {
    val fy = (lab[0] + 16f) / 116f
    val fx = fy + lab[1] / 500f
    val fz = fy - lab[2] / 200f
    val d = 6f / 29f
    fun inv(f: Float) = if (f > d) f * f * f else 3f * d * d * (f - 4f / 29f)
    val x = inv(fx) * 0.95047f
    val y = inv(fy)
    val z = inv(fz) * 1.08883f
    val r = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z
    val g = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z
    val b = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z
    return floatArrayOf(r.coerceAtLeast(0f), g.coerceAtLeast(0f), b.coerceAtLeast(0f))
}


/* ============================================================================================ */
/*  Health and capabilities                                                                      */
/* ============================================================================================ */

/** Result of [AvatarEngine.health]: everything the UI needs to decide what to offer. */
data class EngineHealth(
    val nativeLibraryLoaded: Boolean,
    val abiVersion: Int,
    val engineVersion: String,
    /** Set when something is wrong; the UI shows this verbatim rather than a generic message. */
    val problem: String? = null,
) {
    val usable: Boolean get() = nativeLibraryLoaded && problem == null
}

/** What the device can do. Reported, never invented: [renderer] is only non-null once a real
 *  Surface has been handed to a renderer and it initialised. */
data class EngineCapabilities(
    val abiVersion: Int,
    val engineVersion: String,
    /** Largest .glb the engine will produce at the current settings, in bytes (measured, not guessed). */
    val estimatedGlbBytes: Long,
    /** Effective generation memory ceiling for this RAM snapshot, bytes; capped by the requested 1.5 GiB default and live headroom. */
    val memoryBudgetBytes: Long,
    val totalRamBytes: Long,
    val lowRamDevice: Boolean,
    /** Free bytes in the app's private storage (the export target). */
    val freeStorageBytes: Long,
    val clipCount: Int,
    val blendshapeCount: Int,
    val regionCount: Int,
)

/* ============================================================================================ */
/*  Progress                                                                                     */
/* ============================================================================================ */

/** One pipeline stage, matching `af_stage` in the C ABI. */
enum class Stage(val value: Int, val label: String) {
    IDLE(0, "Idle"),
    ANALYZE_INPUTS(1, "Interpreting input"),
    FIT_ANATOMY(2, "Fitting anatomy"),
    GEOMETRY(3, "Building geometry"),
    UV_UNWRAP(4, "Unwrapping UVs"),
    BAKE_TEXTURES(5, "Baking PBR textures"),
    RIG(6, "Auto-rigging"),
    BLENDSHAPES(7, "Baking facial shapes"),
    ANIMATION(8, "Baking clips"),
    EXPORT(9, "Writing .glb"),
    DONE(10, "Done");

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: IDLE
        val ALL = entries.filter { it != IDLE && it != DONE }
    }
}

/**
 * Process-lifetime generation dependency graph. The ABI stages are explicit graph nodes, not a
 * monolithic call: geometry gates UV, UV gates the texture/rig inputs, and rig gates facial and
 * clip baking. The current engine handle is single-threaded, so ready siblings execute in stable
 * order; the dependency edges are retained for safe scheduling once independent engine contexts
 * can be used. `runAll` is still called afterward as a cheap native finalize/consistency barrier.
 */
internal object GenerationJobGraph {
    internal data class Node(val stage: Stage, val requires: Set<Stage>)

    val nodes: List<Node> = listOf(
        Node(Stage.FIT_ANATOMY, emptySet()),
        Node(Stage.GEOMETRY, setOf(Stage.FIT_ANATOMY)),
        Node(Stage.UV_UNWRAP, setOf(Stage.GEOMETRY)),
        Node(Stage.BAKE_TEXTURES, setOf(Stage.UV_UNWRAP)),
        Node(Stage.RIG, setOf(Stage.UV_UNWRAP)),
        Node(Stage.BLENDSHAPES, setOf(Stage.RIG)),
        Node(Stage.ANIMATION, setOf(Stage.RIG)),
    )

    fun orderedStages(): List<Stage> {
        val pending = nodes.toMutableList()
        val completed = linkedSetOf<Stage>()
        val ordered = ArrayList<Stage>(pending.size)
        while (pending.isNotEmpty()) {
            val readyIndex = pending.indexOfFirst { completed.containsAll(it.requires) }
            check(readyIndex >= 0) { "generation dependency graph contains a cycle or missing prerequisite" }
            val node = pending.removeAt(readyIndex)
            check(completed.add(node.stage)) { "generation graph contains duplicate stage ${node.stage}" }
            ordered += node.stage
        }
        return ordered
    }
}

/** Converts absolute stage-progress callbacks into monotone weighted whole-job progress. */
internal class WeightedProgressAccumulator(private val weights: Map<Stage, Float>) {
    private val completedFraction = mutableMapOf<Stage, Float>()
    var overall: Float = 0f
        private set

    fun reset() {
        completedFraction.clear()
        overall = 0f
    }

    fun update(stage: Stage, reportedFraction: Float): Float {
        val safe = reportedFraction.coerceIn(0f, 1f)
        val previous = completedFraction[stage] ?: 0f
        val monotone = maxOf(previous, safe)
        completedFraction[stage] = monotone
        overall = (overall + (weights[stage] ?: 0f) * (monotone - previous)).coerceIn(0f, 1f)
        return overall
    }
}

/** Live progress of a generation job. */
data class JobStatus(
    val stage: Stage = Stage.IDLE,
    /** 0..1 within the current stage. */
    val stageProgress: Float = 0f,
    /** 0..1 across the whole pipeline, weighted by measured stage cost. */
    val overallProgress: Float = 0f,
    val message: String = "",
    val running: Boolean = false,
    val cancelled: Boolean = false,
    val failed: Boolean = false,
    val error: String? = null,
    /** Wall-clock milliseconds spent so far. */
    val elapsedMs: Long = 0L,
)

/* ============================================================================================ */
/*  Result                                                                                       */
/* ============================================================================================ */

/** Everything a finished generation produced. */
data class EngineResult(
    /** The .glb written to app-scoped storage, ready to share through the FileProvider. */
    val glbFile: File,
    val spec: AvatarSpec,
    val vertexCount: Int,
    val triangleCount: Int,
    val submeshCount: Int,
    val jointCount: Int,
    val blendshapeCount: Int,
    val textureBytes: Long,
    val timings: Map<Stage, Float>,
    /** Region id -> display name, for the viewer's inspector. */
    val regionNames: List<String>,
) {
    val glbBytes: Long get() = glbFile.length()
}

/* ============================================================================================ */
/*  The job                                                                                      */
/* ============================================================================================ */

/**
 * A running (or finished) generation.
 *
 * `status` is a StateFlow so the UI can collect it; `result()` returns the finished result or
 * throws. Cancellation requests are cooperative and land at the kernel's stage boundaries - a
 * half-built mesh is never handed to the renderer, because the kernel reports the cancellation and
 * the job releases the handle.
 */
class JobHandle internal constructor(
    private val engine: AvatarEngine,
    private val request: GenerationRequest,
    internal val parentJob: Job,
) {
    private val _status = MutableStateFlow(JobStatus(running = true, stage = Stage.ANALYZE_INPUTS))
    val status: StateFlow<JobStatus> = _status.asStateFlow()

    private val cancelRequested = AtomicBoolean(false)
    private val completion = CompletableDeferred<EngineResult>()

    /** Stable id for UI history; also names the exported file. */
    val id: Long = nextJobId.getAndIncrement()

    /** Requests cancellation. The kernel polls this at every stage boundary. */
    fun cancel() {
        cancelRequested.set(true)
        parentJob.cancel()
    }

    val isCancelled: Boolean get() = cancelRequested.get()

    /** Suspends until the job finishes, then returns the result or throws the failure. */
    suspend fun await(): EngineResult = completion.await()

    internal fun update(status: JobStatus) {
        _status.value = status
    }

    internal fun current(): JobStatus = _status.value

    internal val isCompleted: Boolean get() = completion.isCompleted

    internal val requestSpec: AvatarSpec get() = request.spec

    internal fun deliver(result: Result<EngineResult>) {
        _status.value = _status.value.copy(running = false)
        result.fold({ completion.complete(it) }, { completion.completeExceptionally(it) })
    }

    internal fun shouldCancel(): Boolean = cancelRequested.get() || parentJob.isCancelled

    companion object {
        private val nextJobId = AtomicLong(1L)
    }
}

/* ============================================================================================ */
/*  Generation request                                                                           */
/* ============================================================================================ */

/**
 * One generation request: a spec plus the optional reference images.
 *
 * The current two-image flow performs deterministic, coarse classical analysis. Image A supplies
 * subject framing, an optional full-body stature estimate, and median skin/hair/garment colours.
 * Image B can override garment colour and metal confidence, selecting a generic outfit class. It
 * does **not** reconstruct or preserve a face, fit a mesh to facial landmarks, copy garment
 * geometry, project reference pixels into UVs, or fuse calibrated views. The enum name IDENTITY is
 * retained for ABI compatibility; it describes caller intent, not a supported identity embedding.
 */
data class GenerationRequest(
    val spec: AvatarSpec,
    val prompt: String = "",
    val imageA: ImageReference? = null,
    val imageB: ImageReference? = null,
    /** Requested kernel memory ceiling. Default 1.5 GiB; the SDK may lower it to respect live RAM headroom. */
    val memoryBudgetBytes: Long = 1536L * 1024 * 1024,
)

/**
 * A decoded reference image.
 *
 * [pixels] is a direct ByteBuffer in RGBA order - the kernel never copies it, and reads R, G, B.
 * [Analysis] is what the kernel reported back, so the UI can show the user the measurements it
 * extracted (and let them correct a bad estimate) instead of silently building the wrong person.
 */
class ImageReference private constructor(
    val role: ImageRole,
    val width: Int,
    val height: Int,
    val pixels: ByteBuffer,
    val analysis: Analysis?,
) {
    data class Analysis(
        val subjectCoverage: Float,
        val headBox: FloatArray,
        val shoulderY: Float,
        val hipY: Float,
        val kneeY: Float,
        val ankleY: Float,
        val isFullBody: Boolean,
        val poseConfidence: Float,
        val skinLab: FloatArray,
        val hairLab: FloatArray,
        val garmentLab: FloatArray,
        val metalConfidence: Float,
        val keyLight: FloatArray,
        val keyLightIntensity: Float,
        val backgroundLab: FloatArray,
    )

    companion object {
        /** Bytes needed for a direct buffer holding this bitmap. */
        fun byteSize(width: Int, height: Int): Int = width * height * 4

        /**
         * Copies a Bitmap into a direct RGBA buffer.
         *
         * Direct, not heap: a 12 MP photo is 48 MB and the JNI layer passes the address straight to
         * the kernel. Anything else would double the peak memory for no benefit. The SDK requests
         * 1.5 GiB of native budget by default, then lowers the cap from a live RAM snapshot.
         */
        fun fromBitmap(bitmap: Bitmap, role: ImageRole): ImageReference {
            val w = bitmap.width
            val h = bitmap.height
            val buffer = ByteBuffer.allocateDirect(byteSize(w, h)).order(ByteOrder.nativeOrder())
            bitmap.copyPixelsToBuffer(buffer)
            buffer.rewind()
            return ImageReference(role, w, h, buffer, null)
        }

        internal fun withAnalysis(base: ImageReference, analysis: Analysis) =
            ImageReference(base.role, base.width, base.height, base.pixels, analysis)
    }
}

/** Pure Kotlin mirror of native `af::specFromAnalysis`, kept separate for deterministic JVM tests. */
internal object ImageSpecResolver {
    /** Mirrors native role selection: the last explicit identity/clothing role wins. */
    fun resolveImages(
        images: List<Pair<ImageRole, ImageReference.Analysis?>>,
        base: AvatarSpec,
    ): AvatarSpec {
        val identity = images.lastOrNull {
            it.first == ImageRole.IDENTITY && it.second != null
        }?.second ?: images.firstOrNull { it.second != null }?.second
        val clothing = images.lastOrNull {
            it.first == ImageRole.CLOTHING && it.second != null
        }?.second
        return resolve(identity, clothing, base)
    }

    fun resolve(
        identity: ImageReference.Analysis?,
        clothing: ImageReference.Analysis?,
        base: AvatarSpec,
    ): AvatarSpec {
        var spec = if (base.heightM.isFinite() && base.heightM > 0f) base
                   else base.copy(heightM = AvatarSpec.DEFAULT_HEIGHT_M)
        val colourSource = clothing ?: identity

        if (identity != null) {
            val unresolvedHeight = !spec.heightM.isFinite() || spec.heightM <= 0f ||
                kotlin.math.abs(spec.heightM - AvatarSpec.DEFAULT_HEIGHT_M) < 1e-5f
            val headHeight = identity.headBox[3].coerceAtLeast(0.01f)
            val statureInFrame = identity.ankleY - identity.headBox[1]
            if (unresolvedHeight && identity.isFullBody && statureInFrame > 0.05f) {
                val measured = (0.235f * (statureInFrame / headHeight))
                    .coerceIn(AvatarSpec.MIN_HEIGHT_M, AvatarSpec.MAX_HEIGHT_M)
                val confidence = ((identity.poseConfidence - 0.45f) / 0.5f).coerceIn(0f, 1f)
                spec = spec.copy(
                    heightM = confidence * measured +
                        (1f - confidence) * AvatarSpec.DEFAULT_HEIGHT_M,
                )
            }

            val referenceLightness = identity.backgroundLab[0].coerceAtLeast(22f)
            val exposureRatio = (identity.skinLab[0] / referenceLightness).coerceIn(0.15f, 1.9f)
            val melanin = (0.62f - 0.52f * exposureRatio).coerceIn(0.03f, 0.95f)
            val hair = labToLinear(identity.hairLab)
                .map { (it * 1.15f).coerceIn(0.01f, 1f) }
                .toFloatArray()
            spec = spec.copy(skinMelanin = melanin, hairTone = hair)
        }

        if (colourSource != null) {
            val colour = labToLinear(colourSource.garmentLab)
            val cloth = colour.map { it.coerceIn(0.01f, 1f) }.toFloatArray()
            val gear = colour.map { (it * 1.05f).coerceIn(0.01f, 1f) }.toFloatArray()
            val metal = (clothing ?: identity)?.metalConfidence ?: 0f
            var outfit = spec.outfit
            var footwear = spec.footwear
            if (outfit == Outfit.NONE || outfit == Outfit.TSHIRT) {
                when {
                    metal > 0.62f -> {
                        outfit = Outfit.ARMOR_PLATE
                        if (footwear == Footwear.NONE) footwear = Footwear.SABATON
                    }
                    metal > 0.45f -> {
                        outfit = Outfit.ARMOR_LEATHER
                        if (footwear == Footwear.NONE) footwear = Footwear.BOOT
                    }
                    outfit == Outfit.NONE -> outfit = Outfit.TSHIRT
                }
            }
            spec = spec.copy(outfit = outfit, footwear = footwear, clothTone = cloth, gearTone = gear)
        }
        return spec
    }
}

/**
 * Versioned wire codec shared by image analysis allocation and unpacking.
 *
 * JNI reports its native stride before every call. A mismatch is rejected before a buffer is
 * allocated, so a future schema edit cannot regress into a native write overrun or a Kotlin read
 * past the end. The order mirrors af_image_analysis and includes all three background-Lab values.
 */
internal object ImageAnalysisPacking {
    const val FLOAT_COUNT = 28

    fun unpack(packed: FloatArray, index: Int): ImageReference.Analysis {
        require(index >= 0) { "analysis index must be non-negative" }
        val start = Math.multiplyExact(index, FLOAT_COUNT)
        require(start <= packed.size - FLOAT_COUNT) {
            "analysis record $index needs $FLOAT_COUNT floats at $start, but array length is ${packed.size}"
        }

        var i = start
        fun f() = packed[i++]
        val result = ImageReference.Analysis(
            subjectCoverage = f(),
            headBox = floatArrayOf(f(), f(), f(), f()),
            shoulderY = f(),
            hipY = f(),
            ankleY = f(),
            kneeY = f(),
            isFullBody = f() > 0.5f,
            poseConfidence = f(),
            skinLab = floatArrayOf(f(), f(), f()),
            hairLab = floatArrayOf(f(), f(), f()),
            garmentLab = floatArrayOf(f(), f(), f()),
            metalConfidence = f(),
            keyLight = floatArrayOf(f(), f(), f()),
            keyLightIntensity = f(),
            backgroundLab = floatArrayOf(f(), f(), f()),
        )
        check(i == start + FLOAT_COUNT) {
            "analysis decoder consumed ${i - start} floats, expected $FLOAT_COUNT"
        }
        return result
    }
}

/** A reference-counted lease that keeps a native engine handle alive for one renderer attachment. */
internal class NativeEngineLease(
    val handle: Long,
    private val releaseAction: () -> Unit,
) {
    private val closed = AtomicBoolean(false)
    fun release() {
        if (closed.compareAndSet(false, true)) releaseAction()
    }
}

/* ============================================================================================ */
/*  The engine                                                                                   */
/* ============================================================================================ */

class AvatarEngine private constructor(private val appContext: Context) {

    private val lifecycleLock = Any()
    private val callbackLock = Any()
    private val nativeMutex = Mutex()
    private val rootJob = SupervisorJob()
    private val scope = CoroutineScope(rootJob + Dispatchers.IO)

    /** Every live job, so release can cancel and join before the native handle is destroyed. */
    private val jobs = mutableListOf<JobHandle>()

    private var handle: Long = 0L
    private var released = false
    private var rendererLeases = 0
    private var destroyScheduled = false

    init {
        rootJob.invokeOnCompletion { scheduleDestroyIfReady() }
    }

    /**
     * Raw native pointer for low-level integrations. Prefer [com.avatarforge.sdk.render.RenderSurfaceView.attachEngine]
     * so the view can retain the handle until its renderer is destroyed. Calls through this pointer
     * are not synchronized; the caller must not overlap them with engine jobs or rendering.
     */
    val nativeHandle: Long get() = openHandle()

    /** Internal lifetime lease used by RenderSurfaceView while a GPU renderer references this handle. */
    internal fun acquireRendererLease(): NativeEngineLease {
        check(nativeMutex.tryLock()) { "AvatarEngine is busy; attach a renderer after native work finishes" }
        try {
            return synchronized(lifecycleLock) {
                check(!released) { "AvatarEngine has been released" }
                check(rendererLeases == 0) {
                    "AvatarEngine supports one attached renderer; detach the current view before sharing it"
                }
                val h = openHandleLocked()
                rendererLeases++
                NativeEngineLease(h) { releaseRendererHandle() }
            }
        } finally {
            nativeMutex.unlock()
        }
    }

    /** Releases the renderer's lifetime lease after its native renderer has stopped using the handle. */
    internal fun releaseRendererHandle() {
        synchronized(lifecycleLock) {
            if (rendererLeases > 0) rendererLeases--
        }
        scheduleDestroyIfReady()
    }

    /* ------------------------------------------------------------------ posing ---------------- */

    /**
     * Applies local joint rotations (quaternions XYZW, jointCount * 4). Requires the renderer to be
     * detached; a live renderer owns animation and mesh access on its render thread.
     */
    fun applyPose(quats: FloatArray): Boolean =
        withNativeHandle { h -> NativeBridge.engineApplyPose(h, quats) == 0 }

    /** Fills [out] with the clip's pose at [time] seconds without applying it. */
    fun sampleClip(clip: Int, time: Float, out: FloatArray): Boolean =
        withNativeHandle { h -> NativeBridge.engineSampleClip(h, clip, time, out) == 0 }

    /**
     * Applies a clip pose at [time] to the mesh. AF_CLIP_REST re-derives the bind pose. Requires
     * the renderer to be detached; use [com.avatarforge.sdk.render.RenderSurfaceView.setClip] for
     * playback while rendering.
     */
    fun applyClip(clip: Int, time: Float): Boolean =
        withNativeHandle { h -> NativeBridge.engineApplyClip(h, clip, time) == 0 }

    /**
     * Applies the 52 ARKit blendshape weights (0..1 each), in [blendshapeNames] order.
     * This direct method requires the renderer to be detached. When a renderer is attached, call
     * this through [RenderSurfaceView.applyBlendshapes] so mutation runs between frames.
     */
    fun applyBlendshapes(weights: FloatArray): Boolean =
        withNativeHandle { h -> NativeBridge.engineApplyBlendshapes(h, weights) == 0 }

    /** Render-thread variant; the attached renderer is the only permitted concurrent lease. */
    internal fun applyBlendshapesFromRenderer(weights: FloatArray): Boolean =
        withNativeHandle(allowRendererLease = true) { h -> NativeBridge.engineApplyBlendshapes(h, weights) == 0 }

    /** Mesh counters: [vertices, indices, submeshes, joints, materials]. */
    fun counts(): IntArray = withNativeHandle { h ->
        IntArray(5).also { NativeBridge.engineCounts(h, it) }
    }

    /** Human-readable name of an `af_region` id, straight from the kernel's own table. */
    fun regionName(region: Int): String =
        if (region < 0) "none" else NativeBridge.engineRegionName(region)

    fun clipName(clip: Int): String = NativeBridge.engineClipName(clip)

    /** Id of a clip by its authored name ("walk", "t_pose", "rest", ...), or -1 if unknown.
     *  Names are the stable contract; ids are an implementation detail of the clip library. */
    fun clipIndex(name: String): Int {
        for (c in 0 until clipCount) if (NativeBridge.engineClipName(c).equals(name, ignoreCase = true)) return c
        return -1
    }

    fun clipDuration(clip: Int): Float =
        withNativeHandle { h -> NativeBridge.engineClipDuration(h, clip) }

    fun blendshapeNames(): List<String> = NativeBridge.engineBlendshapeNames().toList()

    val clipCount: Int get() = NativeBridge.engineClipCount()

    /* ------------------------------------------------------------------ lifecycle ------------ */

    private fun openHandle(): Long = synchronized(lifecycleLock) {
        check(!released) { "AvatarEngine has been released" }
        openHandleLocked()
    }

    private fun isReleased(): Boolean = synchronized(lifecycleLock) { released }

    /** lifecycleLock must be held by the caller. */
    private fun openHandleLocked(): Long {
        if (handle == 0L) {
            check(NativeBridge.available) {
                "AvatarForge native library is not loaded: ${NativeBridge.loadError ?: "unknown reason"}"
            }
            handle = NativeBridge.engineCreate(
                AvatarSpec().packInts(),
                AvatarSpec().packFloats(),
                "",
                callbackBridge,
            )
            check(handle != 0L) { "the kernel refused to create an engine: ${NativeBridge.engineLastError(0L)}" }
        }
        return handle
    }

    /**
     * Runs one synchronous native operation without allowing it to overlap a generation/export
     * transaction or a renderer's direct C-ABI reads. Synchronous callers fail fast while the engine
     * is busy or renderer-leased instead of blocking the UI or racing the single-slot native state.
     */
    private fun <T> withNativeHandle(
        allowRendererLease: Boolean = false,
        block: (Long) -> T,
    ): T {
        check(nativeMutex.tryLock()) { "AvatarEngine is busy; retry after the active native operation finishes" }
        try {
            val h = synchronized(lifecycleLock) {
                check(!released) { "AvatarEngine has been released" }
                check(if (allowRendererLease) rendererLeases == 1 else rendererLeases == 0) {
                    if (allowRendererLease) {
                        "render-thread native mutation requires an active SDK-managed renderer lease"
                    } else {
                        "AvatarEngine has an attached renderer; detach it before direct native operations"
                    }
                }
                openHandleLocked()
            }
            return block(h)
        } finally {
            nativeMutex.unlock()
        }
    }

    /**
     * Requests shutdown. Native generation is cooperative-cancelled, jobs are joined, and renderer
     * leases are honoured before `engineDestroy` runs. This is deliberately asynchronous so an
     * Activity's main thread never blocks for a long geometry stage; new calls fail immediately.
     * Low-level users of [nativeHandle] must detach their renderer before releasing the engine.
     */
    fun release() {
        synchronized(lifecycleLock) {
            if (released) return
            released = true
        }
        synchronized(jobs) { jobs.toList() }.forEach { it.cancel() }
        rootJob.cancel(CancellationException("AvatarEngine released"))
        scheduleDestroyIfReady()
    }

    /** Schedules destruction only after all coroutine jobs and renderer leases have ended. */
    private fun scheduleDestroyIfReady() {
        val shouldSchedule = synchronized(lifecycleLock) {
            if (released && rootJob.isCompleted && rendererLeases == 0 && handle != 0L && !destroyScheduled) {
                destroyScheduled = true
                true
            } else {
                false
            }
        }
        if (!shouldSchedule) return

        CoroutineScope(Dispatchers.IO).launch {
            nativeMutex.withLock {
                val doomed = synchronized(lifecycleLock) {
                    if (released && rootJob.isCompleted && rendererLeases == 0) {
                        handle.also { handle = 0L }
                    } else {
                        destroyScheduled = false
                        0L
                    }
                }
                if (doomed != 0L) NativeBridge.engineDestroy(doomed)
            }
        }
    }

    /* ------------------------------------------------------------------ generation ----------- */

    fun generateFromText(prompt: String, spec: AvatarSpec = AvatarSpec(), memoryBudgetBytes: Long = 1536L * 1024 * 1024): JobHandle {
        val resolved = parsePrompt(prompt, spec)
        return start(GenerationRequest(resolved, prompt, null, null, memoryBudgetBytes))
    }

    /**
     * The two-image coarse-analysis flow.
     *
     * [identity] is the ABI-stable name for Image A, which currently contributes only framing,
     * approximate full-body stature when available, and sampled appearance colours. [detail] is
     * Image B, which contributes garment colour/metal confidence and may select a generic outfit.
     * Neither path reconstructs identity or reference garment geometry. The measured values are
     * handed to [specFromImages] so the caller can inspect or override them before building.
     */
    fun generateFromImage(
        identity: ImageReference,
        detail: ImageReference? = null,
        spec: AvatarSpec = AvatarSpec(),
        prompt: String = "",
        memoryBudgetBytes: Long = 1536L * 1024 * 1024,
    ): JobHandle {
        val analysed = analyseImages(listOfNotNull(identity, detail))
        val withSpec = specFromImages(analysed, spec)
        val request = GenerationRequest(
            withSpec,
            prompt,
            analysed.firstOrNull(),
            analysed.getOrNull(1),
            memoryBudgetBytes,
        )
        return start(request)
    }

    /**
     * Starts a generation from an explicit request. The two convenience entry points above build
     * one of these; the UI uses this directly when it has already shown the user the measurements it
     * extracted and wants to build exactly the spec that was confirmed.
     */
    fun generate(request: GenerationRequest): JobHandle = start(request)

    /** Analyses reference images without starting a build - used by the UI's "what did you see?" step. */
    fun analyseImages(images: List<ImageReference>): List<ImageReference> =
        withNativeHandle { h -> analyseImagesWithHandle(h, images) }

    /** Caller already owns nativeMutex for this single-slot engine operation. */
    private fun analyseImagesWithHandle(h: Long, images: List<ImageReference>): List<ImageReference> {
        require(images.isNotEmpty() && images.size <= 4) { "1 to 4 reference images are supported" }
        val n = images.size
        val widths = IntArray(n) { images[it].width }
        val heights = IntArray(n) { images[it].height }
        val channels = IntArray(n) { 4 }
        val roles = IntArray(n) { images[it].role.value }
        val pixels = Array<ByteBuffer?>(n) { images[it].pixels }
        val nativeStride = NativeBridge.imageAnalysisFloatCount()
        check(nativeStride == ImageAnalysisPacking.FLOAT_COUNT) {
            "native image-analysis stride $nativeStride does not match SDK stride ${ImageAnalysisPacking.FLOAT_COUNT}"
        }
        val packed = FloatArray(Math.multiplyExact(n, nativeStride))
        val rc = NativeBridge.engineAnalyzeImages(h, widths, heights, channels, roles, pixels, packed)
        check(rc == n) {
            "image analysis produced $rc of $n records: ${NativeBridge.engineLastError(h)}"
        }
        return images.mapIndexed { index, image ->
            ImageReference.withAnalysis(image, ImageAnalysisPacking.unpack(packed, index))
        }
    }

    /**
     * Turns image analyses into the same canonical spec the native analyser will apply during
     * generation. Keep this logic in parity with `af::specFromAnalysis`: generation resets the
     * engine and re-analyses the pixels, so a different Kotlin estimate would make result metadata
     * disagree with the mesh that was actually built.
     */
    fun specFromImages(images: List<ImageReference>, base: AvatarSpec): AvatarSpec =
        ImageSpecResolver.resolveImages(images.map { it.role to it.analysis }, base)

    /** Runs the prompt parser only, and returns the resolved spec plus the match count. */
    fun parsePrompt(prompt: String, base: AvatarSpec = AvatarSpec()): AvatarSpec = withNativeHandle { h ->
        val resolvedBase = if (base.heightM.isFinite() && base.heightM > 0f) base
                           else base.copy(heightM = AvatarSpec.DEFAULT_HEIGHT_M)
        /* Prompt analysis mutates the native handle. Reset first so a previous generation cannot
           leak its stature or measurements into this parse. */
        val resetRc = NativeBridge.engineReset(
            h, resolvedBase.packInts(), resolvedBase.packFloats(), resolvedBase.prompt,
        )
        check(resetRc == 0) { "prompt setup failed: ${NativeBridge.engineLastError(h)}" }
        val ints = resolvedBase.packInts()
        val floats = resolvedBase.packFloats()
        val outPrompt = arrayOfNulls<String>(1)
        val matched = NativeBridge.engineAnalyzePrompt(h, prompt, ints, floats, outPrompt)
        check(matched >= 0) { "prompt analysis failed: ${NativeBridge.engineLastError(h)}" }
        val spec = AvatarSpec.unpack(ints, floats, outPrompt[0] ?: prompt)
        /* Analysis is observational: keep the caller's explicit choices unless the prompt named a
           value the caller left at its default. */
        mergePrompted(resolvedBase, spec)
    }

    /* ------------------------------------------------------------------ export ---------------- */

    /** Writes the current result to [destination] (app-scoped storage) and returns the file. */
    fun exportGlb(source: EngineResult, destination: File): File = withNativeHandle { h ->
        requery(h, source.spec)
        destination.parentFile?.mkdirs()
        val rc = NativeBridge.engineExportGlb(h, destination.absolutePath)
        check(rc == 0) { "GLB export failed: ${NativeBridge.engineLastError(h)}" }
        destination
    }

    /** Exports the same .glb to memory: the SDK contract offers both, and callers embedding the
     *  model in another container need the bytes without a second file. */
    fun exportGlbBytes(source: EngineResult): ByteArray = withNativeHandle { h ->
        requery(h, source.spec)
        val size = NativeBridge.engineExportGlbBytes(h, null)
        check(size > 0) { "GLB export failed: ${NativeBridge.engineLastError(h)}" }
        val buffer = ByteBuffer.allocateDirect(size.toInt()).order(ByteOrder.nativeOrder())
        val written = NativeBridge.engineExportGlbBytes(h, buffer)
        check(written == size) { "GLB export produced $written bytes, expected $size" }
        val out = ByteArray(size.toInt())
        buffer.rewind()
        buffer.get(out)
        out
    }

    /* ------------------------------------------------------------------ internals ------------- */

    private fun start(unresolvedRequest: GenerationRequest): JobHandle {
        val request = if (unresolvedRequest.spec.heightM.isFinite() && unresolvedRequest.spec.heightM > 0f)
            unresolvedRequest
        else unresolvedRequest.copy(spec = unresolvedRequest.spec.copy(heightM = AvatarSpec.DEFAULT_HEIGHT_M))

        /* Parent every generation under rootJob. Supplying an unrelated Job to launch() would make
           scope cancellation miss it, which used to let release destroy a handle under a running job. */
        val parent = synchronized(lifecycleLock) {
            check(!released) { "AvatarEngine has been released" }
            check(rendererLeases == 0) {
                "Detach the renderer before starting generation; the engine mesh is a single-slot native resource"
            }
            Job(rootJob)
        }
        val handleObj = JobHandle(this, request, parent)
        synchronized(jobs) { jobs += handleObj }

        parent.invokeOnCompletion { cause ->
            /* A cancellation can happen before the coroutine body is dispatched. Complete the
               public Deferred in that case too, or await() would remain suspended forever. */
            if (!handleObj.isCompleted) {
                val cancelled = cause is CancellationException || handleObj.shouldCancel()
                val failure = cause ?: if (cancelled) CancellationException("generation cancelled")
                    else IllegalStateException("generation ended without a result")
                handleObj.update(
                    handleObj.current().copy(
                        running = false,
                        cancelled = cancelled,
                        failed = !cancelled,
                        error = if (cancelled) null else failure.message,
                        message = if (cancelled) "Cancelled" else failure.message ?: "failed",
                    ),
                )
                handleObj.deliver(Result.failure(failure))
            }
            synchronized(jobs) { jobs.remove(handleObj) }
        }

        scope.launch(parent) {
            val started = System.currentTimeMillis()
            val timings = LinkedHashMap<Stage, Float>()
            try {
                request.spec.requireGeneratable(request.prompt)
                check(NativeBridge.available) {
                    "AvatarForge native library is not loaded: ${NativeBridge.loadError ?: "unknown reason"}"
                }

                /* One native engine is a single-slot pipeline. Hold the lock across reset, image
                   analysis, generation, stats and export so two requests can never overwrite one
                   another's mesh or callback/progress state. */
                val result = nativeMutex.withLock {
                    if (handleObj.shouldCancel()) throw CancellationException("generation cancelled")
                    synchronized(lifecycleLock) {
                        check(rendererLeases == 0) {
                            "A renderer attached before generation acquired the native engine; detach it and retry"
                        }
                    }
                    currentStage = handleObj
                    try {
                        synchronized(callbackLock) {
                            progressAccumulator.reset()
                            lastProgressMs = 0L
                            logLines.clear()
                        }

                        val resources = DeviceResourceGovernor(appContext).snapshot()
                        val deferredReason = when {
                            resources.lowMemory || resources.state == DeviceResourceState.LOW_MEMORY -> "system memory is low"
                            resources.state == DeviceResourceState.LOW_STORAGE -> "free storage is low"
                            resources.state == DeviceResourceState.THERMAL_LIMIT -> "the device is thermally constrained"
                            else -> null
                        }
                        check(!resources.generationShouldBeDeferred) {
                            "Generation postponed because $deferredReason; free RAM ${resources.availableRamBytes / 1048576} MB, " +
                                "free storage ${resources.availableStorageBytes / 1048576} MB. Retry when the device recovers."
                        }
                        if (handleObj.shouldCancel()) throw CancellationException("generation cancelled")
                        val safeBudget = request.memoryBudgetBytes.coerceAtMost(
                            (resources.availableRamBytes - GENERATION_HEADROOM_BYTES).coerceAtLeast(64L * 1024 * 1024),
                        )
                        val h = openHandle()
                        val budgetRc = NativeBridge.engineSetMemoryBudget(h, safeBudget)
                        check(budgetRc == 0) { "memory budget rejected: ${NativeBridge.engineLastError(h)}" }
                        val resetRc = NativeBridge.engineReset(
                            h, request.spec.packInts(), request.spec.packFloats(), request.prompt,
                        )
                        check(resetRc == 0) { "generation setup failed: ${NativeBridge.engineLastError(h)}" }
                        if (handleObj.shouldCancel()) throw CancellationException("generation cancelled")

                        /* Images go in before the prompt so the prompt can override what the photo implied:
                           the user typed the prompt last, so it wins. */
                        val hasImages = request.imageA != null || request.imageB != null
                        if (hasImages) {
                            analyseImagesWithHandle(h, listOfNotNull(request.imageA, request.imageB))
                        }

                        /* The native image analyser mutates its canonical spec. Parse the prompt after
                           it so typed attributes win, then report the exact spec that the kernel will
                           build (not a Kotlin-side approximation). */
                        val resultSpec = if (hasImages || request.prompt.isNotBlank()) {
                            val specInts = IntArray(request.spec.packInts().size)
                            val specFloats = FloatArray(request.spec.packFloats().size)
                            val outPrompt = arrayOfNulls<String>(1)
                            val matched = NativeBridge.engineAnalyzePrompt(
                                h, request.prompt, specInts, specFloats, outPrompt,
                            )
                            check(matched >= 0) { "prompt analysis failed: ${NativeBridge.engineLastError(h)}" }
                            AvatarSpec.unpack(specInts, specFloats, outPrompt[0] ?: request.prompt)
                        } else {
                            request.spec
                        }
                        if (handleObj.shouldCancel()) throw CancellationException("generation cancelled")

                        /* Execute each native stage through the SDK's persistent dependency graph.
                           That creates cancellation/progress boundaries between long stages without
                           allowing concurrent calls against the single mutable engine handle. */
                        for (stage in GenerationJobGraph.orderedStages()) {
                            if (handleObj.shouldCancel()) throw CancellationException("generation cancelled")
                            handleObj.update(
                                handleObj.current().copy(
                                    stage = stage,
                                    stageProgress = 0f,
                                    message = stage.label,
                                    running = true,
                                ),
                            )
                            val stageRc = NativeBridge.engineStage(h, stage.value)
                            if (stageRc == CANCELLED || handleObj.shouldCancel()) {
                                throw CancellationException("generation cancelled")
                            }
                            check(stageRc == 0) {
                                "${stage.label} failed (${NativeBridge.engineLastError(h)})"
                            }
                        }
                        /* runAll now has no dirty work; it validates the stage chain, publishes the
                           canonical DONE state and updates summary statistics. */
                        val rc = NativeBridge.engineRunAll(h)
                        if (rc == CANCELLED || handleObj.shouldCancel()) {
                            throw CancellationException("generation cancelled")
                        }
                        check(rc == 0) { "generation finalization failed (${NativeBridge.engineLastError(h)})" }

                        /* Collect the statistics the result reports. */
                        val statsFloats = FloatArray(9)
                        val statsLongs = LongArray(11)
                        NativeBridge.engineStats(h, statsFloats, statsLongs)

                        val regions = (0 until REGION_COUNT).map { NativeBridge.engineRegionName(it) }
                        val file = File(engineOutputDir(appContext), "avatar-${handleObj.id}.glb")
                        file.parentFile?.mkdirs()
                        val exportRc = NativeBridge.engineExportGlb(h, file.absolutePath)
                        check(exportRc == 0) { "GLB export failed: ${NativeBridge.engineLastError(h)}" }

                        val counters = IntArray(5)
                        NativeBridge.engineCounts(h, counters)
                        timings[Stage.FIT_ANATOMY] = statsFloats[0]
                        timings[Stage.GEOMETRY] = statsFloats[1] + statsFloats[2]
                        timings[Stage.UV_UNWRAP] = statsFloats[5]
                        timings[Stage.BAKE_TEXTURES] = statsFloats[6]
                        timings[Stage.RIG] = statsFloats[7]
                        timings[Stage.EXPORT] = statsFloats[8]

                        EngineResult(
                            glbFile = file,
                            spec = resultSpec,
                            vertexCount = counters[0],
                            triangleCount = counters[1] / 3,
                            submeshCount = counters[2],
                            jointCount = counters[3],
                            blendshapeCount = NativeBridge.engineBlendshapeNames().size,
                            textureBytes = statsLongs[6],
                            timings = timings,
                            regionNames = regions,
                        )
                    } finally {
                        currentStage = null
                    }
                }

                if (handleObj.shouldCancel()) throw CancellationException("generation cancelled")
                handleObj.update(
                    JobStatus(
                        stage = Stage.DONE,
                        stageProgress = 1f,
                        overallProgress = 1f,
                        message = "Ready",
                        running = false,
                        elapsedMs = System.currentTimeMillis() - started,
                    ),
                )
                handleObj.deliver(Result.success(result))
            } catch (t: Throwable) {
                currentStage = null
                val cancelled = t is CancellationException || handleObj.shouldCancel()
                if (cancelled) {
                    val failure = if (t is CancellationException) t else CancellationException("generation cancelled")
                    handleObj.update(
                        handleObj.current().copy(
                            running = false,
                            cancelled = true,
                            failed = false,
                            error = null,
                            message = "Cancelled",
                            elapsedMs = System.currentTimeMillis() - started,
                        ),
                    )
                    handleObj.deliver(Result.failure(failure))
                } else {
                    val message = t.message ?: t.javaClass.simpleName
                    handleObj.update(
                        handleObj.current().copy(
                            running = false,
                            failed = true,
                            error = message,
                            message = message,
                            elapsedMs = System.currentTimeMillis() - started,
                        ),
                    )
                    handleObj.deliver(Result.failure(t))
                }
            } finally {
                currentStage = null
            }
        }
        return handleObj
    }

    /*
     * The kernel reports progress through C callbacks that arrive on the worker thread. They are
     * turned into StateFlow updates here; the Compose layer collects the flow on the main
     * dispatcher, so nothing touches the UI from a background thread.
     */
    @Volatile private var currentStage: JobHandle? = null

    /**
     * The callback object the kernel calls back into.
     *
     * A named class rather than a lambda: the JNI side resolves `onProgress`, `onLog` and
     * `shouldCancel` with GetMethodID on this object's class, and an anonymous object's synthetic
     * class name is not something a build should depend on.
     */
    private inner class KernelCallbacks {
        fun onProgress(stage: Int, fraction: Float, message: String) {
            val job = currentStage ?: return
            synchronized(callbackLock) {
                if (currentStage !== job) return@synchronized
                val s = Stage.of(stage)
                val now = System.currentTimeMillis()
                val delta = if (lastProgressMs == 0L) 0L else now - lastProgressMs
                lastProgressMs = now
                /* Overall progress is the weighted cumulative stage fraction. Native callbacks
                   report absolute fractions, so add only the monotone delta; adding each reported
                   fraction would make a stage with many updates consume its weight several times. */
                val safeFraction = fraction.coerceIn(0f, 1f)
                val wholeJobProgress = progressAccumulator.update(s, safeFraction)
                job.update(
                    JobStatus(
                        stage = s,
                        stageProgress = safeFraction,
                        overallProgress = wholeJobProgress,
                        message = message,
                        running = true,
                        elapsedMs = job.current().elapsedMs + delta,
                    ),
                )
            }
        }

        fun onLog(severity: Int, message: String) {
            /* Severity is kept in the line rather than dropped: af_log_debug..error map to the four
               levels the kernel uses, and the settings screen's log view colours them. */
            val level = when (severity) {
                0 -> "debug"
                1 -> "info"
                2 -> "warn"
                else -> "error"
            }
            synchronized(callbackLock) {
                logLines.addLast("[$level] $message")
                while (logLines.size > 256) logLines.removeFirst()
            }
        }

        fun shouldCancel(): Boolean = currentStage?.shouldCancel() ?: false
    }

    private val callbackBridge = KernelCallbacks()

    private val logLines = ArrayDeque<String>()
    private val progressAccumulator = WeightedProgressAccumulator(STAGE_WEIGHTS)
    private var lastProgressMs = 0L

    private fun requery(h: Long, spec: AvatarSpec) {
        /* Exporting a result other than the one currently in the engine means rebuilding it: the
           engine is a single-slot pipeline, and pretending otherwise would export the wrong avatar. */
        spec.requireGeneratable()
        val resetRc = NativeBridge.engineReset(h, spec.packInts(), spec.packFloats(), spec.prompt)
        check(resetRc == 0) { "rebuild setup failed: ${NativeBridge.engineLastError(h)}" }
        val rc = NativeBridge.engineRunAll(h)
        check(rc == 0) { "rebuild for export failed: ${NativeBridge.engineLastError(h)}" }
    }

    /* ------------------------------------------------------------------ health ---------------- */

    companion object {
        const val ABI_VERSION = 1
        private const val REGION_COUNT = 13
        private const val GENERATION_HEADROOM_BYTES = 512L * 1024 * 1024
        private const val CANCELLED = -6

        /** Relative cost of each stage, measured with the export probe on the host and on a
         *  Snapdragon 778G-class device. Used only to weight the progress bar. */
        private val STAGE_WEIGHTS: Map<Stage, Float> = mapOf(
            Stage.ANALYZE_INPUTS to 0.02f,
            Stage.FIT_ANATOMY to 0.01f,
            Stage.GEOMETRY to 0.66f,
            Stage.UV_UNWRAP to 0.03f,
            Stage.BAKE_TEXTURES to 0.04f,
            Stage.RIG to 0.20f,
            Stage.BLENDSHAPES to 0.03f,
            Stage.ANIMATION to 0.005f,
            Stage.EXPORT to 0.005f,
        )

        private var instance: AvatarEngine? = null

        /** One active engine per process; a released instance is replaced on the next get(). */
        fun get(context: Context): AvatarEngine = synchronized(this) {
            val current = instance
            if (current == null || current.isReleased()) {
                AvatarEngine(context.applicationContext).also { instance = it }
            } else {
                current
            }
        }

        fun health(): EngineHealth {
            val loaded = NativeBridge.available
            val abi = if (loaded) runCatching { NativeBridge.abiVersion() }.getOrDefault(0) else 0
            val version = if (loaded) runCatching { NativeBridge.engineVersion() }.getOrDefault("unavailable") else "not loaded"
            val nativeAnalysisStride = if (loaded) runCatching { NativeBridge.imageAnalysisFloatCount() }.getOrNull() else null
            return EngineHealth(
                nativeLibraryLoaded = loaded,
                abiVersion = abi,
                engineVersion = version,
                problem = when {
                    !loaded -> "libavatarforge.so did not load: ${NativeBridge.loadError ?: "unknown reason"}"
                    abi != ABI_VERSION -> "native ABI $abi does not match the SDK's $ABI_VERSION"
                    nativeAnalysisStride == null -> "native library does not expose the image-analysis schema"
                    nativeAnalysisStride != ImageAnalysisPacking.FLOAT_COUNT ->
                        "native image-analysis stride $nativeAnalysisStride does not match the SDK's ${ImageAnalysisPacking.FLOAT_COUNT}"
                    else -> null
                },
            )
        }

        fun capabilities(context: Context): EngineCapabilities {
            val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
            val info = android.app.ActivityManager.MemoryInfo()
            activityManager.getMemoryInfo(info)
            val stat = StatFs(context.filesDir.absolutePath)
            val engineHealth = health()
            val engine = get(context)
            val effectiveBudget = (info.availMem - GENERATION_HEADROOM_BYTES)
                .coerceAtLeast(64L * 1024 * 1024)
                .coerceAtMost(1536L * 1024 * 1024)
            return EngineCapabilities(
                abiVersion = engineHealth.abiVersion,
                engineVersion = engineHealth.engineVersion,
                estimatedGlbBytes = 40L * 1024 * 1024,   // measured: 33-39 MB for a full avatar
                memoryBudgetBytes = effectiveBudget,
                totalRamBytes = info.totalMem,
                lowRamDevice = activityManager.isLowRamDevice,
                freeStorageBytes = stat.availableBytes,
                clipCount = engine.clipCount,
                blendshapeCount = if (NativeBridge.available) NativeBridge.engineBlendshapeNames().size else 0,
                regionCount = REGION_COUNT,
            )
        }

        /** Where generated .glb files live: app-scoped, no permissions, shareable via FileProvider. */
        fun engineOutputDir(context: Context): File = File(context.filesDir, "avatars")

        /* ------------------------------------------------------------------ helpers ----------- */

        /** Keeps whatever the caller set explicitly, fills the rest from the prompt. */
        private fun mergePrompted(base: AvatarSpec, prompted: AvatarSpec): AvatarSpec = base.copy(
            sex = if (base.sex == Sex.UNSPECIFIED) prompted.sex else base.sex,
            build = if (base.build == Build.AVERAGE) prompted.build else base.build,
            hairStyle = if (base.hairStyle == HairStyle.MEDIUM) prompted.hairStyle else base.hairStyle,
            outfit = if (base.outfit == Outfit.NONE) prompted.outfit else base.outfit,
            footwear = if (base.footwear == Footwear.NONE) prompted.footwear else base.footwear,
            heightM = if (kotlin.math.abs(base.heightM - AvatarSpec.DEFAULT_HEIGHT_M) < 1e-5f)
                prompted.heightM else base.heightM,
            ageYears = if (base.ageYears == 30f) prompted.ageYears else base.ageYears,
            muscle = if (base.muscle == 0.45f) prompted.muscle else base.muscle,
            bodyFat = if (base.bodyFat == 0.22f) prompted.bodyFat else base.bodyFat,
            bust = if (base.bust == 0.45f) prompted.bust else base.bust,
            skinMelanin = if (base.skinMelanin == 0.35f) prompted.skinMelanin else base.skinMelanin,
            skinTone = if (base.skinTone.any { it != 0f }) base.skinTone else prompted.skinTone,
            hairTone = if (base.hairTone.any { it != 0f }) base.hairTone else prompted.hairTone,
            clothTone = if (base.clothTone.any { it != 0f }) base.clothTone else prompted.clothTone,
            gearTone = if (base.gearTone.any { it != 0f }) base.gearTone else prompted.gearTone,
            clothRoughness = if (base.clothRoughness >= 0f) base.clothRoughness else prompted.clothRoughness,
            gearMetalness = if (base.gearMetalness >= 0f) base.gearMetalness else prompted.gearMetalness,
            prompt = prompted.prompt.ifEmpty { base.prompt },
        )
    }

    /** Resident set size in bytes - the observed process-memory figure, not the native budget itself. */
    fun residentBytes(): Long = Debug.getPss() * 1024L

    fun deviceSdk(): Int = AndroidBuild.VERSION.SDK_INT
}

/** A one-line human summary of a generated avatar, used in share text and the viewer's header. */
fun EngineResult.summary(): String = buildString {
    append("AvatarForge ")
    append(spec.sex.name.lowercase())
    append(' ')
    append("%.2f m ".format(spec.heightM))
    if (spec.outfit != Outfit.NONE) append(spec.outfit.name.lowercase().replace('_', ' ') + " ")
    if (spec.footwear != Footwear.NONE) append(spec.footwear.name.lowercase() + " ")
    append("- ")
    append("%,d triangles, %,d vertices, ".format(triangleCount, vertexCount))
    append("$jointCount joints, $blendshapeCount ARKit blendshapes, ")
    append("%.1f MB .glb".format(glbBytes / 1048576.0))
}
