package com.avatarforge.app

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ImageAnalysisRequestGuardTest {
    @Test
    fun `new selection invalidates any earlier in flight analysis`() {
        val guard = ImageAnalysisRequestGuard()
        val earlierSelection = guard.begin()
        val latestSelection = guard.begin()

        assertFalse(guard.isCurrent(earlierSelection))
        assertTrue(guard.isCurrent(latestSelection))
    }

    @Test
    fun `clearing selection invalidates pending analysis`() {
        val guard = ImageAnalysisRequestGuard()
        val pendingAnalysis = guard.begin()
        guard.begin() // A clear starts a new generation, but no analysis is launched.

        assertFalse(guard.isCurrent(pendingAnalysis))
    }
}
