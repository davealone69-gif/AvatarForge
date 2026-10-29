/*
 * AvatarForge :: KernelAccess.kt
 *
 * One process-wide fence between the code that *mutates* the kernel mesh and the code that
 * *reads* it.
 *
 * Why this exists
 * ---------------
 * The renderer is zero-copy: `af_renderer_attach_engine` hands it the engine's own vertex, index,
 * material and texture arrays, so the GL upload in `ensureGeometryBuffers()` reads the exact memory
 * that `af_engine_apply_blendshapes()` / `af_engine_apply_clip()` / `af_engine_apply_pose()` write.
 * Those writes arrive from a different thread than the GL thread:
 *
 *   * expression sliders run on the render thread (posting through RenderSurfaceView) but a host
 *     may call the SDK from its own thread;
 *   * clip playback runs on the render thread from inside `renderFrame()`;
 *   * `af_engine_run_all()` (generation, export rebuild, prompt parse) runs on the SDK's engine
 *     dispatcher.
 *
 * Without a shared lock a frame can upload a half-deformed mesh (torn vertices), and a pick on the
 * UI thread can raycast against vertices that the GL thread is rewriting at the same time. Both are
 * data races that a JVM unit test cannot see and that a phone shows as a one-frame band of stretched
 * triangles or a wrong region id - exactly the class of intermittent defect that is impossible to
 * diagnose from a screenshot.
 *
 * What it is not
 * --------------
 * Not a "thread safety" claim about the whole SDK. It is a reentrant mutex, held for the shortest
 * window that touches shared native geometry. Renderer state that only the GL thread reads (camera,
 * options, overlays, clip selection) is deliberately *outside* the lock, so the UI can keep dragging
 * the camera while a generation job holds the mesh.
 *
 * Lock ordering: there is exactly one lock. Nothing acquires another lock while holding it, so it
 * cannot deadlock with itself. Native -> Kotlin callbacks (`onProgress`, `onLog`, `shouldCancel`)
 * never acquire it, which is what keeps a locked `engineRunAll()` from re-entering.
 *
 * The helpers are intentionally not `inline`: a non-inline generic keeps the visibility rules simple
 * and the call sites are JNI-bound, where a single lambda allocation is noise.
 */
package com.avatarforge.sdk.internal

import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock

internal object KernelAccess {

    private val lock = ReentrantLock()

    /** Runs [block] while holding the kernel fence, releasing it even when [block] throws. */
    fun <T> withKernel(block: () -> T): T {
        lock.lock()
        try {
            return block()
        } finally {
            lock.unlock()
        }
    }

    /**
     * Bounded variant for callers that must never block a user-visible thread for long (picking a
     * region from a tap). Returns null when the fence could not be taken in time - a dropped pick is
     * honest ("nothing picked"), whereas a torn raycast would report a region that is not on screen.
     */
    fun <T> tryWithKernel(timeoutMs: Long, block: () -> T): T? {
        val acquired = try {
            lock.tryLock(timeoutMs, TimeUnit.MILLISECONDS)
        } catch (interrupted: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        }
        if (!acquired) return null
        try {
            return block()
        } finally {
            lock.unlock()
        }
    }
}
