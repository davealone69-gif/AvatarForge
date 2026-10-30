package com.avatarforge.app

import com.avatarforge.sdk.render.DiagnosticRung
import com.avatarforge.sdk.render.RenderedPixelCheck
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStats
import java.util.Locale

/** Build identity included in a user-requested diagnostic report; contains no user/session fields. */
internal data class DiagnosticBuildIdentity(
    val packageName: String,
    val versionName: String,
    val versionCode: Long,
    val abi: String,
    val sourceCommit: String,
)

/** Device identity is limited to non-unique hardware/OS properties needed to diagnose graphics issues. */
internal data class DiagnosticDeviceIdentity(
    val manufacturer: String,
    val model: String,
    val androidRelease: String,
    val apiLevel: Int,
)

/** Formats only technical build, device, engine and rendering state; never prompts or avatar data. */
internal fun buildDiagnosticReport(
    build: DiagnosticBuildIdentity,
    device: DiagnosticDeviceIdentity,
    engineVersion: String,
    engineAbiVersion: Int,
    nativeLibraryLoaded: Boolean,
    nativeLibraryError: String?,
    nativeEngineInitialized: Boolean,
    requestedBackend: String,
    effectiveBackend: String,
    rendererInfo: RendererInfo?,
    stats: RendererStats?,
    diagnosticRung: DiagnosticRung,
    effectiveDiagnosticRung: DiagnosticRung,
    pixelCheck: RenderedPixelCheck?,
    lastError: String?,
): String = buildString {
    appendLine("AvatarForge in-app diagnostic report")
    appendLine("Build")
    appendLine("Package: ${build.packageName}")
    appendLine("Version: ${build.versionName} (${build.versionCode})")
    appendLine("ABI(s): ${build.abi.ifBlank { "unavailable" }}")
    appendLine("Source commit: ${build.sourceCommit.ifBlank { "unavailable" }}")
    appendLine("Device: ${listOf(device.manufacturer, device.model).filter { it.isNotBlank() }.joinToString(" ").ifBlank { "unknown" }}")
    appendLine("Android: ${device.androidRelease.ifBlank { "unknown" }} (API ${device.apiLevel})")

    appendLine()
    appendLine("Engine and native initialization")
    appendLine("Native library: ${if (nativeLibraryLoaded) "loaded" else "FAILED"}")
    if (!nativeLibraryLoaded && !nativeLibraryError.isNullOrBlank()) {
        appendLine("Native load error: $nativeLibraryError")
    }
    appendLine("Engine: ${if (nativeLibraryLoaded && nativeEngineInitialized) "initialized" else if (nativeLibraryLoaded) "not initialized" else "unavailable"}")
    appendLine("Engine version / ABI: $engineVersion / $engineAbiVersion")
    appendLine("Renderer: ${if (rendererInfo == null) "not initialized" else "initialized"}")
    appendLine("Backend requested / effective: $requestedBackend / $effectiveBackend")
    appendLine("Backend active: ${rendererInfo?.backend ?: "not initialized"}")

    if (rendererInfo != null) {
        appendLine("Graphics device/renderer: ${rendererInfo.deviceName.ifBlank { "unavailable" }}")
        appendLine("Graphics API version: ${rendererInfo.apiVersion.ifBlank { "unavailable" }}")
        appendLine("Graphics driver/vendor: ${rendererInfo.driver.ifBlank { "unavailable" }}")
        if (rendererInfo.isGles) {
            appendLine("GLES GL_RENDERER: ${rendererInfo.deviceName.ifBlank { "unavailable" }}")
            appendLine("GLES GL_VENDOR: ${rendererInfo.driver.ifBlank { "unavailable" }}")
            appendLine("GLES GL_VERSION: ${rendererInfo.apiVersion.ifBlank { "unavailable" }}")
            appendLine("GLES GLSL_VERSION: ${rendererInfo.shadingLanguageVersion.ifBlank { "unavailable" }}")
            val anglePresent = listOf(rendererInfo.deviceName, rendererInfo.driver, rendererInfo.apiVersion)
                .any { it.contains("ANGLE", ignoreCase = true) }
            appendLine("ANGLE: ${if (anglePresent) "present in the active GLES driver details" else "not indicated by the active GLES driver details"}")
        } else {
            appendLine("GLES/ANGLE details: not active (native Vulkan backend)")
        }
    } else {
        appendLine("GLES/ANGLE details: unavailable (renderer not initialized)")
    }

    val vulkanWarning = rendererInfo?.vulkanInitializationWarning?.takeIf { it.isNotBlank() }
        ?: extractVulkanInitializationWarning(lastError)
    appendLine("Vulkan initialization warning: ${vulkanWarning ?: "none recorded"}")

    appendLine()
    appendLine("Frame and present counters")
    val currentStats = stats
    if (currentStats == null) {
        appendLine("FPS: not measured")
        appendLine("Frame attempts / submissions / accepted presents: unavailable")
        appendLine("Draw calls / submitted triangles: unavailable")
    } else {
        val fps = if (currentStats.hasValidFrameTiming) {
            String.format(Locale.ROOT, "%.1f FPS (%.2f ms input-delta EMA; not present-completion timing)", currentStats.fps, currentStats.frameMs)
        } else {
            "not measured"
        }
        appendLine("FPS: $fps")
        appendLine("Frame attempts / submissions / accepted presents: ${currentStats.frameAttempts} / ${currentStats.frameSubmissions} / ${currentStats.presentRequestsAccepted}")
        appendLine("Draw calls / submitted triangles: ${currentStats.drawCalls} / ${currentStats.triangles}")
    }
    appendLine("Pixel diagnostic: ${pixelCheck?.summary ?: "not run (no pixel readback requested)"}")
    appendLine("Pixel-diagnostic rung selected / effective: ${diagnosticRung.value} (${diagnosticRung.displayName}) / ${effectiveDiagnosticRung.value} (${effectiveDiagnosticRung.displayName})")
    appendLine("Last error: ${lastError?.takeIf { it.isNotBlank() } ?: "none"}")
    appendLine()
    appendLine("Privacy: this report contains technical build/device/rendering diagnostics only. It excludes prompts, reference images, avatar content and account identifiers; sharing starts only after you choose a destination.")
}

/** Extract the complete Vulkan cause from a two-backend startup failure without dropping its detail. */
internal fun extractVulkanInitializationWarning(lastError: String?): String? {
    if (lastError.isNullOrBlank()) return null
    val marker = "Vulkan: "
    val start = lastError.indexOf(marker)
    if (start < 0) return null
    val detailStart = start + marker.length
    val otherBackend = lastError.indexOf("; OpenGL ES 3.1:", detailStart)
    val detailEnd = if (otherBackend >= 0) otherBackend else lastError.length
    val detail = lastError.substring(detailStart, detailEnd).trim()
    return detail.takeIf { it.isNotEmpty() }?.let { "$marker$it" }
}
