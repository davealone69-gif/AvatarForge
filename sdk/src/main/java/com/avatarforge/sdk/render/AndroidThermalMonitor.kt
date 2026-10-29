package com.avatarforge.sdk.render

import android.content.Context
import android.os.Build
import android.os.PowerManager
import java.util.concurrent.Executor

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
