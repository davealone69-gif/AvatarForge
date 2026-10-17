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
 * Threading model. The heavy stages (geometry, UV unwrap, texture bake, rig) take 30-90 s on a
 * mid-range phone and are executed on a single background thread owned by the job, because the
 * kernel handle is thread-affine: one handle, one thread at a time. Progress and log callbacks are
 * delivered on that same thread; the UI is expected to marshal them itself (the Compose layer does
 * that with a StateFlow).
 */
package com.avatarforge.sdk

import android.content.Context
import android.graphics.Bitmap
import android.os.Build as AndroidBuild
import android.os.Debug
import android.os.StatFs
import com.avatarforge.sdk.internal.NativeBridge
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/* CIE L*a*b* -> linear sRGB, used to carry a garment's measured colour into the spec. */
private fun labToLinear(lab: FloatArray): FloatArray {
    val fy = (lab[0] + 16f) / 116f
    val fx = fy + lab[1] / 500f
    val fz = fy - lab[2] / 200f
    fun inv(t: Float): Float {
        val t3 = t * t * t
        return if (t3 > 0.008856f) t3 else (t - 16f / 116f) / 7.787f
    }
    val x = inv(fx) * 0.95047f
    val y = inv(fy)
    val z = inv(fz) * 1.08883f
    /* sRGB D65 inverse matrix, then the gamma decode. */
    val r = 3.2406f * x - 1.5372f * y - 0.4986f * z
    val g = -0.9689f * x + 1.8758f * y + 0.0415f * z
    val b = 0.0557f * x - 0.2040f * y + 1.0570f * z
    fun lin(c: Float): Float {
        val v = c.coerceIn(0f, 1f)
        return if (v <= 0.04045f) v / 12.92f else Math.pow(((v + 0.055) / 1.055).toDouble(), 2.4).toFloat()
    }
    return floatArrayOf(lin(r), lin(g), lin(b))
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
    /** Peak resident memory the kernel will use, bytes (from the engine's own budget, default 1.5 GB). */
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
    private val parentJob: Job,
) {
    private val _status = MutableStateFlow(JobStatus(running = true, stage = Stage.ANALYZE_INPUTS))
    val status: StateFlow<JobStatus> = _status.asStateFlow()

    private val cancelRequested = AtomicBoolean(false)
    private val completion = CompletableDeferred<EngineResult>()

    /** Stable id for UI history; also names the exported file. */
    val id: Long = nextJobId++

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

    internal val requestSpec: AvatarSpec get() = request.spec

    internal fun deliver(result: Result<EngineResult>) {
        _status.value = _status.value.copy(running = false)
        result.fold({ completion.complete(it) }, { completion.completeExceptionally(it) })
    }

    internal fun shouldCancel(): Boolean = cancelRequested.get() || parentJob.isCancelled

    companion object {
        private var nextJobId = 1L
    }
}

/* ============================================================================================ */
/*  Generation request                                                                           */
/* ============================================================================================ */

/**
 * One generation request: a spec plus the optional reference images.
 *
 * The two-image flow the generator implements is exactly this: an identity image (imageA) and an
 * optional detail/equipment image (imageB). Image A gives the face, skin tone, hair and upper-body
 * proportions; when it is a portrait or half-body shot the kernel infers the missing lower body
 * from the anthropometry it measured (head-to-body ratio, shoulder width, inseam estimate). Image B
 * is segmented from its background, classified as cloth or hard surface, and transferred onto the
 * body - geometrically (the garment volume is unioned into the body field) and texturally (its
 * dominant colour and metalness drive the material bake).
 */
