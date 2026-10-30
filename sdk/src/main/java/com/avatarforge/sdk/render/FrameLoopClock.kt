package com.avatarforge.sdk.render

/** Tracks caller-side render-loop deltas without inventing a first-frame interval. */
internal class FrameLoopClock {
    @Volatile private var previousNs: Long? = null

    fun reset() {
        previousNs = null
    }

    fun nextDeltaSeconds(nowNs: Long): Float {
        val previous = previousNs
        previousNs = nowNs
        if (previous == null || nowNs <= previous) return 0f
        return ((nowNs - previous) / 1_000_000_000.0).toFloat().coerceIn(0f, 0.1f)
    }
}
