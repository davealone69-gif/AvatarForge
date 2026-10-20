package com.avatarforge.sdk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationJobGraphTest {
    @Test
    fun topologicalOrderHonoursEveryNativePrerequisite() {
        val ordered = GenerationJobGraph.orderedStages()
        val positions = ordered.withIndex().associate { it.value to it.index }
        assertEquals(
            listOf(
                Stage.FIT_ANATOMY,
                Stage.GEOMETRY,
                Stage.UV_UNWRAP,
                Stage.BAKE_TEXTURES,
                Stage.RIG,
                Stage.BLENDSHAPES,
                Stage.ANIMATION,
            ),
            ordered,
        )
        for (node in GenerationJobGraph.nodes) {
            for (required in node.requires) {
                assertTrue("$required must precede ${node.stage}", positions.getValue(required) < positions.getValue(node.stage))
            }
        }
    }

    @Test
    fun graphContainsEverySdkDrivenNativeStageExactlyOnce() {
        val stages = GenerationJobGraph.nodes.map { it.stage }
        assertEquals(stages.size, stages.toSet().size)
        assertEquals(Stage.ALL.filter { it.value in 2..8 }.toSet(), stages.toSet())
    }

    @Test
    fun weightedProgressAddsOnlyMonotoneCallbackDeltas() {
        val progress = WeightedProgressAccumulator(
            mapOf(Stage.GEOMETRY to 0.75f, Stage.RIG to 0.25f),
        )
        assertEquals(0.15f, progress.update(Stage.GEOMETRY, 0.2f), 0.00001f)
        assertEquals(0.30f, progress.update(Stage.GEOMETRY, 0.4f), 0.00001f)
        assertEquals(0.675f, progress.update(Stage.GEOMETRY, 0.9f), 0.00001f)
        assertEquals(0.675f, progress.update(Stage.GEOMETRY, 0.3f), 0.00001f)
        assertEquals(0.80f, progress.update(Stage.RIG, 0.5f), 0.00001f)
        assertEquals(0.925f, progress.update(Stage.RIG, 1f), 0.00001f)
        progress.reset()
        assertEquals(0f, progress.overall, 0.00001f)
        assertEquals(0.75f, progress.update(Stage.GEOMETRY, 1f), 0.00001f)
    }
}
