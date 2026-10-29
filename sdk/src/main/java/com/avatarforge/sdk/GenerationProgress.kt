package com.avatarforge.sdk

/**
 * Converts per-stage fractional progress into a monotone overall fraction. The previous
 * implementation added weight * fraction on every callback, so a stage reporting 0.1, 0.2, ... 1.0
 * contributed 5.5x its weight. This computes completed prior stages plus the current stage fraction.
 */
class GenerationProgress(private val weights: Map<Stage, Float>) {
    var overall: Float = 0f
        private set

    fun reset() { overall = 0f }

    fun update(stage: Stage, fraction: Float): Float {
        if (stage == Stage.DONE) {
            overall = 1f
            return overall
        }
        val f = fraction.coerceIn(0f, 1f)
        val prior = Stage.ALL.takeWhile { it != stage }.sumOf { (weights[it] ?: 0f).toDouble() }.toFloat()
        overall = (prior + (weights[stage] ?: 0f) * f).coerceIn(0f, 1f)
        return overall
    }
}
