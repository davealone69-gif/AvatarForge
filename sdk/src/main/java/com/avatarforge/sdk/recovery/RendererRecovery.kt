/*
 * AvatarForge :: deterministic renderer recovery (no AI, no network).
 *
 * This is a bounded stability path, not autonomous code modification. It classifies a renderer
 * failure, retries with known-safe GLES settings (including after a Vulkan fault), and records
 * enough evidence for a human or CI regression test. It never claims pixels are visible: the probe
 * only verifies sustained frame submission with non-zero submitted geometry.
 */
package com.avatarforge.sdk.recovery

import com.avatarforge.sdk.render.RenderBackend
import com.avatarforge.sdk.render.RenderSettings
import com.avatarforge.sdk.render.RendererStats
import com.avatarforge.sdk.render.Quality
import java.security.MessageDigest
import java.util.Locale

enum class RecoveryStage { STARTUP, FRAME, RESIZE }

/** The backend actually active in this app, not the OS implementation underneath GLES. */
enum class RecoveryBackend {
    GLES, VULKAN, UNKNOWN;
    companion object {
        fun from(infoBackend: String?): RecoveryBackend = when {
            infoBackend?.startsWith("gles", ignoreCase = true) == true -> GLES
            infoBackend.equals("vulkan", ignoreCase = true) -> VULKAN
            else -> UNKNOWN
        }
    }
}

enum class RecoveryFaultCategory {
    CONTEXT_LOST, OUT_OF_MEMORY, SHADER_COMPILE, SHADER_LINK, EGL_CONTEXT, EGL_SURFACE,
    FRAMEBUFFER, GL_INVALID_OPERATION, VULKAN_DEVICE_LOST, VULKAN_SURFACE_LOST,
    VULKAN_FAILURE, RESIZE, UNKNOWN
}

data class RecoveryFailure(
    val stage: RecoveryStage,
    val category: RecoveryFaultCategory,
    val fingerprint: String,
)

object RecoveryFailureClassifier {
    fun classify(stage: RecoveryStage, message: String?): RecoveryFailure {
        val raw = message.orEmpty()
        val text = raw.uppercase(Locale.ROOT)
        val category = when {
            "EGL_CONTEXT_LOST" in text || "CONTEXT_LOST" in text -> RecoveryFaultCategory.CONTEXT_LOST
            "GL_OUT_OF_MEMORY" in text || "EGL_BAD_ALLOC" in text || "OUT OF MEMORY" in text -> RecoveryFaultCategory.OUT_OF_MEMORY
            "COMPILE" in text && "SHADER" in text -> RecoveryFaultCategory.SHADER_COMPILE
            "LINK" in text && ("SHADER" in text || "PROGRAM" in text) -> RecoveryFaultCategory.SHADER_LINK
            "EGLCREATECONTEXT" in text || "EGLMAKECURRENT" in text || "EGLINITIALIZE" in text -> RecoveryFaultCategory.EGL_CONTEXT
            "EGLCREATEWINDOWSURFACE" in text || "EGL_BAD_SURFACE" in text || "NATIVE WINDOW" in text -> RecoveryFaultCategory.EGL_SURFACE
            "FRAMEBUFFER" in text -> RecoveryFaultCategory.FRAMEBUFFER
            "GL_INVALID_OPERATION" in text -> RecoveryFaultCategory.GL_INVALID_OPERATION
            "VK_ERROR_DEVICE_LOST" in text || "VKRESULT -4" in text -> RecoveryFaultCategory.VULKAN_DEVICE_LOST
            "VK_ERROR_SURFACE_LOST_KHR" in text || "VKRESULT -1000000000" in text -> RecoveryFaultCategory.VULKAN_SURFACE_LOST
            "VKRESULT" in text || "VKCREATE" in text || "VKQUEUE" in text -> RecoveryFaultCategory.VULKAN_FAILURE
            stage == RecoveryStage.RESIZE -> RecoveryFaultCategory.RESIZE
            else -> RecoveryFaultCategory.UNKNOWN
        }
        return RecoveryFailure(stage, category, fingerprint(stage, raw))
    }

    /** Stable, non-reversible fault id. The raw driver message is not persisted. */
    fun fingerprint(stage: RecoveryStage, message: String?): String {
        val normalized = message.orEmpty()
            .replace(Regex("0[xX][0-9a-fA-F]+"), "0x#")
            .replace(Regex("\\b\\d+\\b"), "#")
            .replace(Regex("\\s+"), " ")
            .trim()
        val digest = MessageDigest.getInstance("SHA-256")
            .digest("${stage.name}|$normalized".toByteArray(Charsets.UTF_8))
        return digest.joinToString("") { "%02x".format(it) }.take(16)
    }
}

data class RecoveryCandidate(
    val id: String,
    val label: String,
    val backend: RecoveryBackend,
    val settings: RenderSettings,
)

data class RecoveryStatus(val phase: String, val message: String)

object RendererRecoveryPolicy {
    const val MAX_AUTOMATIC_ATTEMPTS = 1

