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
        if (enabled) handler.post(frameLoop)
    }

    /* ------------------------------------------------------------------ render thread --------- */

    private val frameLoop = object : Runnable {
        private var lastNs = 0L

        override fun run() {
            val r = renderer ?: return
            val now = System.nanoTime()
            val delta = if (lastNs == 0L) 1f / 60f else ((now - lastNs) / 1_000_000_000.0).toFloat()
            lastNs = now
            try {
                r.setCamera(camera)
                r.frame(delta.coerceIn(0f, 0.1f))
                _stats.value = r.stats()
            } catch (t: Throwable) {
                _error.value = t.message ?: t.javaClass.simpleName
                return
            }
            if (visible) handler.postDelayed(this, FRAME_INTERVAL_MS)
        }
    }

    private inline fun onRenderThread(crossinline block: (RenderSurfaceView) -> Unit) {
        if (Looper.myLooper() == thread.looper) block(this)
        else handler.post { block(this) }
    }

    /* ------------------------------------------------------------------ SurfaceHolder --------- */

    override fun surfaceCreated(holder: SurfaceHolder) {
        val surface: Surface = holder.surface
        handler.post {
            try {
                renderer?.destroy()
                renderer = AvatarRenderer.create(surface, settings)
                _rendererInfo.value = renderer?.info()
                _error.value = null
                renderer?.setCamera(camera)
                renderer?.applyOptions(settings)
                if (pendingEngineHandle != 0L) renderer?.attachEngine(pendingEngineHandle)
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
                _error.value = t.message ?: t.javaClass.simpleName
            }
        }
    }

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
        const val FRAME_INTERVAL_MS = 16L
    }
}
