package com.avatarforge.sdk.render

import android.content.Context
import android.os.Build
import android.os.PowerManager
import java.util.concurrent.Executor

/** Maps a requested preset to a conservative effective tier as Android thermal pressure rises. */
internal object RenderThermalPolicy {
    fun effectiveQuality(requested: Quality, thermalStatus: Int?): Quality = when {
        thermalStatus == null -> requested
        thermalStatus >= PowerManager.THERMAL_STATUS_SEVERE -> Quality.LOW
        thermalStatus >= PowerManager.THERMAL_STATUS_MODERATE -> when (requested) {
            Quality.HIGH -> Quality.MEDIUM
            Quality.MEDIUM -> Quality.LOW
            Quality.AUTO -> Quality.MEDIUM
            Quality.LOW -> Quality.LOW
        }
        else -> requested
    }
}

/** API-29 thermal hooks isolated so the SurfaceView remains loadable on the SDK's API-26 minimum. */
internal object AndroidThermalMonitor {
    fun currentStatus(context: Context): Int? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        return runCatching { Api29.currentStatus(context) }.getOrNull()
    }

    fun register(context: Context, executor: Executor, onChange: (Int) -> Unit): AutoCloseable? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        return runCatching { Api29.register(context, executor, onChange) }.getOrNull()
    }

    private object Api29 {
        fun currentStatus(context: Context): Int? =
            context.getSystemService(PowerManager::class.java)?.currentThermalStatus

        fun register(context: Context, executor: Executor, onChange: (Int) -> Unit): AutoCloseable? {
            val manager = context.getSystemService(PowerManager::class.java) ?: return null
            val listener = PowerManager.OnThermalStatusChangedListener { status -> onChange(status) }
            manager.addThermalStatusListener(executor, listener)
            return AutoCloseable {
                runCatching { manager.removeThermalStatusListener(listener) }
            }
        }
    }
}
