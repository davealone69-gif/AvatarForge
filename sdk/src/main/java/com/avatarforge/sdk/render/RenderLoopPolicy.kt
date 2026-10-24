package com.avatarforge.sdk.render

/** Pure decisions shared by the SurfaceView scheduler and its JVM regression tests. */
internal object RenderLoopPolicy {
    fun shouldRun(
        requested: Boolean,
        shown: Boolean,
        windowVisible: Boolean,
        attached: Boolean,
        surfaceValid: Boolean,
        released: Boolean,
    ): Boolean = requested && shown && windowVisible && attached && surfaceValid && !released

    /** Empty scenes still produce frames; triangle counts must not hide a stopped backend. */
    fun hasHealthyFrame(stats: RendererStats, previousSequence: Int = 0): Boolean =
        stats.frameSequence != previousSequence && stats.fps.isFinite() && stats.fps > 0f &&
            !stats.frameFatal && stats.lastVkResult >= 0
}
