/*
 * AvatarForge :: Diagnostics.kt
 *
 * Structured, copy-pasteable device/engine/renderer diagnostics. Every value here is measured
 * or reported by the platform or the native layer - nothing is guessed - and every FAIL/WARN
 * carries a hint the user can act on without reading source code. The same report powers the
 * in-app diagnostics panel (structured rows) and the "Copy report" action (plain text), so a bug
 * report contains exactly what the screen showed.
 *
 * The collector is deliberately renderer-optional: engine/device diagnostics work on the input
 * screen before any Surface exists; the renderer section is filled in once the viewer has one.
 */
package com.avatarforge.sdk

import android.content.Context
import android.os.Build
import android.os.PowerManager
import com.avatarforge.sdk.render.RendererDiagnostics
import com.avatarforge.sdk.render.RendererInfo
import com.avatarforge.sdk.render.RendererStartupState
import com.avatarforge.sdk.render.RendererStats
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Traffic-light status for one diagnostic row. */
enum class DiagStatus { OK, WARN, FAIL, INFO }

/** One check. [hint] is shown (and copied) whenever the row is not OK/INFO. */
data class DiagItem(
    val area: String,
    val label: String,
    val detail: String,
    val status: DiagStatus,
    val hint: String? = null,
)

/**
 * The full snapshot. [rendererDiagnostics] is null before the viewer's Surface exists; the
 * renderer rows then say so instead of pretending a GPU was probed.
 */
data class DiagnosticsReport(
    val collectedAtMs: Long,
    val items: List<DiagItem>,
    val rendererDiagnostics: RendererDiagnostics?,
    val rendererInfo: RendererInfo?,
    val rendererStats: RendererStats?,
) {
    val worstStatus: DiagStatus
        get() = when {
            items.any { it.status == DiagStatus.FAIL } -> DiagStatus.FAIL
            items.any { it.status == DiagStatus.WARN } -> DiagStatus.WARN
            else -> DiagStatus.OK
        }

    /** Stable, human-readable rendering for the clipboard / a bug report. */
    val plainText: String
        get() = buildString {
            val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss z", Locale.US).format(Date(collectedAtMs))
            appendLine("AvatarForge diagnostics - $stamp")
            appendLine("Overall: ${worstStatus.name}")
            var lastArea = ""
            for (item in items) {
                if (item.area != lastArea) {
                    appendLine()
                    appendLine("== ${item.area} ==")
                    lastArea = item.area
                }
                appendLine("[${item.status.name}] ${item.label}: ${item.detail}")
                item.hint?.let { appendLine("      hint: $it") }
            }
            rendererDiagnostics?.takeIf { it.attempts.isNotEmpty() }?.let { d ->
                appendLine()
                appendLine("== Renderer start-up attempts ==")
                appendLine("Final state: ${d.state.name} · frames at last state change: ${d.framesRendered}")
                for (a in d.attempts) {
                    val backend = if (a.preferVulkan) "Vulkan preferred" else "GLES forced"
                    val outcome = if (a.succeeded) "OK" else "FAIL"
                    appendLine("Attempt ${a.attempt} ($backend): $outcome in ${a.elapsedMs} ms" +
                        (a.error?.let { " - $it" } ?: ""))
                }
                if (d.tierTransitions.isNotEmpty()) {
                    appendLine("Quality tier changes: ${d.tierTransitions.joinToString(" · ")}")
                }
            }
        }
}

/**
 * Builds a [DiagnosticsReport] from measured state only. Allows null renderer pieces so the same
 * call works on the input screen (nothing GPU-related exists yet) and in the viewer.
 */
