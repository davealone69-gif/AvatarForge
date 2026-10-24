package com.avatarforge.sdk

import com.avatarforge.sdk.render.RendererAttempt
import com.avatarforge.sdk.render.RendererDiagnostics
import com.avatarforge.sdk.render.RendererStartupState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Locks the shared report contract used by both the on-screen panel and the clipboard text, so
 * the two cannot drift, and the verdict never understates a FAIL/WARN row.
 */
class DiagnosticsReportTest {

    private fun report(vararg items: DiagItem, diagnostics: RendererDiagnostics? = null) =
        DiagnosticsReport(
            collectedAtMs = 0L,
            items = items.toList(),
            rendererDiagnostics = diagnostics,
            rendererInfo = null,
            rendererStats = null,
        )

    @Test
    fun `worst status is fail when any row fails`() {
        val r = report(
            DiagItem("App", "a", "ok", DiagStatus.OK),
            DiagItem("Renderer", "b", "broken", DiagStatus.FAIL, "retry"),
            DiagItem("Device", "c", "warm", DiagStatus.WARN),
        )
        assertEquals(DiagStatus.FAIL, r.worstStatus)
    }

    @Test
    fun `warn beats ok and info-only reports are ok`() {
        assertEquals(
            DiagStatus.WARN,
            report(DiagItem("Device", "m", "low", DiagStatus.WARN)).worstStatus,
        )
        assertEquals(
            DiagStatus.OK,
            report(DiagItem("App", "v", "0.10.1", DiagStatus.INFO)).worstStatus,
        )
    }

    @Test
    fun `plain text keeps area grouping status detail and hints`() {
        val text = report(
            DiagItem("App", "version", "0.10.1 (2)", DiagStatus.INFO),
            DiagItem("Device", "memory", "tight", DiagStatus.WARN, "close background apps"),
        ).plainText
        assertTrue(text.contains("Overall: WARN"))
        assertTrue(text.contains("== App =="))
        assertTrue(text.contains("== Device =="))
        assertTrue(text.contains("[WARN] memory: tight"))
        assertTrue(text.contains("hint: close background apps"))
    }

    @Test
    fun `attempt history and tier transitions render in plain text`() {
        val diagnostics = RendererDiagnostics(
            state = RendererStartupState.FAILED,
            attempts = listOf(
                RendererAttempt(1, preferVulkan = true, succeeded = false, error = "vk fail", elapsedMs = 12),
                RendererAttempt(2, preferVulkan = false, succeeded = true, error = null, elapsedMs = 8),
            ),
            lastError = "vk fail",
            tierTransitions = listOf("frame 412: tier 2 -> 1"),
        )
        val text = report(
            DiagItem("Renderer", "start", "FAILED", DiagStatus.FAIL),
            diagnostics = diagnostics,
        ).plainText
        assertTrue(text.contains("Renderer start-up attempts"))
        assertTrue(text.contains("Vulkan preferred"))
        assertTrue(text.contains("GLES forced"))
        assertTrue(text.contains("vk fail"))
        assertTrue(text.contains("frame 412: tier 2 -> 1"))
    }
}
