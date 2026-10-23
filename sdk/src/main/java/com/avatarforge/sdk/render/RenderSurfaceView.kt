/*
 * AvatarForge :: RenderSurfaceView.kt
 *
 * The SurfaceView that hosts AvatarRenderer, plus the render thread that owns it.
 *
 * Why a hand-written thread instead of a loop inside a Compose effect: the renderer must be
 * created, used and destroyed on one thread. A configuration change may recreate the Surface and
 * GPU renderer, while an explicit engine lease keeps the native mesh alive for safe reattachment.
 * The message loop also keeps frame work off the main thread during recomposition and camera drags.
 *
 * The thread's protocol is deliberately small:
 *
 *     CMD_CREATE(surface)   -> build the renderer, attach the engine if one is pending
 *     CMD_ATTACH(engine)    -> hand a freshly generated avatar to the GPU
 *     CMD_FRAME             -> draw one frame (self-scheduled against a 60 Hz work budget while visible)
 *     CMD_CONFIG(closure)   -> run renderer calls that must happen on the render thread
 *     CMD_QUIT              -> destroy the renderer and exit
 *
 * Everything the UI needs to know (backend name, fps, current clip) is published through
 * StateFlows that the Compose layer collects.
 */
package com.avatarforge.sdk.render

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Build
import android.os.PerformanceHintManager
import android.os.Process
import android.os.Trace
import android.util.AttributeSet
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import com.avatarforge.sdk.AvatarEngine
import com.avatarforge.sdk.NativeEngineLease
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.resume

class RenderSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private val thread = HandlerThread("avatarforge-render").apply { start() }
    private val handler = Handler(thread.looper)
    private val lifecycleLock = Any()
    @Volatile private var released = false
    private var leasedEngine: AvatarEngine? = null
    private var engineLease: NativeEngineLease? = null
    /** Monotone token prevents a stale attach failure from clearing a newer request. */
    private var attachmentVersion = 0L

    private val _rendererInfo = MutableStateFlow<RendererInfo?>(null)
    val rendererInfo: StateFlow<RendererInfo?> = _rendererInfo.asStateFlow()

    private val _stats = MutableStateFlow(RendererStats(0f, 0f, 0f, 0f, 0, 0, 0, 0, 1, 0, 0))
    val stats: StateFlow<RendererStats> = _stats.asStateFlow()

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error.asStateFlow()

    /** Set by the UI; applied on the render thread before the next frame. */
    @Volatile private var settings: RenderSettings = RenderSettings()
    @Volatile private var camera: CameraSettings = CameraSettings()

    /** Geometry the renderer should adopt once a surface exists. */
    @Volatile private var pendingEngineHandle: Long = 0L

    /** Native handle of the engine currently attached (0 = none). */
    @Volatile var attachedEngineHandle: Long = 0L
        private set

    /** Accessed only on the render thread. */
    private var renderer: AvatarRenderer? = null
    @Volatile private var activeSurface: Surface? = null
    /** Render-thread watchdog state for a renderer that returns without counting frames. */
    private var stalledFrames = 0
    private var restartedAfterStall = false
    @Volatile private var surfaceWidth = 0
    @Volatile private var surfaceHeight = 0
    private var performanceHintSession: PerformanceHintManager.Session? = null
    private var thermalRegistration: AutoCloseable? = null
    private val _thermalStatusFlow = MutableStateFlow<Int?>(null)
    val thermalStatusFlow: StateFlow<Int?> = _thermalStatusFlow.asStateFlow()
    @Volatile private var thermalStatus: Int? = null
    private val thermalExecutor = Executor { command -> handler.post(command) }
    @Volatile private var visible = false

    init {
        registerThermalListener()
        holder.addCallback(this)
        /* The renderer wants every pixel the view has, and it will letterbox nothing: the camera
           aspect is recomputed on resize so the avatar never stretches. */
        holder.setFormat(android.graphics.PixelFormat.OPAQUE)
    }

    private fun settingsForThermal(requested: RenderSettings): RenderSettings = requested.copy(
        quality = RenderThermalPolicy.effectiveQuality(requested.quality, thermalStatus),
    )

    private fun registerThermalListener() {
        thermalStatus = AndroidThermalMonitor.currentStatus(context)
        _thermalStatusFlow.value = thermalStatus
        thermalRegistration = AndroidThermalMonitor.register(context, thermalExecutor) { status ->
            if (!released) {
                thermalStatus = status
                _thermalStatusFlow.value = status
                runCatching { renderer?.applyOptions(settingsForThermal(settings)) }
                    .onFailure { _error.value = it.message ?: it.javaClass.simpleName }
            }
        }
    }

    private fun unregisterThermalListener() {
        runCatching { thermalRegistration?.close() }
        thermalRegistration = null
    }

    /* ------------------------------------------------------------------ public API ------------ */

    /** Applies render settings (quality, environment, overlays). Safe from the UI thread. */
    fun applySettings(newSettings: RenderSettings) {
        settings = newSettings
        onRenderThread { it.renderer?.applyOptions(it.settingsForThermal(newSettings)) }
    }

    /** Applies camera state (orbit). Safe from the UI thread; called on every drag frame. */
    fun applyCamera(newCamera: CameraSettings) {
        camera = newCamera
        onRenderThread { it.renderer?.setCamera(newCamera) }
    }

    /**
     * Attaches an engine with a lifetime lease. The handle cannot be destroyed until this view's
     * renderer has been torn down or detached.
     */
    fun attachEngine(engine: AvatarEngine) {
        synchronized(lifecycleLock) {
            check(!released) { "RenderSurfaceView has been released" }
            val oldLease: NativeEngineLease?
            val activeLease: NativeEngineLease
            if (leasedEngine === engine && engineLease != null) {
                activeLease = engineLease!!
                oldLease = null
            } else {
                activeLease = engine.acquireRendererLease()
                oldLease = engineLease
                leasedEngine = engine
                engineLease = activeLease
            }
            pendingEngineHandle = activeLease.handle
            attachedEngineHandle = activeLease.handle
            enqueueEngineAttach(activeLease.handle, activeLease, oldLease, ++attachmentVersion)
        }
    }

    /** Low-level overload. The caller owns the handle lifetime and must serialize native access. */
    fun attachEngine(engineHandle: Long) {
        synchronized(lifecycleLock) {
            if (released) return
            val oldLease = engineLease
            leasedEngine = null
            engineLease = null
            pendingEngineHandle = engineHandle
            attachedEngineHandle = engineHandle
            enqueueEngineAttach(engineHandle, null, oldLease, ++attachmentVersion)
        }
    }

    /** Detaches the mesh; a previous SDK-managed engine lease is dropped after the render thread detaches. */
    fun detachEngine() = attachEngine(0L)

    private fun enqueueEngineAttach(
        engineHandle: Long,
        activeLease: NativeEngineLease?,
        oldLease: NativeEngineLease?,
        version: Long,
    ) {
        val task = Runnable {
            try {
                if (renderer == null) {
                    val surface = activeSurface
                    if (surface?.isValid == true) createRendererOnRenderThread(surface)
                }
                renderer?.attachEngine(engineHandle)
                oldLease?.release()
            } catch (t: Throwable) {
                /* A failed rebind may leave the renderer holding its old raw engine pointer. Destroy
                   it before dropping either lease, then let a later surface-created event retry. */
                runCatching { renderer?.destroy() }
                renderer = null
                _rendererInfo.value = null
                _error.value = t.message ?: t.javaClass.simpleName
                val releaseActiveLease = synchronized(lifecycleLock) {
                    val latest = attachmentVersion == version
                    if (latest) {
                        attachedEngineHandle = 0L
                        pendingEngineHandle = 0L
                        if (engineLease === activeLease) {
                            engineLease = null
                            leasedEngine = null
                        }
                    }
                    latest || engineLease !== activeLease
                }
                if (releaseActiveLease) activeLease?.release()
                oldLease?.release()
            }
        }
        if (Looper.myLooper() == thread.looper) {
            task.run()
        } else if (!handler.post(task)) {
            activeLease?.release()
            oldLease?.release()
            if (engineLease === activeLease) {
                engineLease = null
                leasedEngine = null
            }
            attachedEngineHandle = 0L
            pendingEngineHandle = 0L
        }
    }

    /** Tells the renderer the engine's geometry changed (new pose, blendshapes, clip frame). */
    fun invalidateMesh() = onRenderThread { it.renderer?.invalidateMesh() }

    /** Applies morph weights on the render thread, between clip updates and GPU buffer reads. */
    fun applyBlendshapes(weights: FloatArray) = onRenderThread {
        if (it.attachedEngineHandle != 0L && ::engine.isInitialized) {
            val applied = runCatching { engine.applyBlendshapesFromRenderer(weights) }
                .onFailure { error -> _error.value = error.message ?: error.javaClass.simpleName }
                .getOrDefault(false)
            if (applied) it.renderer?.invalidateMesh()
        }
    }

    fun setClip(clip: Int, speed: Float = 1f, loop: Boolean = true) =
        onRenderThread { it.renderer?.setClip(clip, speed, loop) }

    fun setOverlays(skeleton: Boolean, flatShading: Boolean, highlightRegion: Int) =
        onRenderThread { it.renderer?.setOverlays(skeleton, flatShading, highlightRegion) }

    /** Synchronous picker for non-UI callers; use [pickAsync] from the main thread. */
    fun pick(viewX: Float, viewY: Float): RegionPick? =
        runOnRenderThreadAndWait<RegionPick?>(null) { pickOnRenderThread(viewX, viewY) }

    /** Picks without blocking the UI while the render thread tests the mesh. */
    suspend fun pickAsync(viewX: Float, viewY: Float): RegionPick? = suspendCancellableCoroutine { continuation ->
        val posted = synchronized(lifecycleLock) {
            !released && handler.post {
                if (continuation.isActive) {
                    val result = runCatching { pickOnRenderThread(viewX, viewY) }
                        .onFailure { _error.value = it.message ?: it.javaClass.simpleName }
                        .getOrNull()
                    if (continuation.isActive) continuation.resume(result)
                }
            }
        }
        if (!posted && continuation.isActive) continuation.resume(null)
    }

    /** Accessed only on the render thread. */
    private fun pickOnRenderThread(viewX: Float, viewY: Float): RegionPick? {
        val handle = attachedEngineHandle
        val current = renderer
        val viewWidth = surfaceWidth
        val viewHeight = surfaceHeight
        if (handle == 0L || current == null || viewWidth <= 0 || viewHeight <= 0) return null
        return current.pickRegion(handle, viewX, viewY, viewWidth, viewHeight, camera)
    }

    /** Starts/stops the frame loop. Called from the host lifecycle. */
    fun setFrameLoopEnabled(enabled: Boolean) {
        synchronized(lifecycleLock) {
            if (released) return
            visible = enabled
            handler.removeCallbacks(frameLoop)
            frameLoop.resetClock()
            if (enabled) handler.post(frameLoop)
        }
    }

    /* ------------------------------------------------------------------ render thread --------- */

    private val frameLoop = object : Runnable {
        @Volatile private var lastNs = 0L

        fun resetClock() { lastNs = 0L }

        override fun run() {
            val r = renderer
            if (r == null) {
                resetClock()
                if (visible) handler.postDelayed(this, FRAME_RETRY_MS)
                return
            }
            val frameStartNs = System.nanoTime()
            val delta = if (lastNs == 0L) 1f / 60f
                        else ((frameStartNs - lastNs) / 1_000_000_000.0).toFloat()
            lastNs = frameStartNs
            Trace.beginSection("AvatarForge.frame")
            try {
                Trace.beginSection("AvatarForge.camera")
                try {
                    r.setCamera(camera)
                } finally {
                    Trace.endSection()
                }
                Trace.beginSection("AvatarForge.renderer.frame")
                try {
                    r.frame(delta.coerceIn(0f, 0.1f))
                } finally {
                    Trace.endSection()
                }
                Trace.beginSection("AvatarForge.renderer.stats")
                try {
                    val st = r.stats()
                    _stats.value = st
                    reportFrameWorkDuration(System.nanoTime() - frameStartNs)
                    /* An empty scene still counts frames. Zero FPS plus zero triangles for a full
                       second therefore points to a renderer that is returning before drawing. */
                    if (st.fps <= 0f && st.triangles == 0) stalledFrames++ else stalledFrames = 0
                    if (stalledFrames >= STALL_FRAMES) handleRendererStall()
                } finally {
                    Trace.endSection()
                }
            } catch (t: Throwable) {
                closePerformanceHints()
                _error.value = t.message ?: t.javaClass.simpleName
                /* A single transient frame failure must not permanently freeze the viewer on a
                   black SurfaceView. Keep the diagnostic visible, then retry at a low rate while
                   the view remains visible. A successful frame clears the stale error below. */
                if (visible) handler.postDelayed(this, FRAME_ERROR_RETRY_MS)
                return
            } finally {
                Trace.endSection()
            }
            if (_error.value != null) _error.value = null
            if (visible) {
                /* The previous loop always slept 16 ms *after* doing the render work. A 22 ms
                   frame therefore became 38 ms (~26 FPS), which falsely triggered the quality
                   downgrade. Sleep only for the unused part of the 60 Hz budget. */
                val elapsedNs = System.nanoTime() - frameStartNs
                val remainingNs = (FRAME_INTERVAL_NS - elapsedNs).coerceAtLeast(0L)
                val delayMs = (remainingNs + 999_999L) / 1_000_000L
                handler.postDelayed(this, delayMs)
            }
        }
    }

    /** One-shot recovery for a renderer stuck at zero frames; all work runs on the render thread. */
    private fun handleRendererStall() {
        stalledFrames = 0
        val surface = activeSurface
        if (!restartedAfterStall && surface?.isValid == true && !released) {
            restartedAfterStall = true
            android.util.Log.e("AvatarForge", "renderer produced no frames; rebuilding once")
            try {
                createRendererOnRenderThread(surface)
                if (pendingEngineHandle != 0L) renderer?.attachEngine(pendingEngineHandle)
                _error.value = "Renderer stalled; restarted once. Checking for frames."
            } catch (t: Throwable) {
                runCatching { renderer?.destroy() }
                renderer = null
                _rendererInfo.value = null
                _error.value = "Renderer recovery failed: ${t.message ?: t.javaClass.simpleName}. Check logcat -s AvatarForge."
            }
        } else {
            _error.value = "Renderer produced no frames after recovery. Check logcat -s AvatarForge."
            android.util.Log.e("AvatarForge", _error.value ?: "renderer stalled")
        }
    }

    /** Optional Android Dynamic Performance Framework session, created on the render thread. */
    private fun openPerformanceHints() {
        closePerformanceHints()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
        performanceHintSession = runCatching {
            context.getSystemService(PerformanceHintManager::class.java)
                ?.createHintSession(intArrayOf(Process.myTid()), FRAME_INTERVAL_NS)
        }.getOrNull()
    }

    private fun reportFrameWorkDuration(durationNs: Long) {
        val session = performanceHintSession ?: return
        if (durationNs <= 0L) return
        runCatching { session.reportActualWorkDuration(durationNs) }
            .onFailure { closePerformanceHints() }
    }

    private fun closePerformanceHints() {
        runCatching { performanceHintSession?.close() }
        performanceHintSession = null
    }

    private inline fun onRenderThread(crossinline block: (RenderSurfaceView) -> Unit) {
        synchronized(lifecycleLock) {
            if (released) return
            if (Looper.myLooper() == thread.looper) {
                try {
                    block(this)
                } catch (t: Throwable) {
                    _error.value = t.message ?: t.javaClass.simpleName
                }
            } else {
                handler.post {
                    try {
                        block(this)
                    } catch (t: Throwable) {
                        _error.value = t.message ?: t.javaClass.simpleName
                    }
                }
            }
        }
    }

    /** Runs a result-producing operation on the renderer owner thread and waits for completion. */
    private fun <T> runOnRenderThreadAndWait(
        defaultValue: T,
        allowAfterRelease: Boolean = false,
        block: () -> T,
    ): T {
        if (Looper.myLooper() == thread.looper) {
            if (released && !allowAfterRelease) return defaultValue
            return block()
        }

        val result = AtomicReference(defaultValue)
        val failure = AtomicReference<Throwable?>(null)
        val latch = CountDownLatch(1)
        val posted = synchronized(lifecycleLock) {
            if (released && !allowAfterRelease) false
            else handler.post {
                try {
                    result.set(block())
                } catch (t: Throwable) {
                    failure.set(t)
                } finally {
                    latch.countDown()
                }
            }
        }
        if (!posted) return defaultValue
        awaitUninterruptibly(latch)
        failure.get()?.let { throw it }
        return result.get()
    }

    private fun awaitUninterruptibly(latch: CountDownLatch) {
        var interrupted = false
        while (true) {
            try {
                latch.await()
                break
            } catch (_: InterruptedException) {
                interrupted = true
            }
        }
        if (interrupted) Thread.currentThread().interrupt()
    }

    private fun joinUninterruptibly(target: Thread) {
        var interrupted = false
        while (true) {
            try {
                target.join()
                break
            } catch (_: InterruptedException) {
                interrupted = true
            }
        }
        if (interrupted) Thread.currentThread().interrupt()
    }

    /* ------------------------------------------------------------------ SurfaceHolder --------- */

    override fun surfaceCreated(holder: SurfaceHolder) {
        val surface: Surface = holder.surface
        activeSurface = surface
        stalledFrames = 0
        restartedAfterStall = false
        onRenderThread {
            try {
                createRendererOnRenderThread(surface)
                if (pendingEngineHandle != 0L) renderer?.attachEngine(pendingEngineHandle)
            } catch (t: Throwable) {
                runCatching { renderer?.destroy() }
                renderer = null
                closePerformanceHints()
                _rendererInfo.value = null
                _error.value = t.message ?: t.javaClass.simpleName
            }
        }
    }

    /** Creates the GPU renderer on its owner thread for a new or recovered Surface. */
    private fun createRendererOnRenderThread(surface: Surface) {
        closePerformanceHints()
        renderer?.destroy()
        val effectiveSettings = settingsForThermal(settings)
        renderer = AvatarRenderer.create(surface, effectiveSettings)
        _rendererInfo.value = renderer?.info()
        _error.value = null
        renderer?.setCamera(camera)
        renderer?.applyOptions(effectiveSettings)
        openPerformanceHints()
        handler.removeCallbacks(frameLoop)
        if (visible) handler.post(frameLoop)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        onRenderThread {
            val activeRenderer = renderer
            if (activeRenderer != null) {
                val resized = runCatching { activeRenderer.resize(width, height) }
                    .onFailure { _error.value = it.message ?: it.javaClass.simpleName }
                    .getOrDefault(false)
                if (!resized) {
                    _error.value = _error.value ?: "Renderer resize failed for ${width}×${height}; GPU targets could not be recreated"
                    handler.removeCallbacks(frameLoop)
                } else {
                    _error.value = null
                    if (visible) {
                        handler.removeCallbacks(frameLoop)
                        handler.post(frameLoop)
                    }
                }
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stalledFrames = 0
        restartedAfterStall = false
        activeSurface = null
        surfaceWidth = 0
        surfaceHeight = 0
        /* The renderer must be gone before Android invalidates the Surface. No timeout is safe:
           returning early would let a queued frame use a dead ANativeWindow. */
        runOnRenderThreadAndWait(Unit) {
            handler.removeCallbacks(frameLoop)
            closePerformanceHints()
            runCatching { renderer?.destroy() }
                .onFailure { _error.value = it.message ?: it.javaClass.simpleName }
            renderer = null
            _rendererInfo.value = null
        }
    }

    /* ------------------------------------------------------------------ teardown -------------- */

    /** Call from the host's onDestroy: stop drawing, destroy GPU objects, then release the handle lease. */
    fun release() {
        val leaseToRelease: NativeEngineLease?
        synchronized(lifecycleLock) {
            if (released) return
            released = true
            attachmentVersion++
            visible = false
            handler.removeCallbacks(frameLoop)
            leaseToRelease = engineLease
            engineLease = null
            leasedEngine = null
            pendingEngineHandle = 0L
            attachedEngineHandle = 0L
            activeSurface = null
            surfaceWidth = 0
            surfaceHeight = 0
        }
        unregisterThermalListener()
        runOnRenderThreadAndWait(Unit, allowAfterRelease = true) {
            closePerformanceHints()
            runCatching { renderer?.destroy() }
                .onFailure { _error.value = it.message ?: it.javaClass.simpleName }
            renderer = null
            _rendererInfo.value = null
        }
        leaseToRelease?.release()
        thread.quitSafely()
        if (Looper.myLooper() != thread.looper) joinUninterruptibly(thread)
    }

    /** The engine this view renders; kept for render-thread closures. */
    private lateinit var engine: AvatarEngine

    fun bind(engine: AvatarEngine) {
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
        const val FRAME_RETRY_MS = 16L
        const val FRAME_ERROR_RETRY_MS = 1000L
        const val STALL_FRAMES = 60
    }
}
