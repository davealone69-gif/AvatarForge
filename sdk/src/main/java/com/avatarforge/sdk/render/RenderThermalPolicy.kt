package com.avatarforge.sdk.render

/**
 * Deterministic thermal quality cap. Pure Kotlin so the policy is JVM-testable; Android thermal
 * plumbing lives in [AndroidThermalMonitor]. Unknown thermal state is reported as unknown and does
 * not invent a measured temperature. Severe pressure caps optional GPU cost and halves the frame
 * pacing target; it never changes the avatar, camera, exposure or material identity.
 */
object RenderThermalPolicy {
    const val THERMAL_NONE = 0
    const val THERMAL_LIGHT = 1
    const val THERMAL_MODERATE = 2
    const val THERMAL_SEVERE = 3
    const val THERMAL_CRITICAL = 4
    const val THERMAL_EMERGENCY = 5
    const val THERMAL_SHUTDOWN = 6

    fun effectiveQuality(requested: Quality, thermalStatus: Int?): Quality = when {
        thermalStatus == null -> requested
        thermalStatus >= THERMAL_SEVERE -> Quality.LOW
        thermalStatus >= THERMAL_MODERATE -> when (requested) {
            Quality.HIGH -> Quality.MEDIUM
            Quality.AUTO -> Quality.MEDIUM
            Quality.MEDIUM -> Quality.LOW
            Quality.LOW -> Quality.LOW
        }
        else -> requested
    }

    fun cap(settings: RenderSettings, thermalStatus: Int?): RenderSettings =
        settings.copy(quality = effectiveQuality(settings.quality, thermalStatus))

    fun frameIntervalNs(thermalStatus: Int?): Long =
        if (thermalStatus != null && thermalStatus >= THERMAL_SEVERE) 33_333_333L else 16_666_667L

    fun describe(thermalStatus: Int?): String = when (thermalStatus) {
        null -> "unknown"
        THERMAL_NONE -> "none"
        THERMAL_LIGHT -> "light"
        THERMAL_MODERATE -> "moderate"
        THERMAL_SEVERE -> "severe"
        THERMAL_CRITICAL -> "critical"
        THERMAL_EMERGENCY -> "emergency"
        THERMAL_SHUTDOWN -> "shutdown"
        else -> "unknown($thermalStatus)"
    }
}
