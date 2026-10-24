package com.avatarforge.sdk.render

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RenderLoopPolicyTest {
    private fun stats(fps: Float = 60f, triangles: Int = 0, fatal: Boolean = false, vk: Int = 0, sequence: Int = 1) =
        RendererStats(16f, 1f, fps, 0f, 0, triangles, 0, 0, 1, 0, 0, vk, fatal, sequence)

    @Test
    fun renderingRequiresEveryLifecycleConditionButNeverOverridesHostPause() {
        for (mask in 0 until 64) {
            val run = RenderLoopPolicy.shouldRun(
                requested = mask and 1 != 0,
                shown = mask and 2 != 0,
                windowVisible = mask and 4 != 0,
                attached = mask and 8 != 0,
                surfaceValid = mask and 16 != 0,
                released = mask and 32 != 0,
            )
            if (mask == 31) assertTrue("all render conditions satisfied", run)
            else assertFalse("invalid lifecycle combination $mask", run)
        }
    }

    @Test
    fun showingTheWindowAgainPreservesAnExplicitHostPause() {
        assertFalse(RenderLoopPolicy.shouldRun(false, true, true, true, true, false))
        assertFalse(RenderLoopPolicy.shouldRun(true, true, false, true, true, false))
        assertTrue(RenderLoopPolicy.shouldRun(true, true, true, true, true, false))
    }

    @Test
    fun anEmptySceneWithFramesIsHealthy() {
        assertTrue(RenderLoopPolicy.hasHealthyFrame(stats()))
    }

    @Test
    fun zeroFpsIsUnhealthyEvenWhenStaleTriangleCountsRemain() {
        assertFalse(RenderLoopPolicy.hasHealthyFrame(stats(fps = 0f, triangles = 100)))
    }

    @Test
    fun fatalNativeReturnMustNotBeHiddenByEarlierPositiveFps() {
        assertFalse(RenderLoopPolicy.hasHealthyFrame(stats(fatal = true)))
        assertFalse(RenderLoopPolicy.hasHealthyFrame(stats(vk = -4))) // VK_ERROR_DEVICE_LOST
    }

    @Test
    fun invalidTimingIsNotARecoverySuccess() {
        for (fps in listOf(Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY, -1f)) {
            assertFalse("fps=$fps", RenderLoopPolicy.hasHealthyFrame(stats(fps = fps)))
        }
    }

    @Test
    fun stalePositiveFpsDoesNotClearAnErrorWithoutFrameProgress() {
        assertFalse(RenderLoopPolicy.hasHealthyFrame(stats(sequence = 10), previousSequence = 10))
        assertTrue(RenderLoopPolicy.hasHealthyFrame(stats(sequence = 11), previousSequence = 10))
        assertTrue(RenderLoopPolicy.hasHealthyFrame(stats(sequence = 0), previousSequence = Int.MAX_VALUE))
    }

    @Test
    fun acceptedSuboptimalPresentIsNotFatal() {
        assertTrue(RenderLoopPolicy.hasHealthyFrame(stats(vk = 1000001003)))
    }
}
