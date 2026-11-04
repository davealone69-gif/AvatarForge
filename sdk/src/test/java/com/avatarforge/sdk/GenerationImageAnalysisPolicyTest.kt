package com.avatarforge.sdk

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationImageAnalysisPolicyTest {
    @Test
    fun `no images do not trigger native analysis`() {
        assertFalse(shouldRunNativeImageAnalysis(imageCount = 0, cachedAnalysisCount = 0))
    }

    @Test
    fun `fully analyzed images reuse the confirmed spec`() {
        assertFalse(shouldRunNativeImageAnalysis(imageCount = 2, cachedAnalysisCount = 2))
    }

    @Test
    fun `any missing image analysis triggers a native pass`() {
        assertTrue(shouldRunNativeImageAnalysis(imageCount = 2, cachedAnalysisCount = 1))
        assertTrue(shouldRunNativeImageAnalysis(imageCount = 1, cachedAnalysisCount = 0))
    }
}