fun collectDiagnostics(
    context: Context,
    rendererDiagnostics: RendererDiagnostics? = null,
    rendererInfo: RendererInfo? = null,
    rendererStats: RendererStats? = null,
): DiagnosticsReport {
    val items = ArrayList<DiagItem>()
    val health = AvatarEngine.health()
    val resources = DeviceResourceGovernor(context).snapshot()

    /* ------------------------------------------------------------ app / native engine ------- */
    val appArea = "App"
    val version = runCatching {
        @Suppress("DEPRECATION")
        context.packageManager.getPackageInfo(context.packageName, 0)
            .let { pi ->
                val code = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
                "${pi.versionName ?: "unknown"} ($code)"
            }
    }.getOrDefault("unavailable")
    items += DiagItem(appArea, "App version", version, DiagStatus.INFO)

    if (health.nativeLibraryLoaded) {
        items += DiagItem(appArea, "Native library", "libavatarforge.so loaded", DiagStatus.OK)
    } else {
        items += DiagItem(
            appArea, "Native library",
            "failed to load: ${health.problem ?: "unknown reason"}",
            DiagStatus.FAIL,
            "Reinstall AvatarForge - the native library for this CPU ABI is missing or unreadable.",
        )
    }

    if (health.usable) {
        items += DiagItem(
            appArea, "Engine",
            "version ${health.engineVersion} · ABI ${health.abiVersion} · healthy",
            DiagStatus.OK,
        )
    } else {
        items += DiagItem(
            appArea, "Engine",
            health.problem ?: "not usable",
            DiagStatus.FAIL,
            "Reinstall the app; an ABI mismatch cannot be repaired at runtime.",
        )
    }

    runCatching { AvatarEngine.capabilities(context) }.onSuccess { caps ->
        items += DiagItem(
            appArea, "Engine content",
            "${caps.clipCount} clips · ${caps.blendshapeCount} blendshapes · ${caps.regionCount} regions",
            DiagStatus.INFO,
        )
    }

    /* ---------------------------------------------------------------------------- device ---- */
    val deviceArea = "Device"
    val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown ABI"
    items += DiagItem(
        deviceArea, "Model",
        "${Build.MANUFACTURER} ${Build.MODEL} · Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}) · $abi",
        DiagStatus.INFO,
    )
    val totalGb = resources.totalRamBytes / 1073741824.0
    val availMb = resources.availableRamBytes / 1048576
    val memoryStatus = when {
        resources.lowMemory || resources.availableRamBytes < DeviceResourceGovernor.MIN_AVAILABLE_RAM_BYTES -> DiagStatus.WARN
        else -> DiagStatus.OK
    }
    items += DiagItem(
        deviceArea, "Memory",
        "%.1f GB total · %d MB available%s".format(totalGb, availMb, if (resources.lowMemory) " · low-RAM device" else ""),
        memoryStatus,
        if (memoryStatus == DiagStatus.WARN) "Close background apps; a generation peaks near 1.5 GB of native memory." else null,
    )
    val freeStorageMb = resources.availableStorageBytes / 1048576
    val storageStatus = if (resources.availableStorageBytes < DeviceResourceGovernor.MIN_AVAILABLE_STORAGE_BYTES) DiagStatus.WARN else DiagStatus.OK
    items += DiagItem(
        deviceArea, "Storage",
        "$freeStorageMb MB free in app storage",
        storageStatus,
        if (storageStatus == DiagStatus.WARN) "Free space; an exported avatar needs about 64 MB." else null,
    )

    val thermalLabel: String
    val thermalStatus: DiagStatus
    val thermalHint: String?
    if (resources.thermalStatus == null) {
        thermalLabel = "unavailable on this Android version"
        thermalStatus = DiagStatus.INFO
        thermalHint = null
    } else {
        val t = resources.thermalStatus
        thermalLabel = when (t) {
            PowerManager.THERMAL_STATUS_NONE -> "none"
            PowerManager.THERMAL_STATUS_LIGHT -> "light"
            PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
            PowerManager.THERMAL_STATUS_SEVERE -> "severe"
            PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
            PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
            PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
            else -> "unknown($t)"
        }
        thermalStatus = when {
            t >= PowerManager.THERMAL_STATUS_SEVERE -> DiagStatus.FAIL
            t >= PowerManager.THERMAL_STATUS_MODERATE -> DiagStatus.WARN
            else -> DiagStatus.OK
        }
        thermalHint = if (thermalStatus != DiagStatus.OK)
            "Let the device cool down; the renderer steps quality down and a build may be deferred under heat."
        else null
    }
    items += DiagItem(deviceArea, "Thermal", thermalLabel, thermalStatus, thermalHint)
    items += DiagItem(deviceArea, "CPU cores", resources.cpuCores.toString(), DiagStatus.INFO)

    /* --------------------------------------------------------------------------- renderer ---- */
    val rendererArea = "Renderer"
    if (rendererDiagnostics == null) {
        items += DiagItem(
            rendererArea, "Start-up",
            "not started - the renderer is created when the viewer opens",
            DiagStatus.INFO,
        )
    } else {
        val d = rendererDiagnostics
        val stateStatus = when (d.state) {
            RendererStartupState.RUNNING -> DiagStatus.OK
            RendererStartupState.FAILED -> DiagStatus.FAIL
            else -> DiagStatus.INFO
        }
        items += DiagItem(
            rendererArea, "Start-up",
            "${d.state.name} · ${d.attempts.size} attempt(s) · ${d.framesRendered} frame(s) at last state change",
            stateStatus,
            if (d.state == RendererStartupState.FAILED)
                "Open Render settings, turn off 'Prefer Vulkan (else GLES 3.1)', then tap Retry renderer."
            else null,
        )
        d.lastError?.let {
            items += DiagItem(rendererArea, "Last renderer error", it, DiagStatus.FAIL)
        }
        items += DiagItem(
            rendererArea, "Surface lifecycle",
            "created ${d.surfaceCreates} · changed ${d.surfaceChanges} " +
                "(last ${d.lastSurfaceWidth}x${d.lastSurfaceHeight}) · destroyed ${d.surfaceDestroys}",
            DiagStatus.INFO,
        )
        items += DiagItem(
            rendererArea, "Frame loop restarts",
            d.frameLoopRestarts.toString(),
            DiagStatus.INFO,
            if (d.frameLoopRestarts > 3)
                "Several restarts while the viewer was open can look like flashing; note this number in any report."
            else null,
        )
        if (d.tierTransitions.isNotEmpty()) {
            items += DiagItem(
                rendererArea, "Quality tier changes",
                d.tierTransitions.joinToString(" · "),
                DiagStatus.INFO,
                "The adaptive controller stepped MSAA/shadow/bloom quality to hold the frame budget.",
            )
        }
    }

    rendererInfo?.let { info ->
        items += DiagItem(
            rendererArea, "Backend",
            "${info.backend} · ${info.deviceName} · ${info.apiVersion} · ${info.driver}",
            DiagStatus.OK,
        )
        items += DiagItem(
            rendererArea, "GPU features",
            "shadows=${info.supportsShadows} · MSAA=${info.supportsMsaa} · " +
                "float HDR=${info.supportsFloatHdr} · anisotropy=${info.supportsAnisotropy}",
            DiagStatus.INFO,
        )
    }

    rendererStats?.let { stats ->
        if (rendererDiagnostics?.state == RendererStartupState.RUNNING) {
            val fpsStatus = if (stats.fps in 0.01f..29.99f) DiagStatus.WARN else DiagStatus.OK
            items += DiagItem(
                rendererArea, "Frame rate",
                "%.0f FPS · %.1f ms frame · quality tier %d · %d draw calls · %d triangles".format(
                    stats.fps, stats.frameMs, stats.qualityLevel, stats.drawCalls, stats.triangles,
                ),
                fpsStatus,
                if (fpsStatus == DiagStatus.WARN)
                    "Set quality to AUTO (or LOW) in Render settings so the budget controller can step down."
                else null,
            )
        }
    }

    return DiagnosticsReport(
        collectedAtMs = System.currentTimeMillis(),
        items = items,
        rendererDiagnostics = rendererDiagnostics,
        rendererInfo = rendererInfo,
        rendererStats = rendererStats,
    )
}
