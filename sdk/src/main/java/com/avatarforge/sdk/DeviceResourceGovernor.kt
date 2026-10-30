package com.avatarforge.sdk

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.os.PowerManager
import android.os.StatFs

/**
 * A measured snapshot of resources available to AvatarForge.
 *
 * This is a policy input, not a guarantee of available memory or frame rate. Android can reclaim
 * memory at any time, and thermal state can change after this snapshot; callers should refresh it
 * before starting a heavy generation job and after a job completes.
 */
enum class DeviceResourceState {
    NORMAL,
    LOW_MEMORY,
    LOW_STORAGE,
    THERMAL_LIMIT
}

data class DeviceResourceSnapshot(
    val state: DeviceResourceState,
    val availableRamBytes: Long,
    val totalRamBytes: Long,
    val availableStorageBytes: Long,
    val lowMemory: Boolean,
    val cpuCores: Int,
    val thermalStatus: Int?
) {
    val generationShouldBeDeferred: Boolean
        get() = lowMemory ||
            state == DeviceResourceState.LOW_MEMORY ||
            state == DeviceResourceState.LOW_STORAGE ||
            state == DeviceResourceState.THERMAL_LIMIT
}

/**
 * Android resource governor.
 *
 * Snapshot values are observations, not guarantees. Emergency stop cooperatively cancels the
 * isolated candidate build and releases its optional image references; it never destroys or
 * detaches the active character or renderer. AvatarForge has no optional neural model bundle to
 * unload, and active character textures are required for continued rendering.
 */
class DeviceResourceGovernor(private val context: Context) {
    /** Requests a cooperative stop for the current build; false means there was no running job. */
    fun emergencyStop(job: JobHandle?): Boolean {
        if (job == null || !job.status.value.running) return false
        job.emergencyStop()
        return true
    }

    fun snapshot(): DeviceResourceSnapshot {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val memory = ActivityManager.MemoryInfo().also(manager::getMemoryInfo)
        val storage = StatFs(context.filesDir.absolutePath).availableBytes
        val thermal = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            context.getSystemService(PowerManager::class.java)?.currentThermalStatus
        } else {
            null
        }
        val state = when {
            memory.lowMemory || memory.availMem < MIN_AVAILABLE_RAM_BYTES ->
                DeviceResourceState.LOW_MEMORY
            storage < MIN_AVAILABLE_STORAGE_BYTES ->
                DeviceResourceState.LOW_STORAGE
            thermal != null && thermal >= PowerManager.THERMAL_STATUS_SEVERE ->
                DeviceResourceState.THERMAL_LIMIT
            else -> DeviceResourceState.NORMAL
        }
        return DeviceResourceSnapshot(
            state = state,
            availableRamBytes = memory.availMem,
            totalRamBytes = memory.totalMem,
            availableStorageBytes = storage,
            lowMemory = memory.lowMemory,
            cpuCores = Runtime.getRuntime().availableProcessors(),
            thermalStatus = thermal
        )
    }

    companion object {
        const val MIN_AVAILABLE_RAM_BYTES: Long = 1024L * 1024 * 1024
        const val MIN_AVAILABLE_STORAGE_BYTES: Long = 512L * 1024 * 1024
    }
}
