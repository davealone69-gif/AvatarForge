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
 * The thread's protocol is deliberately small:
 *
 *     CMD_CREATE(surface)   -> build the renderer, attach the engine if one is pending
 *     CMD_ATTACH(engine)    -> hand a freshly generated avatar to the GPU
 *     CMD_FRAME             -> draw one frame (posted by the choreographer while visible)
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
import android.util.AttributeSet
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class RenderSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private val thread = HandlerThread("avatarforge-render").apply { start() }
    private val handler = Handler(thread.looper)

    private val _rendererInfo = MutableStateFlow<RendererInfo?>(null)
    val rendererInfo: StateFlow<RendererInfo?> = _rendererInfo.asStateFlow()

    private val _stats = MutableStateFlow(emptyStats())
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

    private var renderer: AvatarRenderer? = null
    private var visible = false
    private var activePreferVulkan: Boolean? = null
    private var rendererSurfaceGeneration = -1L
    @Volatile private var surfaceGeneration = 0L
    @Volatile private var surfaceReady = false
    @Volatile private var released = false

    init {
        holder.addCallback(this)
        /* The renderer wants every pixel the view has, and it will letterbox nothing: the camera
           aspect is recomputed on resize so the avatar never stretches. */
        holder.setFormat(android.graphics.PixelFormat.OPAQUE)
    }

    /* ------------------------------------------------------------------ public API ------------ */

    /** Applies render settings (quality, environment, overlays). Safe from the UI thread. */
    fun applySettings(newSettings: RenderSettings) {
        settings = newSettings
        onRenderThread {
            if (released) return@onRenderThread
            val current = renderer
            if (current != null && activePreferVulkan != newSettings.preferVulkan) {
                /* setOptions cannot change the native backend. Recreate on the owning thread;
                   the Surface and generated engine remain owned by this view. */
                restartRenderer(holder.surface, surfaceGeneration)
            } else {
                current?.applyOptions(newSettings)
            }
        }
    }

    /** Recreate after a startup/frame failure, optionally switching backend for this attempt. */
    fun retryRenderer(newSettings: RenderSettings = settings) {
        settings = newSettings
        handler.post {
            if (released) return@post
            if (!surfaceReady) {
                _error.value = "The drawing surface is not ready. Return to the editor and reopen the viewer."
                return@post
            }
            restartRenderer(holder.surface, surfaceGeneration)
        }
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

    fun setClip(clip: Int, speed: Float = 1f, loop: Boolean = true) =
        onRenderThread { it.renderer?.setClip(clip, speed, loop) }

    fun setOverlays(skeleton: Boolean, flatShading: Boolean, highlightRegion: Int) =
        onRenderThread { it.renderer?.setOverlays(skeleton, flatShading, highlightRegion) }

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
        @Volatile private var lastNs = 0L

        fun resetClock() { lastNs = 0L }

        override fun run() {
            val r = renderer
            if (r == null) {
                /* Startup/retry owns rescheduling. Polling here after a failed create would burn
                   CPU forever without ever creating a renderer. */
                resetClock()
                return
            }
            val frameStartNs = System.nanoTime()
            val delta = if (lastNs == 0L) 1f / 60f
                        else ((frameStartNs - lastNs) / 1_000_000_000.0).toFloat()
            lastNs = frameStartNs
            try {
                r.setCamera(camera)
                r.frame(delta.coerceIn(0f, 0.1f))
                _stats.value = r.stats()
            } catch (t: Throwable) {
                Log.e(RENDER_LOG_TAG, "Renderer frame failed", t)
                destroyRenderer()
                _error.value = "Renderer frame failed: ${t.message ?: t.javaClass.simpleName}"
                return
            }
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

    private inline fun onRenderThread(crossinline block: (RenderSurfaceView) -> Unit) {
        if (Looper.myLooper() == thread.looper) block(this)
        else handler.post { block(this) }
    }

    /* ------------------------------------------------------------------ SurfaceHolder --------- */

    private fun destroyRenderer() {
        try {
            renderer?.destroy()
        } finally {
            renderer = null
            activePreferVulkan = null
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
        try {
            val requested = settings
            val created = AvatarRenderer.create(surface, requested)
            renderer = created
            activePreferVulkan = requested.preferVulkan
            rendererSurfaceGeneration = generation
            _rendererInfo.value = created.info()
            created.setCamera(camera)
            created.applyOptions(settings)
            if (pendingEngineHandle != 0L) created.attachEngine(pendingEngineHandle)
            if (visible) handler.post(frameLoop)
        } catch (t: Throwable) {
            Log.e(RENDER_LOG_TAG, "Renderer initialization failed (preferVulkan=${settings.preferVulkan})", t)
            destroyRenderer()
            _error.value = t.message ?: t.javaClass.simpleName
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
                destroyRenderer()
                _error.value = "GPU render targets could not be resized. Retry the viewer to rebuild them."
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
        surfaceReady = false
        setFrameLoopEnabled(false)
        val latch = java.util.concurrent.CountDownLatch(1)
        handler.post {
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

        fun emptyStats() = RendererStats(0f, 0f, 0f, 0f, 0, 0, 0, 0, 1, 0, 0)
    }
}
