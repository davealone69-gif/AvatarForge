package com.avatarforge.sdk.recovery

import com.avatarforge.sdk.render.Quality
import com.avatarforge.sdk.render.RenderSettings
import com.avatarforge.sdk.render.RendererStats
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class RendererRecoveryTest {
    private fun stats(fps: Float = 60f, tris: Int = 100, draws: Int = 3) = RendererStats(
        frameMs = if (fps > 0f) 1000f / fps else 0f,
        cpuMs = 1f,
        fps = fps,
        textureMiB = 1f,
        drawCalls = draws,
        triangles = tris,
        vertices = tris * 3,
        shadowMapSize = 0,
        msaaSamples = 1,
        bloomLevels = 0,
        qualityLevel = 0,
    )

    @Test
    fun `safe settings disable optional GPU cost and preserve exposure`() {
        val current = RenderSettings(quality = Quality.HIGH, exposure = 1.25f, shadows = true, bloom = true)
        val safe = RendererRecoveryPolicy.safeSettings(current, preferVulkan = false)
        assertEquals(Quality.LOW, safe.quality)
        assertFalse(safe.preferVulkan)
        assertFalse(safe.shadows)
        assertFalse(safe.bloom)
        assertFalse(safe.sss)
        assertFalse(safe.groundPlane)
        assertEquals(1, safe.msaa)
        assertEquals(1024, safe.shadowMapSize)
        assertEquals(0, safe.bloomLevels)
        assertEquals(1.25f, safe.exposure, 0f)
        assertTrue(RendererRecoveryPolicy.isSafe(safe))
    }

    @Test
    fun `policy tries safe primary then alternate and stops after two automatic attempts`() {
        val current = RenderSettings(preferVulkan = true)
        val first = RendererRecoveryPolicy.next(current, RecoveryBackend.VULKAN, 0, emptySet())
        assertNotNull(first)
        assertEquals(RecoveryBackend.VULKAN, first!!.backend)
        assertEquals("safe-vulkan", first.id)

        val second = RendererRecoveryPolicy.next(current, RecoveryBackend.VULKAN, 1, setOf(first.id))
        assertNotNull(second)
        assertEquals(RecoveryBackend.GLES, second!!.backend)
        assertEquals("safe-gles", second.id)

        assertNull(RendererRecoveryPolicy.next(current, RecoveryBackend.VULKAN, 2, setOf(first.id, second.id)))
        assertNull(RendererRecoveryPolicy.next(current, RecoveryBackend.VULKAN, 0, setOf(first.id, second.id)))
    }

    @Test
    fun `already safe primary is not retried as the same candidate`() {
        val safe = RendererRecoveryPolicy.safeSettings(RenderSettings(preferVulkan = false), preferVulkan = false)
        val tried = setOf(RendererRecoveryPolicy.configurationId(safe, RecoveryBackend.GLES))
        val next = RendererRecoveryPolicy.next(safe, RecoveryBackend.GLES, 0, tried)
        assertNotNull(next)
        assertEquals(RecoveryBackend.VULKAN, next!!.backend)
    }

    @Test
    fun `classifier maps driver failures and fingerprints without storing raw text`() {
        val compile = RecoveryFailureClassifier.classify(RecoveryStage.STARTUP, "fragment shader forward: compile error at line 12")
        assertEquals(RecoveryFaultCategory.SHADER_COMPILE, compile.category)
        val memory = RecoveryFailureClassifier.classify(RecoveryStage.FRAME, "GL_OUT_OF_MEMORY at 0x1234")
        assertEquals(RecoveryFaultCategory.OUT_OF_MEMORY, memory.category)
        val a = RecoveryFailureClassifier.fingerprint(RecoveryStage.FRAME, "GL_INVALID_OPERATION at 0x1234 call 7")
        val b = RecoveryFailureClassifier.fingerprint(RecoveryStage.FRAME, "GL_INVALID_OPERATION at 0xABCD call 9")
        assertEquals(a, b)
        assertEquals(16, a.length)
        assertFalse(a.contains("0x"))
    }

    @Test
    fun `probe requires consecutive non-empty frames and does not claim visibility`() {
        val probe = RecoveryFrameProbe(requiredFrames = 3, timeoutFrames = 5)
        assertNull(probe.observe(stats()))
        assertNull(probe.observe(stats()))
        assertEquals(RecoveryProbeOutcome.FRAME_SUBMISSION_STABLE, probe.observe(stats()))
    }

    @Test
    fun `probe resets on empty frame and times out honestly`() {
        val probe = RecoveryFrameProbe(requiredFrames = 3, timeoutFrames = 4)
        assertNull(probe.observe(stats()))
        assertNull(probe.observe(stats(tris = 0, draws = 0)))
        assertNull(probe.observe(stats()))
        assertEquals(RecoveryProbeOutcome.PROBE_TIMEOUT, probe.observe(stats()))
    }

    @Test
    fun `advisor recommends bounded next step from journal evidence`() {
        val failure = RecoveryRecord(
            timestampMs = 1L,
            kind = RecoveryRecordKind.FAILURE,
            stage = RecoveryStage.STARTUP,
            category = RecoveryFaultCategory.SHADER_LINK,
            fingerprint = "0123456789abcdef",
            backend = "GLES",
        )
        assertTrue(RecoveryAdvisor.recommendation(listOf(failure)).contains("alternate backend"))
        val stable = RecoveryRecord(2L, RecoveryRecordKind.FRAME_SUBMISSION_STABLE, backend = "GLES")
        assertTrue(RecoveryAdvisor.recommendation(listOf(failure, stable)).contains("Visible output still needs human confirmation"))
        val fallback = RecoveryRecord(3L, RecoveryRecordKind.BACKEND_FALLBACK, backend = "gles31", candidate = "vulkan->gles")
        assertTrue(RecoveryAdvisor.recommendation(listOf(fallback)).contains("fell back to gles31"))
    }
}
