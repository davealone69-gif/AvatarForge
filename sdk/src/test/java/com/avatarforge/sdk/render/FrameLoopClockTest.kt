package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Test

class FrameLoopClockTest {
    @Test
    fun `first invocation and first invocation after reset are not synthetic samples`() {
        val clock = FrameLoopClock()
        assertEquals(0f, clock.nextDeltaSeconds(12_000_000_000L), 0f)
        assertEquals(0.05f, clock.nextDeltaSeconds(12_050_000_000L), 0.0001f)

        clock.reset()
        assertEquals(0f, clock.nextDeltaSeconds(90_000_000_000L), 0f)
        assertEquals(0.075f, clock.nextDeltaSeconds(90_075_000_000L), 0.0001f)
    }

    @Test
    fun `non-increasing time is invalid and long deltas keep the existing clamp`() {
        val clock = FrameLoopClock()
        clock.nextDeltaSeconds(10_000_000_000L)
        assertEquals(0f, clock.nextDeltaSeconds(9_000_000_000L), 0f)
        assertEquals(0.1f, clock.nextDeltaSeconds(9_500_000_000L), 0.0001f)
    }
}
