/*
 * AvatarForge :: RendererDiagnosticNotes.kt
 *
 * Pure-Kotlin interpretation rules for renderer diagnostics, deliberately free of Android types
 * so the wording is locked by JVM unit tests. The collector reports measured values only; these
 * functions decide which explanation an honest report owes the reader when measured values alone
 * could mislead.
 *
 * Background: a physical-device report (SM-S921B / Xclipse 940, 2026-10-24) showed
 *   "Attempt 1 (Vulkan preferred): OK" next to backend "gles31 · ANGLE ..." and four frame-loop
 * restarts with zero surface destroys. Both are correct behaviour, but the old wording made the
 * first look contradictory and the second look alarming. Findings AF-017 / AF-018.
 */
package com.avatarforge.sdk

import com.avatarforge.sdk.render.RendererDiagnostics
import com.avatarforge.sdk.render.RendererInfo

object RendererDiagnosticNotes {

    /**
     * The frame-loop counter increments in three benign situations (host visibility changes, the
     * one successful renderer create, the one real surface resize) as well as during genuine
     * surface churn. Only churn can flash; a high count against an untouched, single-attempt
     * surface therefore gets an explanatory note instead of the flashing warning (AF-017).
     */
    fun frameLoopRestartHint(d: RendererDiagnostics): String? {
        if (d.frameLoopRestarts <= 3) return null
        val stableSurface = d.surfaceDestroys == 0 && d.surfaceCreates <= 1 &&
            d.attempts.size <= 1
        return if (stableSurface) {
            "Benign loop reschedules (visibility, renderer creation and the surface resize); " +
                "no surface was destroyed, so these restarts cannot cause flashing."
        } else {
            "Restarts together with surface recreation can look like flashing; note the " +
                "created/destroyed counts in any report."
        }
    }

    /**
     * Native creation tries Vulkan first and silently falls back to GLES 3.1 (af_render.cpp);
     * the Vulkan rejection reason is discarded when the fallback succeeds. When the report shows
     * a Vulkan-preferred attempt alongside a non-Vulkan active backend, state that observed fact
     * explicitly and mark the reason unknown, rather than implying Vulkan is driving the GPU
     * (AF-018). Returns null when Vulkan is active, was not requested, or no renderer exists yet.
     */
    fun backendFallbackItem(info: RendererInfo?, d: RendererDiagnostics?): DiagItem? {
        if (info == null || d == null) return null
        if (info.isVulkan) return null
        if (d.attempts.none { it.preferVulkan }) return null
        return DiagItem(
            area = "Renderer",
            label = "Backend selection",
            detail = "Vulkan was preferred, but native Vulkan initialisation was declined; " +
                "the automatic GLES 3.1 fallback is active (backend '${info.backend}').",
            status = DiagStatus.INFO,
            hint = "The native layer does not yet expose the Vulkan rejection reason; include " +
                "this report's device and driver row when asking about it. GLES 3.1 is a fully " +
                "supported backend.",
        )
    }
}
