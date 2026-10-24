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

    /** Last Surface handed to surfaceCreated; retained so a failed create can be retried. */
    @Volatile private var activeSurface: Surface? = null

    /** Create attempts made for the current surface; bounded by MAX_CREATE_ATTEMPTS.
     *  Written from surfaceCreated (main thread) and read on the render thread. */
    @Volatile private var createAttempts = 0

    private var renderer: AvatarRenderer? = null
    private var visible = false

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
        onRenderThread { it.renderer?.applyOptions(newSettings) }
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
                resetClock()
                if (visible) handler.postDelayed(this, FRAME_RETRY_MS)
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
                _error.value = t.message ?: t.javaClass.simpleName
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

    override fun surfaceCreated(holder: SurfaceHolder) {
        activeSurface = holder.surface
        createAttempts = 0
        handler.removeCallbacks(retryCreate)
        handler.post { attemptCreate() }
    }

    /**
     * Re-runs the bounded create sequence for the current surface. The viewer's Retry action
     * calls this after the automatic attempts have been exhausted.
     */
    fun restart() {
        handler.removeCallbacks(retryCreate)
        handler.post {
            createAttempts = 0
            attemptCreate()
        }
    }

    /**
     * One renderer create attempt on the render thread. A failed create used to leave the viewer
     * permanently on "initialising GPU..." at 0 FPS: the frame loop only re-posts itself while
     * the renderer is null and nothing ever re-ran creation. The recovery here is bounded:
     *
     *   1. create with the user's settings (Vulkan preferred by default);
     *   2. if that fails, retry with Vulkan switched off so GLES 3.1 gets a clean attempt
     *      instead of inheriting state from a failed Vulkan initialisation;
     *   3. one delayed retry in case the Surface was not fully ready;
     *
     * after which the error card (with its Retry action) stays up and no more attempts run.
     */
    private fun attemptCreate() {
        val surface = activeSurface
        if (surface == null) {
            _error.value = "the render surface is gone"
            return
        }
        val forceGl = createAttempts >= 1
        val attemptSettings = if (forceGl) settings.copy(preferVulkan = false) else settings
        try {
            renderer?.destroy()
            renderer = null
            _rendererInfo.value = null
            val created = AvatarRenderer.create(surface, attemptSettings)
            renderer = created
            _rendererInfo.value = created.info()
            _error.value = null
            created.setCamera(camera)
            /* Re-apply the user's real settings: the GLES-forced attempt only swapped the
               backend preference for creation, the viewer's own toggle must stay authoritative. */
            created.applyOptions(settings)
            if (pendingEngineHandle != 0L) created.attachEngine(pendingEngineHandle)
            createAttempts = 0
            /*
             * The view can become visible before Android creates its Surface. In that order,
             * frameLoop exits because renderer is null and never schedules itself again.
             * Restart it only after the renderer and Surface are both ready.
             */
            handler.removeCallbacks(frameLoop)
            if (visible) handler.post(frameLoop)
        } catch (t: Throwable) {
            renderer = null
            _rendererInfo.value = null
            createAttempts++
            if (createAttempts < MAX_CREATE_ATTEMPTS) {
                val reason = t.message ?: t.javaClass.simpleName
                _error.value = if (forceGl)
                    "GPU initialisation failed ($reason); retrying..."
                else
                    "Vulkan backend unavailable ($reason); trying OpenGL ES 3.1..."
                /* The final automatic attempt is delayed so a not-yet-ready Surface can settle. */
                handler.removeCallbacks(retryCreate)
                if (createAttempts == MAX_CREATE_ATTEMPTS - 1) {
                    handler.postDelayed(retryCreate, CREATE_RETRY_DELAY_MS)
                } else {
                    handler.post(retryCreate)
                }
            } else {
                _error.value = t.message ?: t.javaClass.simpleName
            }
        }
    }

    private val retryCreate = Runnable { attemptCreate() }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        handler.post {
            renderer?.resize(width, height)
            if (visible && renderer != null) {
                handler.removeCallbacks(frameLoop)
                handler.post(frameLoop)
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        /* Synchronous: the renderer must be gone before the Surface is invalidated, otherwise the
           next frame draws into a dead native window. */
        val latch = java.util.concurrent.CountDownLatch(1)
        handler.post {
            activeSurface = null
            handler.removeCallbacks(retryCreate)
            renderer?.destroy()
            renderer = null
            rendererInfoValueReset()
            latch.countDown()
        }
        latch.await(2, java.util.concurrent.TimeUnit.SECONDS)
    }

    private fun rendererInfoValueReset() {
        _rendererInfo.value = null
    }

    /* ------------------------------------------------------------------ teardown -------------- */

    /** Call from the host's onDestroy: stops the thread and frees the GPU objects. */
    fun release() {
        setFrameLoopEnabled(false)
        handler.removeCallbacks(retryCreate)
        val latch = java.util.concurrent.CountDownLatch(1)
        handler.post {
            renderer?.destroy()
            renderer = null
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
        const val FRAME_RETRY_MS = 16L
        /* Three create attempts per surface: as-configured, GLES-forced, then one delayed. */
        const val MAX_CREATE_ATTEMPTS = 3
        const val CREATE_RETRY_DELAY_MS = 250L
    }
}