    /** One conservative GLES retry; never loop a fatal Vulkan frame on the same API. */
    fun safeSettings(current: RenderSettings): RenderSettings = current.copy(
        backend = RenderBackend.GLES, quality = Quality.LOW, shadows = false, bloom = false, sss = false,
        groundPlane = false, msaa = 1, shadowMapSize = 1024, bloomLevels = 0,
    )

    fun isSafe(settings: RenderSettings): Boolean =
        settings.backend == RenderBackend.GLES && settings.quality == Quality.LOW &&
            !settings.shadows && !settings.bloom && !settings.sss &&
            !settings.groundPlane && settings.msaa <= 1 && settings.shadowMapSize <= 1024 && settings.bloomLevels <= 0

    fun configurationId(settings: RenderSettings, backend: RecoveryBackend): String =
        "${if (isSafe(settings)) "safe" else "custom"}-${backend.name.lowercase(Locale.ROOT)}"

    fun next(
        current: RenderSettings,
        actualBackend: RecoveryBackend,
        automaticAttempts: Int,
        tried: Set<String>,
    ): RecoveryCandidate? {
        if (automaticAttempts >= MAX_AUTOMATIC_ATTEMPTS) return null
        val id = "safe-gles"
        if (id in tried) return null
        return RecoveryCandidate(id, "safe OpenGL ES", RecoveryBackend.GLES, safeSettings(current))
    }
}

enum class RecoveryProbeOutcome { FRAME_SUBMISSION_STABLE, PROBE_TIMEOUT }

/**
 * Deterministic post-recovery regression check. It proves only that the renderer keeps submitting
 * non-empty frames; it is not a visibility, likeness, or GPU-correctness oracle.
 */
class RecoveryFrameProbe(
    private val requiredFrames: Int = 30,
    private val timeoutFrames: Int = 180,
) {
    private var consecutive = 0
    private var total = 0

    init {
        require(requiredFrames in 1..timeoutFrames)
    }

    fun observe(stats: RendererStats): RecoveryProbeOutcome? {
        total++
        val valid = stats.hasValidFrameTiming &&
            stats.triangles > 0 && stats.drawCalls > 0
        consecutive = if (valid) consecutive + 1 else 0
        return when {
            consecutive >= requiredFrames -> RecoveryProbeOutcome.FRAME_SUBMISSION_STABLE
            total >= timeoutFrames -> RecoveryProbeOutcome.PROBE_TIMEOUT
            else -> null
        }
    }
}

object RecoveryAdvisor {
    fun recommendation(records: List<RecoveryRecord>): String {
        val lastFailure = records.lastOrNull { it.kind == RecoveryRecordKind.FAILURE }
        if (lastFailure == null) {
            val fallback = records.lastOrNull { it.kind == RecoveryRecordKind.BACKEND_FALLBACK }
            return if (fallback != null) {
                "Native Vulkan startup fell back to OpenGL ES. Check the actual backend and driver in Self diagnostics; frame submission does not prove visible pixels."
            } else {
                "No renderer fault is recorded. Native Vulkan is requested by default; GLES is the fallback. Check the actual backend and visible output."
            }
        }
        val lastStable = records.lastOrNull { it.kind == RecoveryRecordKind.FRAME_SUBMISSION_STABLE }
        if (lastStable != null && lastStable.timestampMs >= lastFailure.timestampMs) {
            return "Frame submission recovered with safe settings. Visible output still needs human confirmation; export this report before changing quality."
        }
        val repeats = records.count { it.kind == RecoveryRecordKind.FAILURE && it.fingerprint == lastFailure.fingerprint }
        val base = when (lastFailure.category) {
            RecoveryFaultCategory.OUT_OF_MEMORY -> "Keep LOW quality, close other apps, and retry. If repeated, export the report for a memory-budget regression."
            RecoveryFaultCategory.SHADER_COMPILE, RecoveryFaultCategory.SHADER_LINK -> "Try Safe recovery and export the report; the GLES shader/driver pair needs a source regression test."
            RecoveryFaultCategory.EGL_CONTEXT, RecoveryFaultCategory.EGL_SURFACE, RecoveryFaultCategory.CONTEXT_LOST -> "Reopen the viewer to recreate the Surface, then try Safe recovery. If repeated, export the lifecycle report."
            RecoveryFaultCategory.FRAMEBUFFER -> "Safe recovery disables MSAA/HDR-dependent extras. If repeated, export the report for a render-target regression."
            RecoveryFaultCategory.GL_INVALID_OPERATION -> "Try Safe recovery; export the report so the invalid GL call can be traced."
            RecoveryFaultCategory.VULKAN_DEVICE_LOST, RecoveryFaultCategory.VULKAN_SURFACE_LOST,
            RecoveryFaultCategory.VULKAN_FAILURE -> "Switch to safe OpenGL ES and export the report with the failing VkResult. Do not loop a broken Vulkan surface."
            RecoveryFaultCategory.RESIZE -> "Retry after rotation/resize completes. If repeated, export the report for a resize-target regression."
            RecoveryFaultCategory.UNKNOWN, null -> "Try Safe recovery once, then export the report. Do not keep retrying in a loop."
        }
        return if (repeats >= 3) "Repeated fault fingerprint ${lastFailure.fingerprint}. $base" else base
    }
}
