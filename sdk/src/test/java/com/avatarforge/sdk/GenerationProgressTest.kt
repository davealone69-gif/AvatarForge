package com.avatarforge.sdk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationProgressTest {
    private val weights = mapOf(
        Stage.ANALYZE_INPUTS to 0.02f, Stage.FIT_ANATOMY to 0.01f, Stage.GEOMETRY to 0.66f,
        Stage.UV_UNWRAP to 0.03f, Stage.BAKE_TEXTURES to 0.04f, Stage.RIG to 0.20f,
        Stage.BLENDSHAPES to 0.03f, Stage.ANIMATION to 0.005f, Stage.EXPORT to 0.005f,
    )

    @Test fun repeatedStageFractionsDoNotOvercount() {
        val progress = GenerationProgress(weights)
        var last = 0f
        for (i in 0..10) {
            val value = progress.update(Stage.GEOMETRY, i / 10f)
            assertTrue(value >= last)
            last = value
        }
        assertEquals(0.03f + 0.66f, last, 1e-5f)
    }

    @Test fun stageTransitionUsesCompletedPriorWeights() {
        val progress = GenerationProgress(weights)
        progress.update(Stage.GEOMETRY, 1f)
        val value = progress.update(Stage.UV_UNWRAP, 0.5f)
        assertEquals(0.02f + 0.01f + 0.66f + 0.015f, value, 1e-5f)
    }

    @Test fun doneIsOneAndResetIsZero() {
        val progress = GenerationProgress(weights)
        assertEquals(1f, progress.update(Stage.DONE, 0f))
        progress.reset()
        assertEquals(0f, progress.overall)
    }
}
