/*
 * AvatarForge :: AvatarForge.kt
 *
 * The SDK's face to a host app: measuring device capability, deciding which quality preset this
 * device can actually hold at 30-60 FPS, and moving exported files out of the app through a
 * FileProvider-backed share intent.
 *
 * The device-tier heuristic is deliberately conservative and honest. It reads RAM and the reported
 * GPU (through the renderer, once one exists) and picks a preset; it never promises a frame rate,
 * it predicts one and then lets the renderer's own budget controller lower quality if the frames
 * run long. Every number it uses is measured on the device it is running on.
 */
package com.avatarforge.sdk

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.os.StatFs
import androidx.core.content.FileProvider
import com.avatarforge.sdk.render.Quality
import com.avatarforge.sdk.render.RenderSettings
import java.io.File

object AvatarForge {

    /** FileProvider authority declared in the SDK manifest (merged into the app's). */
    fun fileProviderAuthority(context: Context): String = "${context.packageName}.avatarforge.fileprovider"

    /** A content:// URI for a generated file, valid for the process and shareable in an Intent. */
    fun uriFor(context: Context, file: File): Uri =
        FileProvider.getUriForFile(context, fileProviderAuthority(context), file)

    /** A share intent for a generated avatar. Uses only the app-scoped provider - no network share. */
    fun shareIntent(context: Context, result: EngineResult, subject: String = "AvatarForge avatar"): Intent {
        val uri = uriFor(context, result.glbFile)
        return Intent(Intent.ACTION_SEND).apply {
            type = "model/gltf-binary"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, subject)
            putExtra(Intent.EXTRA_TEXT, result.summary())
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    /**
     * Copies the generated .glb to a destination the user chose (Storage Access Framework). The
     * app never needs WRITE_EXTERNAL_STORAGE: it writes through the URI the picker returned.
     */
    fun copyToDestination(context: Context, source: File, destination: Uri): Long {
        val output = context.contentResolver.openOutputStream(destination, "wt")
            ?: error("the chosen destination could not be opened for writing")
        output.use { out ->
            source.inputStream().use { input -> return input.copyTo(out) }
        }
    }

    /**
     * Chooses a render preset from the device's RAM.
     *
     * 6 GB and up: HIGH (2048 shadow map, 4x MSAA, 5 bloom levels).
     * 4 GB:        MEDIUM (2048/2x/4).
     * below 4 GB:  LOW (1024/no MSAA/3) - the kernel also gets a smaller texture atlas in that case
     *              (see AvatarEngine's memory budget).
     *
     * The thresholds are where they are because the *engine* peaks near 1.5 GB during a build and
     * the renderer holds a 2048^2 RGBA8 atlas set (~68 MB) plus MSAA targets; a device with 4 GB
     * has ~1.8-2.2 GB available to a foreground app, so that is the tier where 2048 maps stop being
     * safe alongside the build.
     */
    fun recommendedSettings(context: Context): RenderSettings {
        val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val info = ActivityManager.MemoryInfo()
        activityManager.getMemoryInfo(info)
        val resources = DeviceResourceGovernor(context).snapshot()
        val gb = info.totalMem / (1024.0 * 1024.0 * 1024.0)
        val lowRam = activityManager.isLowRamDevice
        val thermal = resources.thermalStatus
        val severeThermal = thermal != null && thermal >= PowerManager.THERMAL_STATUS_SEVERE
        val moderateThermal = thermal != null && thermal >= PowerManager.THERMAL_STATUS_MODERATE
        val preset = when {
            lowRam || gb < 4.0 || info.lowMemory ||
                resources.availableRamBytes < 1536L * 1024 * 1024 ||
                resources.state == DeviceResourceState.LOW_STORAGE || severeThermal -> Quality.LOW
            gb < 6.0 || resources.availableRamBytes < 2L * 1024 * 1024 * 1024 ||
                moderateThermal -> Quality.MEDIUM
            else -> Quality.HIGH
        }
        return RenderSettings(
            quality = preset,
            msaa = if (preset == Quality.HIGH) 4 else if (preset == Quality.MEDIUM) 2 else 1,
            shadowMapSize = if (preset == Quality.HIGH) 2048 else if (preset == Quality.MEDIUM) 2048 else 1024,
            bloomLevels = if (preset == Quality.HIGH) 5 else if (preset == Quality.MEDIUM) 4 else 3,
        )
    }

    /** Free space check before a build: the export is 33-40 MB and the build is in-memory. */
    fun hasRoomForExport(context: Context, requiredBytes: Long = 64L * 1024 * 1024): Boolean {
        val stat = StatFs(context.filesDir.absolutePath)
        return stat.availableBytes > requiredBytes
    }

    /** One-paragraph, human-readable device report for the settings screen's diagnostics card. */
    fun deviceReport(context: Context): String {
        val health = AvatarEngine.health()
        val caps = runCatching { AvatarEngine.capabilities(context) }.getOrNull()
        val stat = StatFs(context.filesDir.absolutePath)
        return buildString {
            appendLine("Kernel      : ${health.engineVersion} (ABI ${health.abiVersion})")
            appendLine("Native      : ${if (health.nativeLibraryLoaded) "loaded" else "FAILED - ${health.problem}"}")
            appendLine("Device      : ${Build.MANUFACTURER} ${Build.MODEL} (API ${Build.VERSION.SDK_INT}, ${Build.SUPPORTED_ABIS.firstOrNull()})")
            caps?.let {
                appendLine("RAM         : ${"%.1f".format(it.totalRamBytes / 1073741824.0)} GB${if (it.lowRamDevice) " (low-RAM)" else ""}")
                appendLine("Engine budget: ${it.memoryBudgetBytes / 1048576} MB")
                appendLine("Clips       : ${it.clipCount}, blendshapes: ${it.blendshapeCount}, regions: ${it.regionCount}")
            }
            appendLine("Free storage: ${stat.availableBytes / 1048576} MB")
            val resources = DeviceResourceGovernor(context).snapshot()
            appendLine("Available RAM: ${resources.availableRamBytes / 1048576} MB")
            appendLine("Resource state: ${resources.state}")
            appendLine("Thermal status: ${resources.thermalStatus?.let { thermal ->
                when (thermal) {
                    PowerManager.THERMAL_STATUS_NONE -> "none"
                    PowerManager.THERMAL_STATUS_LIGHT -> "light"
                    PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
                    PowerManager.THERMAL_STATUS_SEVERE -> "severe"
                    PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
                    PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
                    PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
                    else -> "unknown($thermal)"
                }
            } ?: "unavailable on this Android version"}")
        }
    }
}
