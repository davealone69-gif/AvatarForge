package com.avatarforge.app

import com.avatarforge.sdk.render.DiagnosticRung
import com.avatarforge.sdk.render.RenderedPixelCheck
import com.avatarforge.sdk.render.RenderedPixelStatus
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStats
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticReportTest {
    private val build = DiagnosticBuildIdentity(
        packageName = "com.avatarforge",
        versionName = "0.10.1",
        versionCode = 2L,
        abi = "arm64-v8a (process 64-bit)",
        sourceCommit = "51bdb602d8d35e9c6e81a6f0060411056dbb9328",
    )
    private val device = DiagnosticDeviceIdentity("Samsung", "SM-S921B", "16", 36)

    @Test
    fun `report separates requested Vulkan from actual gles31 fallback and renderer metadata`() {
        val info = RendererInfo(
            backend = "gles31",
            deviceName = "ANGLE (Samsung Xclipse 940) on Vulkan",
            apiVersion = "OpenGL ES 3.2",
            driver = "Samsung Electronics",
            supportsShadows = true,
            supportsMsaa = true,
            supportsFloatHdr = false,
            supportsAnisotropy = true,
            shadingLanguageVersion = "OpenGL ES GLSL ES 3.20",
        )
        val stats = RendererStats(
            frameMs = 16.67f,
            cpuMs = 4f,
            fps = 60f,
            textureMiB = 24f,
            drawCalls = 7,
            triangles = 1200,
            vertices = 3600,
            shadowMapSize = 1024,
            msaaSamples = 2,
            bloomLevels = 1,
            qualityLevel = 1,
            frameAttempts = 80,
            frameSubmissions = 79,
            presentRequestsAccepted = 78,
        )
        val pixelCheck = RenderedPixelCheck(
            status = RenderedPixelStatus.NON_BACKGROUND_PIXELS_FOUND,
            width = 720,
            height = 1280,
            nonBackgroundPixels = 42,
            diagnostic = "fixture pixels found",
        )

        val report = buildDiagnosticReport(
            build = build,
            device = device,
            engineVersion = "AvatarForge/0.10.1",
            engineAbiVersion = 1,
            nativeLibraryLoaded = true,
            nativeLibraryError = null,
            nativeEngineInitialized = true,
            requestedBackend = "VULKAN",
            rendererInfo = info,
            stats = stats,
            diagnosticRung = DiagnosticRung.TRIANGLE,
            effectiveDiagnosticRung = DiagnosticRung.TRIANGLE,
            pixelCheck = pixelCheck,
            lastError = "none",
        )

        listOf(
            "Package: com.avatarforge",
            "Version: 0.10.1 (2)",
            "ABI(s): arm64-v8a (process 64-bit)",
            "Source commit: 51bdb602d8d35e9c6e81a6f0060411056dbb9328",
            "Device: Samsung SM-S921B",
            "Engine: initialized",
            "Backend requested: VULKAN",
            "Backend actual: gles31",
            "GLES GL_RENDERER: ANGLE (Samsung Xclipse 940) on Vulkan",
            "GLES GL_VENDOR: Samsung Electronics",
            "GLES GL_VERSION: OpenGL ES 3.2",
            "GLES GLSL_VERSION: OpenGL ES GLSL ES 3.20",
            "ANGLE: present",
            "FPS: 60.0 FPS",
            "Frame attempts / submissions / accepted presents: 80 / 79 / 78",
            "Draw calls / submitted triangles: 7 / 1200",
            "Pixel-diagnostic rung selected / effective: 1 (triangle) / 1 (triangle)",
            "Pixel diagnostic: Non-background pixels found — 42 non-background pixels in 720×1280",
            "Last error: none",
        ).forEach { assertTrue("missing report field: $it", report.contains(it)) }
        assertFalse(report.contains("prompt="))
        assertFalse(report.contains("reference image bytes"))
    }

    @Test
    fun `unmeasured and uninitialized state is explicit without causing a readback`() {
        val report = buildDiagnosticReport(
            build = build.copy(sourceCommit = ""),
            device = device,
            engineVersion = "unknown",
            engineAbiVersion = 1,
            nativeLibraryLoaded = true,
            nativeLibraryError = null,
            nativeEngineInitialized = false,
            requestedBackend = "VULKAN",
            rendererInfo = null,
            stats = RendererStats(
                frameMs = -1f,
                cpuMs = -1f,
                fps = -1f,
                textureMiB = 0f,
                drawCalls = 0,
                triangles = 0,
                vertices = 0,
                shadowMapSize = 0,
                msaaSamples = 1,
                bloomLevels = 0,
                qualityLevel = 1,
            ),
            diagnosticRung = DiagnosticRung.PRODUCTION_AVATAR,
            effectiveDiagnosticRung = DiagnosticRung.PRODUCTION_AVATAR,
            pixelCheck = null,
            lastError = null,
        )

        assertTrue(report.contains("Source commit: unavailable"))
        assertTrue(report.contains("Engine: not initialized"))
        assertTrue(report.contains("Renderer: not initialized"))
        assertTrue(report.contains("Backend actual: not initialized"))
        assertTrue(report.contains("FPS: not measured"))
        assertTrue(report.contains("Pixel diagnostic: not run (no pixel readback requested)"))
        assertTrue(report.contains("Vulkan initialization warning: none recorded"))
    }

    @Test
    fun `complete Vulkan warning is preserved from fallback info or combined startup error`() {
        val completeWarning = "Native Vulkan initialization failed: vkCreateInstance failed\nfull driver detail; trying OpenGL ES 3.1"
        val info = RendererInfo(
            backend = "gles31",
            deviceName = "test GLES",
            apiVersion = "OpenGL ES 3.1",
            driver = "test vendor",
            supportsShadows = true,
            supportsMsaa = false,
            supportsFloatHdr = false,
            supportsAnisotropy = false,
            vulkanInitializationWarning = completeWarning,
        )
        val report = buildDiagnosticReport(
            build = build,
            device = device,
            engineVersion = "test",
            engineAbiVersion = 1,
            nativeLibraryLoaded = true,
            nativeLibraryError = null,
            nativeEngineInitialized = true,
            requestedBackend = "VULKAN",
            rendererInfo = info,
            stats = null,
            diagnosticRung = DiagnosticRung.PRODUCTION_AVATAR,
            effectiveDiagnosticRung = DiagnosticRung.PRODUCTION_AVATAR,
            pixelCheck = null,
            lastError = null,
        )
        assertTrue(report.contains("Backend requested: VULKAN"))
        assertTrue(report.contains("Backend actual: gles31"))
        assertTrue(report.contains("Vulkan initialization warning: $completeWarning"))

        val startupError = "Renderer::create failed (-6): Vulkan: vkCreateInstance failed\nfull driver detail; OpenGL ES 3.1: eglInitialize failed"
        assertEquals(
            "Vulkan: vkCreateInstance failed\nfull driver detail",
            extractVulkanInitializationWarning(startupError),
        )
    }
}
