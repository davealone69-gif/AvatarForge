package com.avatarforge.sdk

import com.avatarforge.sdk.render.RendererAttempt
import com.avatarforge.sdk.render.RendererDiagnostics
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStartupState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Locks findings AF-017 (frame-loop restart wording) and AF-018 (silent Vulkan -> GLES fallback).
 * Both rules are pure Kotlin: the device report that motivated them cannot be recreated in a
 * JVM test, but the exact wording decisions the report exposed can be.
 */
class RendererDiagnosticNotesTest {

    private val stableSurface = RendererDiagnostics(
        state = RendererStartupState.RUNNING,
        attempts = listOf(
            RendererAttempt(1, preferVulkan = true, succeeded = true, error = null, elapsedMs = 264),
        ),
        frameLoopRestarts = 4,
        surfaceCreates = 1,
        surfaceChanges = 1,
        surfaceDestroys = 0,
    )

    private val glesInfo = RendererInfo(
        backend = "gles31",
        deviceName = "ANGLE (Xclipse 940 on Vulkan 1.3.304)",
        apiVersion = "OpenGL ES 3.2",
        driver = "25.6.4",
        supportsShadows = true,
        supportsMsaa = true,
        supportsFloatHdr = true,
        supportsAnisotropy = true,
    )

    // ---- AF-017 -----------------------------------------------------------------------------

    @Test
    fun `no restart hint at or below three restarts`() {
        assertNull(RendererDiagnosticNotes.frameLoopRestartHint(stableSurface.copy(frameLoopRestarts = 3)))
    }

    @Test
    fun `four restarts on a stable surface explain rather than warn about flashing`() {
        val hint = RendererDiagnosticNotes.frameLoopRestartHint(stableSurface)
        requireNotNull(hint)
        assertTrue(hint.contains("Benign loop reschedules"))
        assertTrue(hint.contains("cannot cause flashing"))
    }

    @Test
    fun `restarts with real surface churn keep the flashing warning`() {
        val churned = stableSurface.copy(surfaceDestroys = 1, surfaceCreates = 2)
        val hint = RendererDiagnosticNotes.frameLoopRestartHint(churned)
        requireNotNull(hint)
        assertTrue(hint.contains("flashing"))
    }

    // ---- AF-018 -----------------------------------------------------------------------------

    @Test
    fun `vulkan preferred but gles active produces an explicit fallback item`() {
        val item = RendererDiagnosticNotes.backendFallbackItem(glesInfo, stableSurface)
        requireNotNull(item)
        assertEquals("Backend selection", item.label)
        assertEquals(DiagStatus.INFO, item.status)
        assertTrue(item.detail.contains("Vulkan was preferred"))
        assertTrue(item.detail.contains("gles31"))
        requireNotNull(item.hint)
        // The reason is honestly unknown at this layer; the hint must not invent one.
        assertTrue(item.hint.contains("does not yet expose the Vulkan rejection reason"))
    }

    @Test
    fun `no fallback item when vulkan is the active backend`() {
        val vk = glesInfo.copy(backend = "vulkan", apiVersion = "Vulkan 1.3.304")
        assertNull(RendererDiagnosticNotes.backendFallbackItem(vk, stableSurface))
    }

    @Test
    fun `no fallback item when the user explicitly forced gles`() {
        val forcedGl = stableSurface.copy(
            attempts = listOf(
                RendererAttempt(1, preferVulkan = false, succeeded = true, error = null, elapsedMs = 90),
            ),
        )
        assertNull(RendererDiagnosticNotes.backendFallbackItem(glesInfo, forcedGl))
    }

    @Test
    fun `no fallback item before a renderer exists`() {
        assertNull(RendererDiagnosticNotes.backendFallbackItem(null, stableSurface))
        assertNull(RendererDiagnosticNotes.backendFallbackItem(glesInfo, null))
    }
}