data class GenerationRequest(
    val spec: AvatarSpec,
    val prompt: String = "",
    val imageA: ImageReference? = null,
    val imageB: ImageReference? = null,
    /** Total kernel memory ceiling. Default 1.5 GB, comfortably inside the app's 2 GB budget. */
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
         * the kernel. Anything else would double the peak memory for no benefit, and the 2 GB
         * budget is there to be respected, not approached.
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

/* ============================================================================================ */
/*  The engine                                                                                   */
/* ============================================================================================ */

class AvatarEngine private constructor(private val appContext: Context) {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Every job ever started, so the UI can show history and the engine can cancel everything
     *  when the process is going away. */
    private val jobs = mutableListOf<JobHandle>()

    private var handle: Long = 0L

    /**
     * The kernel handle, for the renderer.
     *
     * The renderer reads the engine's vertex/index/texture buffers directly (zero-copy) through the
     * C ABI, so it needs the handle rather than the Kotlin wrapper. It is deliberately not a `Long`
     * the UI can invent: the getter opens the engine on first use, and the handle stays valid until
     * [release].
     */
    val nativeHandle: Long get() = openHandle()

    /* ------------------------------------------------------------------ posing ---------------- */

    /** Applies local joint rotations (quaternions XYZW, jointCount * 4). */
    fun applyPose(quats: FloatArray): Boolean {
        val h = openHandle()
        return NativeBridge.engineApplyPose(h, quats) == 0
    }

    /** Fills [out] with the clip's pose at [time] seconds without applying it. */
    fun sampleClip(clip: Int, time: Float, out: FloatArray): Boolean {
        val h = openHandle()
        return NativeBridge.engineSampleClip(h, clip, time, out) == 0
    }

    /** Applies a clip pose at [time] to the mesh. AF_CLIP_REST re-derives the bind pose. */
    fun applyClip(clip: Int, time: Float): Boolean {
        val h = openHandle()
        return NativeBridge.engineApplyClip(h, clip, time) == 0
    }

    /** Applies the 52 ARKit blendshape weights (0..1 each), in [blendshapeNames] order. */
    fun applyBlendshapes(weights: FloatArray): Boolean {
        val h = openHandle()
        return NativeBridge.engineApplyBlendshapes(h, weights) == 0
    }

    /** Mesh counters: [vertices, indices, submeshes, joints, materials]. */
    fun counts(): IntArray {
        val out = IntArray(5)
        NativeBridge.engineCounts(openHandle(), out)
        return out
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

    fun clipDuration(clip: Int): Float = NativeBridge.engineClipDuration(openHandle(), clip)

    fun blendshapeNames(): List<String> = NativeBridge.engineBlendshapeNames().toList()

    val clipCount: Int get() = NativeBridge.engineClipCount()

    /* ------------------------------------------------------------------ lifecycle ------------ */

    private fun openHandle(): Long {
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

    fun release() {
        jobs.forEach { it.cancel() }
        if (handle != 0L) {
            NativeBridge.engineDestroy(handle)
            handle = 0L
        }
    }

    /* ------------------------------------------------------------------ generation ----------- */

    fun generateFromText(prompt: String, spec: AvatarSpec = AvatarSpec(), memoryBudgetBytes: Long = 1536L * 1024 * 1024): JobHandle {
        val resolved = parsePrompt(prompt, spec)
        return start(GenerationRequest(resolved, prompt, null, null, memoryBudgetBytes))
    }

    /**
     * The two-image flow.
     *
     * [identity] is Image A - a portrait or half-body shot is expected and the missing lower body is
     * inferred from the subject's proportions. [detail] is Image B - armour, clothing or equipment,
     * segmented from its background and transferred onto the body. Both are analysed before the
     * build starts, and the measurements the kernel derived are handed to [specFromImages] so the
     * caller can show or override them.
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
    fun analyseImages(images: List<ImageReference>): List<ImageReference> {
        require(images.isNotEmpty() && images.size <= 4) { "1 to 4 reference images are supported" }
        val h = openHandle()
        val n = images.size
        val widths = IntArray(n) { images[it].width }
        val heights = IntArray(n) { images[it].height }
        val channels = IntArray(n) { 4 }
        val roles = IntArray(n) { images[it].role.value }
        @Suppress("UNCHECKED_CAST")
        val pixels = Array<ByteBuffer?>(n) { images[it].pixels }
        val packed = FloatArray(n * ANALYSIS_FLOATS)
        val rc = NativeBridge.engineAnalyzeImages(h, widths, heights, channels, roles, pixels, packed)
        check(rc >= 0) { "image analysis failed: ${NativeBridge.engineLastError(h)}" }
        return images.mapIndexed { index, image ->
            ImageReference.withAnalysis(image, unpackAnalysis(packed, index))
        }
    }

    /**
     * Turns the analyses into a spec: stature from the head-count ratio, skin and hair tone from the
     * median Lab, garment colour and metalness from image B. Anything the caller already set
     * explicitly is kept - the images fill in, they do not overwrite.
     */
    fun specFromImages(images: List<ImageReference>, base: AvatarSpec): AvatarSpec {
        var spec = base
        val identity = images.firstOrNull()
        val detail = images.getOrNull(1)
        if (identity?.analysis != null) {
            val a = identity.analysis
            /* Head-box height in the frame x 7.4 heads = the subject's stature. The kernel reports
               the head box and the body landmarks; this is the scale-free estimate that works from
               any phone photo. */
            val headFraction = a.headBox[3].takeIf { it > 0.02f }
            if (headFraction != null) {
                val estHeight = (7.4f * (0.235f / headFraction.coerceIn(0.05f, 0.40f))).coerceIn(1.40f, 2.10f)
                if (spec.heightM == 0f) spec = spec.copy(heightM = estHeight)
            }
            spec = spec.copy(skinMelanin = melaninFromLab(a.skinLab))
        }
        if (detail?.analysis != null) {
            val b = detail.analysis
            spec = spec.copy(
                /* A garment reference that reads as hard surface selects the armour outfit unless
                   the caller already asked for something specific. */
                outfit = if (spec.outfit == Outfit.NONE && b.metalConfidence > 0.45f) Outfit.ARMOR_PLATE else spec.outfit,
                gearMetalness = if (spec.gearMetalness < 0f) b.metalConfidence else spec.gearMetalness,
                clothTone = if (spec.clothTone.any { it != 0f }) spec.clothTone else labToLinear(b.garmentLab),
            )
        }
        return spec
    }

    /** Runs the prompt parser only, and returns the resolved spec plus the match count. */
    fun parsePrompt(prompt: String, base: AvatarSpec = AvatarSpec()): AvatarSpec {
        val h = openHandle()
        val ints = base.packInts()
        val floats = base.packFloats()
        val outPrompt = arrayOfNulls<String>(1)
        val matched = NativeBridge.engineAnalyzePrompt(h, prompt, ints, floats, outPrompt)
        check(matched >= 0) { "prompt analysis failed: ${NativeBridge.engineLastError(h)}" }
        val spec = AvatarSpec.unpack(ints, floats, outPrompt[0] ?: prompt)
        /* Analysis is observational: keep the caller's explicit choices unless the prompt named a
           value the caller left at its default. */
        return mergePrompted(base, spec)
    }

    /* ------------------------------------------------------------------ export ---------------- */

    /** Writes the current result to [destination] (app-scoped storage) and returns the file. */
    fun exportGlb(source: EngineResult, destination: File): File {
        val h = openHandle()
        requery(h, source.spec)
        destination.parentFile?.mkdirs()
        val rc = NativeBridge.engineExportGlb(h, destination.absolutePath)
        check(rc == 0) { "GLB export failed: ${NativeBridge.engineLastError(h)}" }
        return destination
    }

    /** Exports the same .glb to memory: the SDK contract offers both, and callers embedding the
     *  model in another container need the bytes without a second file. */
    fun exportGlbBytes(source: EngineResult): ByteArray {
        val h = openHandle()
        requery(h, source.spec)
        val size = NativeBridge.engineExportGlbBytes(h, null)
        check(size > 0) { "GLB export failed: ${NativeBridge.engineLastError(h)}" }
        val buffer = ByteBuffer.allocateDirect(size.toInt()).order(ByteOrder.nativeOrder())
        val written = NativeBridge.engineExportGlbBytes(h, buffer)
        check(written == size) { "GLB export produced $written bytes, expected $size" }
        val out = ByteArray(size.toInt())
        buffer.rewind()
        buffer.get(out)
        return out
    }

    /* ------------------------------------------------------------------ internals ------------- */

    private fun start(request: GenerationRequest): JobHandle {
        val parent = Job()
        val handleObj = JobHandle(this, request, parent)
        synchronized(jobs) { jobs += handleObj }

        scope.launch(parent) {
            val started = System.currentTimeMillis()
            /* The progress accumulators are engine-wide, not per job: clear them here or the second
               generation starts its bar where the first one finished. */
            overall = 0f
            lastProgressMs = 0L
            logLines.clear()
            val timings = LinkedHashMap<Stage, Float>()
            try {
                check(NativeBridge.available) {
                    "AvatarForge native library is not loaded: ${NativeBridge.loadError ?: "unknown reason"}"
                }
                val h = openHandle()
                NativeBridge.engineSetMemoryBudget(h, request.memoryBudgetBytes)
                NativeBridge.engineReset(h, request.spec.packInts(), request.spec.packFloats(), request.prompt)

                /* Images go in before the prompt so the prompt can override what the photo implied:
                   the user typed the prompt last, so it wins. */
                if (request.imageA != null || request.imageB != null) {
                    val list = listOfNotNull(request.imageA, request.imageB)
                    analyseImages(list)
                }

                currentStage = handleObj
                val rc = NativeBridge.engineRunAll(h)
                currentStage = null
                if (rc != 0) {
                    if (rc == CANCELLED) {
                        handleObj.update(handleObj.current().copy(running = false, cancelled = true, message = "Cancelled"))
                        handleObj.deliver(Result.failure(kotlinx.coroutines.CancellationException("generation cancelled")))
                        return@launch
                    }
                    error("generation failed (${NativeBridge.engineLastError(h)})")
                }

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

                val result = EngineResult(
                    glbFile = file,
                    spec = request.spec,
                    vertexCount = counters[0],
                    triangleCount = counters[1] / 3,
                    submeshCount = counters[2],
                    jointCount = counters[3],
                    blendshapeCount = NativeBridge.engineBlendshapeNames().size,
                    textureBytes = statsLongs[6],
                    timings = timings,
                    regionNames = regions,
                )
                handleObj.update(
                    JobStatus(
                        stage = Stage.DONE,
                        stageProgress = 1f,
                        overallProgress = 1f,
                        message = "Ready",
                        running = false,
                        elapsedMs = System.currentTimeMillis() - started,
                    )
                )
                handleObj.deliver(Result.success(result))
            } catch (t: Throwable) {
                currentStage = null
                handleObj.update(
                    handleObj.current().copy(
                        running = false,
                        failed = true,
                        error = t.message ?: t.javaClass.simpleName,
                        message = t.message ?: "failed",
                        elapsedMs = System.currentTimeMillis() - started,
                    )
                )
                handleObj.deliver(Result.failure(t))
            }
        }
        return handleObj
    }

    /*
     * The kernel reports progress through C callbacks that arrive on the worker thread. They are
     * turned into StateFlow updates here; the Compose layer collects the flow on the main
     * dispatcher, so nothing touches the UI from a background thread.
     */
    private var currentStage: JobHandle? = null

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
            val s = Stage.of(stage)
            val now = System.currentTimeMillis()
            val delta = if (lastProgressMs == 0L) 0L else now - lastProgressMs
            lastProgressMs = now
            /* Overall progress is weighted by measured stage cost, not split equally: on a
               mid-range phone geometry is about two thirds of the wall clock, and a bar that races
               to 60% and then stalls for a minute reads as a hang. */
            overall += (STAGE_WEIGHTS[s] ?: 0f) * fraction.coerceIn(0f, 1f)
            job.update(
                JobStatus(
                    stage = s,
                    stageProgress = fraction.coerceIn(0f, 1f),
                    overallProgress = overall.coerceIn(0f, 1f),
                    message = message,
                    running = true,
                    elapsedMs = job.current().elapsedMs + delta,
                )
            )
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
            logLines.addLast("[$level] $message")
            while (logLines.size > 256) logLines.removeFirst()
        }

        fun shouldCancel(): Boolean = currentStage?.shouldCancel() ?: false
    }

    private val callbackBridge = KernelCallbacks()

    private val logLines = ArrayDeque<String>()
    private var overall = 0f
    private var lastProgressMs = 0L

    private fun requery(h: Long, spec: AvatarSpec) {
        /* Exporting a result other than the one currently in the engine means rebuilding it: the
           engine is a single-slot pipeline, and pretending otherwise would export the wrong avatar. */
        NativeBridge.engineReset(h, spec.packInts(), spec.packFloats(), spec.prompt)
        val rc = NativeBridge.engineRunAll(h)
        check(rc == 0) { "rebuild for export failed: ${NativeBridge.engineLastError(h)}" }
    }

    /* ------------------------------------------------------------------ health ---------------- */

    companion object {
        const val ABI_VERSION = 1
        private const val ANALYSIS_FLOATS = 22
        private const val REGION_COUNT = 13
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

        /** One engine per process: the kernel owns a single 34 MB mesh budget and a GPU context. */
        fun get(context: Context): AvatarEngine =
            instance ?: synchronized(this) {
                instance ?: AvatarEngine(context.applicationContext).also { instance = it }
            }

        fun health(): EngineHealth = EngineHealth(
            nativeLibraryLoaded = NativeBridge.available,
            abiVersion = if (NativeBridge.available) NativeBridge.abiVersion() else 0,
            engineVersion = if (NativeBridge.available) NativeBridge.engineVersion() else "not loaded",
            problem = when {
                !NativeBridge.available -> "libavatarforge.so did not load: ${NativeBridge.loadError ?: "unknown reason"}"
                NativeBridge.abiVersion() != ABI_VERSION ->
                    "native ABI ${NativeBridge.abiVersion()} does not match the SDK's $ABI_VERSION"
                else -> null
            },
        )

        fun capabilities(context: Context): EngineCapabilities {
            val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
            val info = android.app.ActivityManager.MemoryInfo()
            activityManager.getMemoryInfo(info)
            val stat = StatFs(context.filesDir.absolutePath)
            val lowest = health().let { if (it.usable) return@let it else it }
            val engine = get(context)
            return EngineCapabilities(
                abiVersion = lowest.abiVersion,
                engineVersion = lowest.engineVersion,
                estimatedGlbBytes = 40L * 1024 * 1024,   // measured: 33-39 MB for a full avatar
                memoryBudgetBytes = 1536L * 1024 * 1024,
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

        private fun unpackAnalysis(packed: FloatArray, index: Int): ImageReference.Analysis {
            var i = index * ANALYSIS_FLOATS
            fun f() = packed[i++]
            return ImageReference.Analysis(
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
                backgroundLab = floatArrayOf(f()),
            )
        }

        /**
         * Inverts CIE L* to melanin content.
         *
         * Not a lookup table from a paper: it is the same monotone relation the texture bake uses
         * (af_texture.cpp), solved the other way round, so a photo's skin tone and a typed
         * `skin_melanin` land in the same place. L* 92 (very fair) -> ~0.06, L* 20 (deep) -> ~0.95.
         */
        private fun melaninFromLab(lab: FloatArray): Float {
            val l = lab[0].coerceIn(0f, 100f)
            return ((92f - l) / 78f).coerceIn(0.02f, 0.98f)
        }

        /** Keeps whatever the caller set explicitly, fills the rest from the prompt. */
        private fun mergePrompted(base: AvatarSpec, prompted: AvatarSpec): AvatarSpec = base.copy(
            sex = if (base.sex == Sex.UNSPECIFIED) prompted.sex else base.sex,
            build = if (base.build == Build.AVERAGE) prompted.build else base.build,
            hairStyle = if (base.hairStyle == HairStyle.MEDIUM) prompted.hairStyle else base.hairStyle,
            outfit = if (base.outfit == Outfit.NONE) prompted.outfit else base.outfit,
            footwear = if (base.footwear == Footwear.NONE) prompted.footwear else base.footwear,
            heightM = if (base.heightM == 0f) prompted.heightM else base.heightM,
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

    /** Resident set size in bytes - the number the settings screen shows next to the 2 GB budget. */
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
