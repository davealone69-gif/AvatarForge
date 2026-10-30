/*
 * AvatarForge :: RenderSurfaceView.kt
 *
 * The SurfaceView that hosts AvatarRenderer, plus the render thread that owns it.
 *
 * Why a hand-written thread instead of a loop inside a Compose effect: the renderer must be
 * created, used and destroyed on one thread, and it must survive configuration changes without
 * losing its GPU resources. A dedicated thread with a message loop gives both, and it keeps the
 * frame loop off the main thread entirely - a 60 Hz viewer on the main thread would drop frames
 * during recomposition, which is exactly when the user is dragging the camera.
 *
 * Everything the UI needs to know (backend name, fps, current clip) is published through
 * StateFlows that the Compose layer collects.
 *
 * Stability path: startup/frame/resize failures are classified and journaled locally, then a bounded
 * deterministic policy retries safe GLES settings (including after a Vulkan failure). This is not AI
 * and does not modify code. A probe checks sustained non-empty frame submission only; visible output
 * still requires human/device confirmation.
 */
package com.avatarforge.sdk.render

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.AttributeSet
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import com.avatarforge.sdk.recovery.RecoveryBackend
import com.avatarforge.sdk.recovery.RecoveryFrameProbe
import com.avatarforge.sdk.recovery.RecoveryJournal
import com.avatarforge.sdk.recovery.RecoveryProbeOutcome
import com.avatarforge.sdk.recovery.RecoveryRecordKind
import com.avatarforge.sdk.recovery.RecoveryStage
import com.avatarforge.sdk.recovery.RecoveryStatus
import com.avatarforge.sdk.recovery.RecoveryFailureClassifier
import com.avatarforge.sdk.recovery.RendererRecoveryPolicy
import java.util.concurrent.Executor
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class RenderSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private val thread = HandlerThread("avatarforge-render").apply { start() }
    private val handler = Handler(thread.looper)

    /** Private, bounded, offline fault journal. The UI may read/clear the same file. */
    val recoveryJournal = RecoveryJournal(context.applicationContext.filesDir)

    private val _rendererInfo = MutableStateFlow<RendererInfo?>(null)
    val rendererInfo: StateFlow<RendererInfo?> = _rendererInfo.asStateFlow()

    private val _stats = MutableStateFlow(emptyStats())
    val stats: StateFlow<RendererStats> = _stats.asStateFlow()

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error.asStateFlow()

    private val _effectiveSettings = MutableStateFlow(RenderSettings())
    /** Settings actually requested by the view, including deterministic recovery changes. */
    val effectiveSettings: StateFlow<RenderSettings> = _effectiveSettings.asStateFlow()

    private val _requestedDiagnosticRung = MutableStateFlow(DiagnosticRung.PRODUCTION_AVATAR)
    /** Rung requested by the host, retained even while no drawing surface exists. */
    val requestedDiagnosticRung: StateFlow<DiagnosticRung> = _requestedDiagnosticRung.asStateFlow()

    private val _effectiveDiagnosticRung = MutableStateFlow(DiagnosticRung.PRODUCTION_AVATAR)
    /** Rung confirmed by the live native renderer, or the default before renderer creation. */
    val effectiveDiagnosticRung: StateFlow<DiagnosticRung> = _effectiveDiagnosticRung.asStateFlow()

    private val _recoveryStatus = MutableStateFlow<RecoveryStatus?>(null)
    val recoveryStatus: StateFlow<RecoveryStatus?> = _recoveryStatus.asStateFlow()

    /** Last platform thermal status, or null when unavailable. Read-only for diagnostics. */
    val thermalStatusForDiagnostics: Int? get() = thermalStatus

    /** Set by the UI; applied on the render thread before the next frame. */
    @Volatile private var settings: RenderSettings = RenderSettings()
    @Volatile private var camera: CameraSettings = CameraSettings()

    /** Geometry and playback/inspection state restored on a backend or surface restart. */
    @Volatile private var pendingEngineHandle: Long = 0L
    private data class RequestedClip(val clip: Int, val speed: Float, val loop: Boolean)
    private data class RequestedOverlays(val skeleton: Boolean, val flat: Boolean, val region: Int)
    @Volatile private var requestedClip: RequestedClip? = null
    @Volatile private var requestedOverlays = RequestedOverlays(false, false, -1)

    /** Native handle of the engine currently attached (0 = none). */
    @Volatile var attachedEngineHandle: Long = 0L
        private set

    private var renderer: AvatarRenderer? = null
    private var visible = false
    private var rendererSurfaceGeneration = -1L
    @Volatile private var surfaceGeneration = 0L
    @Volatile private var surfaceReady = false
    @Volatile private var released = false
    @Volatile private var thermalStatus: Int? = null
    private var thermalMonitor: AutoCloseable? = null

    /* Render-thread recovery state. Bounded: at most two automatic attempts per failure session. */
    private var automaticAttempts = 0
    private val triedConfigurations = mutableSetOf<String>()
    private var recoveryProbe: RecoveryFrameProbe? = null
    private var recoveryCandidateId: String? = null

    init {
        holder.addCallback(this)
        /* The renderer wants every pixel the view has, and it will letterbox nothing: the camera
           aspect is recomputed on resize so the avatar never stretches. */
        holder.setFormat(android.graphics.PixelFormat.OPAQUE)
        thermalStatus = AndroidThermalMonitor.currentStatus(context)
        thermalMonitor = AndroidThermalMonitor.register(context, Executor { handler.post(it) }) { status ->
            thermalStatus = status
            onRenderThread {
                _effectiveSettings.value = RenderThermalPolicy.cap(settings, thermalStatus)
                renderer?.applyOptions(_effectiveSettings.value)
                frameLoop.resetClock()
                if (visible && surfaceReady) {
                    handler.removeCallbacks(frameLoop)
                    handler.post(frameLoop)
                }
            }
        }
    }

    /* ------------------------------------------------------------------ public API ------------ */

    /** Applies settings on the render thread. Changing graphics API requires a new native
     * renderer/surface; setOptions() cannot switch a live EGL context into Vulkan. */
    fun applySettings(newSettings: RenderSettings) {
        val changed = newSettings != settings
        val backendChanged = newSettings.backend != settings.backend
        publishSettings(newSettings)
        onRenderThread {
            if (released) return@onRenderThread
            if (changed) resetRecoverySession()
            if (backendChanged && surfaceReady) restartRenderer(holder.surface, surfaceGeneration)
            else renderer?.applyOptions(RenderThermalPolicy.cap(newSettings, thermalStatus))
        }
    }

    /** Recreate the selected native renderer after a startup/frame failure. */
    fun retryRenderer(newSettings: RenderSettings = settings) {
        publishSettings(newSettings)
        handler.post {
            if (released) return@post
            resetRecoverySession()
            val backend = currentBackend()
            triedConfigurations.add(RendererRecoveryPolicy.configurationId(newSettings, backend))
            recoveryCandidateId = "manual-retry"
            recoveryProbe = RecoveryFrameProbe()
            recoveryJournal.record(
                RecoveryRecordKind.MANUAL_RETRY,
                backend = backend.name,
                candidate = "manual-retry",
            )
            if (!surfaceReady) {
                _error.value = "The drawing surface is not ready. Return to the editor and reopen the viewer."
                return@post
            }
            restartRenderer(holder.surface, surfaceGeneration)
        }
    }

    /** User-initiated safe recovery. If no Surface exists yet, arms safe settings for the next viewer. */
    fun runSafeRecovery() {
        handler.post {
            if (released) return@post
            val failedBackend = currentBackend()
            resetRecoverySession()
            triedConfigurations.add(RendererRecoveryPolicy.configurationId(settings, failedBackend))
            val safe = RendererRecoveryPolicy.safeSettings(settings)
            val backend = RecoveryBackend.GLES
            val candidateId = "safe-gles"
            triedConfigurations.add(candidateId)
            recoveryCandidateId = candidateId
            recoveryProbe = RecoveryFrameProbe()
            publishSettings(safe)
            recoveryJournal.record(
                RecoveryRecordKind.MANUAL_SAFE_RECOVERY,
                backend = backend.name,
                candidate = candidateId,
            )
            if (surfaceReady) {
                _recoveryStatus.value = RecoveryStatus("RETRYING", "Trying $candidateId with optional GPU effects disabled.")
                restartRenderer(holder.surface, surfaceGeneration)
            } else {
                _recoveryStatus.value = RecoveryStatus("ARMED", "Safe settings are armed. Open the 3D viewer to test them.")
            }
        }
    }

    fun clearRecoveryJournal(): Boolean {
        val cleared = recoveryJournal.clear()
        onRenderThread { resetRecoverySession() }
        return cleared
    }

    /** Applies camera state (orbit). Safe from the UI thread; called on every drag frame. */
    fun applyCamera(newCamera: CameraSettings) {
        camera = newCamera
        onRenderThread { it.renderer?.setCamera(newCamera) }
    }

    /** Attaches a freshly generated engine so the viewer shows it. */
    fun attachEngine(engineHandle: Long) {
        attachedEngineHandle = engineHandle
        pendingEngineHandle = engineHandle
        onRenderThread { it.renderer?.attachEngine(engineHandle) }
    }

    /** Tells the renderer the engine's geometry changed (new pose, blendshapes, clip frame). */
    fun invalidateMesh() = onRenderThread { it.renderer?.invalidateMesh() }

    fun setClip(clip: Int, speed: Float = 1f, loop: Boolean = true) {
        requestedClip = RequestedClip(clip, speed, loop)
        onRenderThread { it.renderer?.setClip(clip, speed, loop) }
    }

    fun setOverlays(skeleton: Boolean, flatShading: Boolean, highlightRegion: Int) {
        requestedOverlays = RequestedOverlays(skeleton, flatShading, highlightRegion)
        onRenderThread { it.renderer?.setOverlays(skeleton, flatShading, highlightRegion) }
    }

    /** Validates and retains a rung, then applies it on the renderer's owning thread. */
    fun setDiagnosticRung(rung: Int) {
        val requested = DiagnosticRung.orDefault(rung)
        _requestedDiagnosticRung.value = requested
        onRenderThread { view ->
            view.renderer?.let { current ->
                current.setDiagnosticRung(requested.value)
                _effectiveDiagnosticRung.value = DiagnosticRung.orDefault(current.diagnosticRung())
            }
        }
    }

    /**
     * Applies ARKit channel weights to the attached avatar and re-uploads the mesh.
     *
     * Runs on the render thread on purpose: the morph application writes the engine's vertex array,
     * the renderer reads that same array, and doing the write from Compose would put a UI-thread
     * mutation next to a GL-thread upload. [AvatarEngine.applyBlendshapes] takes the kernel fence as
     * well, so this is ordered against an in-flight frame even if a caller invokes it directly.
     */
    fun applyBlendshapes(weights: FloatArray) = onRenderThread { view ->
        if (view.released || view.attachedEngineHandle == 0L) return@onRenderThread
        /* runCatching also covers the "view never bound to an engine" case: the lateinit access
           raises UninitializedPropertyAccessException there, which is logged, not fatal. */
        val applied = runCatching { engine.applyBlendshapes(weights) }.getOrElse { t ->
            Log.e(RENDER_LOG_TAG, "Blendshape application failed", t)
            false
        }
        if (applied) view.renderer?.invalidateMesh()
    }

    /** Picks the region under a tap; null when the ray misses the avatar. */
    fun pick(viewX: Float, viewY: Float): RegionPick? {
        val handle = attachedEngineHandle
        if (handle == 0L) return null
        /* Picking is pure CPU maths against the mesh (af_render_capi.cpp), so it does not need the
           render thread - it needs the engine's current vertices, which it reads directly. */
        val current = renderer ?: return null
        return current.pickRegion(handle, viewX, viewY, width, height, camera)
    }

    /** Starts/stops the frame loop. Called from the host lifecycle. */
    fun setFrameLoopEnabled(enabled: Boolean) {
        visible = enabled
        handler.removeCallbacks(frameLoop)
        frameLoop.resetClock()
        if (enabled) handler.post(frameLoop)
    }

    /* ------------------------------------------------------------------ render thread --------- */

    private val frameLoop = object : Runnable {
        private val frameClock = FrameLoopClock()

        fun resetClock() { frameClock.reset() }

        override fun run() {
            val r = renderer
            if (r == null) {
                /* Startup/retry owns rescheduling. Polling here after a failed create would burn
                   CPU forever without ever creating a renderer. */
                resetClock()
                return
            }
            val frameStartNs = System.nanoTime()
            val delta = frameClock.nextDeltaSeconds(frameStartNs)
            try {
                r.setCamera(camera)
                r.frame(delta)
                val frameStats = r.stats()
                _stats.value = frameStats
                observeRecoveryProbe(frameStats)
            } catch (t: Throwable) {
                val backend = currentBackend()
                Log.e(RENDER_LOG_TAG, "Renderer frame failed", t)
                handleRendererFailure(RecoveryStage.FRAME, "Renderer frame failed: ${t.message ?: t.javaClass.simpleName}", backend)
                return
            }
            if (visible) {
                /* Sleep only for the unused part of the 60 Hz budget, not a fixed 16 ms after work. */
                val elapsedNs = System.nanoTime() - frameStartNs
                val remainingNs = (RenderThermalPolicy.frameIntervalNs(thermalStatus) - elapsedNs).coerceAtLeast(0L)
                val delayMs = (remainingNs + 999_999L) / 1_000_000L
                handler.postDelayed(this, delayMs)
            }
        }
    }

    private inline fun onRenderThread(crossinline block: (RenderSurfaceView) -> Unit) {
        if (Looper.myLooper() == thread.looper) block(this)
        else handler.post { block(this) }
    }

    private fun publishSettings(newSettings: RenderSettings) {
        settings = newSettings
        _effectiveSettings.value = RenderThermalPolicy.cap(newSettings, thermalStatus)
    }

    private fun currentBackend(): RecoveryBackend =
        RecoveryBackend.from(_rendererInfo.value?.backend)

    private fun resetRecoverySession() {
        automaticAttempts = 0
        triedConfigurations.clear()
        recoveryProbe = null
        recoveryCandidateId = null
        _recoveryStatus.value = null
    }

    private fun observeRecoveryProbe(frameStats: RendererStats) {
        val probe = recoveryProbe ?: return
        if (attachedEngineHandle == 0L) return
        val outcome = probe.observe(frameStats) ?: return
        val backend = currentBackend().name
        val candidate = recoveryCandidateId ?: "recovery"
        recoveryProbe = null
        when (outcome) {
            RecoveryProbeOutcome.FRAME_SUBMISSION_STABLE -> {
                recoveryJournal.record(
                    RecoveryRecordKind.FRAME_SUBMISSION_STABLE,
                    backend = backend,
                    attempt = automaticAttempts,
                    candidate = candidate,
                )
                _recoveryStatus.value = RecoveryStatus(
                    "STABLE",
                    "Frame submission is stable with $candidate. Visible avatar output still needs human confirmation.",
                )
            }
            RecoveryProbeOutcome.PROBE_TIMEOUT -> {
                recoveryJournal.record(
                    RecoveryRecordKind.PROBE_TIMEOUT,
                    backend = backend,
                    attempt = automaticAttempts,
                    candidate = candidate,
                )
                _recoveryStatus.value = RecoveryStatus(
                    "UNVERIFIED",
                    "Renderer started, but non-empty frame submission was not verified. Export diagnostics before changing settings.",
                )
            }
        }
    }

    private fun handleRendererFailure(stage: RecoveryStage, message: String, backend: RecoveryBackend) {
        val failure = RecoveryFailureClassifier.classify(stage, message)
        recoveryJournal.record(
            RecoveryRecordKind.FAILURE,
            stage = failure.stage,
            category = failure.category,
            fingerprint = failure.fingerprint,
            backend = backend.name,
            attempt = automaticAttempts,
        )
        destroyRenderer()
        _error.value = message
        triedConfigurations.add(RendererRecoveryPolicy.configurationId(settings, backend))
        val candidate = RendererRecoveryPolicy.next(settings, backend, automaticAttempts, triedConfigurations)
        if (candidate == null) {
            recoveryProbe = null
            recoveryCandidateId = null
            _recoveryStatus.value = RecoveryStatus(
                "STOPPED",
                "Automatic recovery stopped after $automaticAttempts attempt(s). Open MENU → Self diagnostics for Safe recovery or export the report.",
            )
            return
        }
        automaticAttempts++
        triedConfigurations.add(candidate.id)
        recoveryCandidateId = candidate.id
        recoveryProbe = RecoveryFrameProbe()
        publishSettings(candidate.settings)
        recoveryJournal.record(
            RecoveryRecordKind.AUTOMATIC_ATTEMPT,
            stage = failure.stage,
            category = failure.category,
            fingerprint = failure.fingerprint,
            backend = candidate.backend.name,
            attempt = automaticAttempts,
            candidate = candidate.id,
        )
        _recoveryStatus.value = RecoveryStatus(
            "RETRYING",
            "Automatic recovery $automaticAttempts/${RendererRecoveryPolicy.MAX_AUTOMATIC_ATTEMPTS}: ${candidate.label}. Optional GPU effects are disabled.",
        )
        handler.post {
            if (!released && surfaceReady) restartRenderer(holder.surface, surfaceGeneration)
            else _recoveryStatus.value = RecoveryStatus("WAITING_SURFACE", "Safe settings are armed. Reopen the viewer to test them.")
        }
    }

    /* ------------------------------------------------------------------ SurfaceHolder --------- */

    private fun destroyRenderer() {
        try {
            renderer?.destroy()
        } finally {
            renderer = null
            rendererSurfaceGeneration = -1L
            _rendererInfo.value = null
            _stats.value = emptyStats()
        }
    }

    private fun restartRenderer(surface: Surface, generation: Long) {
        if (released || !surfaceReady || generation != surfaceGeneration || !surface.isValid) return
        handler.removeCallbacks(frameLoop)
        frameLoop.resetClock()
        destroyRenderer()
        _error.value = null
        val requested = settings
        val requestedBackend = if (requested.backend == RenderBackend.VULKAN)
            RecoveryBackend.VULKAN else RecoveryBackend.GLES
        try {
            val created = AvatarRenderer.create(surface, requested)
            renderer = created
            rendererSurfaceGeneration = generation
            val info = created.info()
            _rendererInfo.value = info
            if (requestedBackend == RecoveryBackend.VULKAN && RecoveryBackend.from(info.backend) == RecoveryBackend.GLES) {
                recoveryJournal.record(
                    RecoveryRecordKind.BACKEND_FALLBACK,
                    backend = RecoveryBackend.GLES.name,
                    candidate = "vulkan-init-to-gles",
                )
                _recoveryStatus.value = RecoveryStatus(
                    "FALLBACK", "Native Vulkan did not initialize. OpenGL ES is active; check logs for the Vulkan error.",
                )
            }
            created.setCamera(camera)
            if (pendingEngineHandle != 0L) created.attachEngine(pendingEngineHandle)
            /* Reapply after engine attachment so a recreated renderer cannot silently return to
               production output and so the selected rung is authoritative for the next frame. */
            created.setDiagnosticRung(_requestedDiagnosticRung.value.value)
            _effectiveDiagnosticRung.value = DiagnosticRung.orDefault(created.diagnosticRung())
            requestedOverlays.let { created.setOverlays(it.skeleton, it.flat, it.region) }
            requestedClip?.let { created.setClip(it.clip, it.speed, it.loop) }
            if (recoveryProbe != null) {
                _recoveryStatus.value = if (pendingEngineHandle != 0L) {
                    RecoveryStatus("VERIFYING", "Checking sustained non-empty frame submission. This does not prove visible pixels.")
                } else {
                    RecoveryStatus("WAITING_GEOMETRY", "Renderer started. Attach an avatar to verify frame submission.")
                }
            }
            if (visible) handler.post(frameLoop)
        } catch (t: Throwable) {
            Log.e(RENDER_LOG_TAG, "${requested.backend} renderer initialization failed", t)
            handleRendererFailure(
                RecoveryStage.STARTUP,
                t.message ?: t.javaClass.simpleName,
                requestedBackend,
            )
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val surface = holder.surface
        val generation = ++surfaceGeneration
        surfaceReady = true
        handler.post { restartRenderer(surface, generation) }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        val generation = surfaceGeneration
        handler.post {
            if (!surfaceReady || generation != surfaceGeneration || rendererSurfaceGeneration != generation) return@post
            val current = renderer ?: return@post
            if (!current.resize(width, height)) {
                val backend = currentBackend()
                handleRendererFailure(
                    RecoveryStage.RESIZE,
                    "GPU render targets could not be resized. Safe recovery will rebuild them.",
                    backend,
                )
                return@post
            }
            if (visible) {
                handler.removeCallbacks(frameLoop)
                handler.post(frameLoop)
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        /* Invalidate before waiting: queued startup/frame work must not touch the dead Surface.
           Synchronous teardown still completes before Android invalidates the native window. */
        surfaceReady = false
        val generation = surfaceGeneration
        val latch = java.util.concurrent.CountDownLatch(1)
        handler.post {
            resetRecoverySession()
            if (!surfaceReady && rendererSurfaceGeneration == generation) destroyRenderer()
            latch.countDown()
        }
        if (!latch.await(2, java.util.concurrent.TimeUnit.SECONDS)) {
            Log.e(RENDER_LOG_TAG, "Timed out waiting for renderer teardown")
        }
    }

    /* ------------------------------------------------------------------ teardown -------------- */

    /** Call from the host's onDestroy: stops the thread and frees the GPU objects. */
    fun release() {
        if (released) return
        released = true
        thermalMonitor?.close()
        thermalMonitor = null
        surfaceReady = false
        setFrameLoopEnabled(false)
        val latch = java.util.concurrent.CountDownLatch(1)
        handler.post {
            resetRecoverySession()
            destroyRenderer()
            latch.countDown()
        }
        latch.await(2, java.util.concurrent.TimeUnit.SECONDS)
        thread.quitSafely()
    }

    /** The engine this view renders; kept for the render-thread closures. */
    private lateinit var engine: com.avatarforge.sdk.AvatarEngine

    fun bind(engine: com.avatarforge.sdk.AvatarEngine) {
        this.engine = engine
    }

    /** Set once in XML/inflation; kept for parity with View's own visibility semantics. */
    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        setFrameLoopEnabled(visibility == VISIBLE)
    }

    private companion object {
        /* 60 Hz target. The kernel's own budget controller steps quality down if frames take too
           long, so this interval is a ceiling on CPU submissions, not a promise the GPU will
           keep - the honest number is in RendererStats.fps. */
        const val FRAME_INTERVAL_NS = 16_666_667L
        const val RENDER_LOG_TAG = "AvatarForgeRenderer"

        fun emptyStats() = RendererStats(-1f, -1f, -1f, 0f, 0, 0, 0, 0, 1, 0, 0)
    }
}
