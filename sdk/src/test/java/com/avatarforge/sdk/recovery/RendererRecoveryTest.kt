package com.avatarforge.sdk.recovery

import com.avatarforge.sdk.render.Quality
import com.avatarforge.sdk.render.RenderBackend
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
        frameMs = if (fps > 0f) 1000f / fps else -1f,
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
        val current = RenderSettings(quality = Quality.HIGH, backend = RenderBackend.VULKAN,
            exposure = 1.25f, shadows = true, bloom = true)
        val safe = RendererRecoveryPolicy.safeSettings(current)
        assertEquals(RenderBackend.GLES, safe.backend)
        assertEquals(Quality.LOW, safe.quality)
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
    fun `policy switches a fatal Vulkan attempt to safe GLES only once`() {
        val current = RenderSettings(backend = RenderBackend.VULKAN)
        val first = RendererRecoveryPolicy.next(current, RecoveryBackend.VULKAN, 0, emptySet())
        assertNotNull(first)
        assertEquals(RecoveryBackend.GLES, first!!.backend)
        assertEquals(RenderBackend.GLES, first.settings.backend)
        assertEquals("safe-gles", first.id)
        assertNull(RendererRecoveryPolicy.next(first.settings, RecoveryBackend.GLES, 1, setOf(first.id)))
        assertNull(RendererRecoveryPolicy.next(current, RecoveryBackend.UNKNOWN, 0, setOf(first.id)))
        assertEquals(RecoveryBackend.GLES, RecoveryBackend.from("gles31 · ANGLE on Vulkan"))
        assertEquals(RecoveryBackend.VULKAN, RecoveryBackend.from("vulkan"))
    }

    @Test
    fun `already safe GLES is not retried in a loop`() {
        val safe = RendererRecoveryPolicy.safeSettings(RenderSettings())
        val tried = setOf(RendererRecoveryPolicy.configurationId(safe, RecoveryBackend.GLES))
        assertNull(RendererRecoveryPolicy.next(safe, RecoveryBackend.GLES, 0, tried))
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
        assertEquals(RecoveryFaultCategory.VULKAN_DEVICE_LOST,
            RecoveryFailureClassifier.classify(RecoveryStage.FRAME,
                "vkQueueSubmit failed (VkResult -4)").category)
        assertEquals(RecoveryFaultCategory.VULKAN_SURFACE_LOST,
            RecoveryFailureClassifier.classify(RecoveryStage.FRAME,
                "vkQueuePresentKHR failed (VkResult -1000000000)").category)
    }

    @Test
    fun `probe requires consecutive non-empty frames and does not claim visibility`() {
        val probe = RecoveryFrameProbe(requiredFrames = 3, timeoutFrames = 5)
        assertNull(probe.observe(stats()))
        assertNull(probe.observe(stats()))
        assertEquals(RecoveryProbeOutcome.FRAME_SUBMISSION_STABLE, probe.observe(stats()))
    }

    @Test
    fun `probe does not count an unmeasured timing sample`() {
        val probe = RecoveryFrameProbe(requiredFrames = 2, timeoutFrames = 4)
        assertNull(probe.observe(stats(fps = 0f)))
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
        assertTrue(RecoveryAdvisor.recommendation(listOf(failure)).contains("GLES shader/driver pair"))
        val stable = RecoveryRecord(2L, RecoveryRecordKind.FRAME_SUBMISSION_STABLE, backend = "GLES")
        assertTrue(RecoveryAdvisor.recommendation(listOf(failure, stable)).contains("Visible output still needs human confirmation"))
        val fallback = RecoveryRecord(3L, RecoveryRecordKind.BACKEND_FALLBACK, backend = "gles31", candidate = "legacy-fallback")
        assertTrue(RecoveryAdvisor.recommendation(listOf(fallback)).contains("Native Vulkan startup fell back"))
    }
}
