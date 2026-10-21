package com.avatarforge.sdk.render

import android.os.PowerManager
import org.junit.Assert.assertEquals
import org.junit.Test

class RenderThermalPolicyTest {
    @Test
    fun absentOrLightThermalSignalPreservesTheRequestedPreset() {
        for (requested in Quality.entries) {
            assertEquals(requested, RenderThermalPolicy.effectiveQuality(requested, null))
            assertEquals(requested, RenderThermalPolicy.effectiveQuality(requested, PowerManager.THERMAL_STATUS_LIGHT))
        }
    }

    @Test
    fun moderateThermalPressureStepsDownOneTier() {
        assertEquals(Quality.MEDIUM, RenderThermalPolicy.effectiveQuality(Quality.HIGH, PowerManager.THERMAL_STATUS_MODERATE))
        assertEquals(Quality.LOW, RenderThermalPolicy.effectiveQuality(Quality.MEDIUM, PowerManager.THERMAL_STATUS_MODERATE))
        assertEquals(Quality.MEDIUM, RenderThermalPolicy.effectiveQuality(Quality.AUTO, PowerManager.THERMAL_STATUS_MODERATE))
        assertEquals(Quality.LOW, RenderThermalPolicy.effectiveQuality(Quality.LOW, PowerManager.THERMAL_STATUS_MODERATE))
    }

    @Test
    fun severeThermalPressureUsesLowTierRegardlessOfRequestedPreset() {
        for (requested in Quality.entries) {
            assertEquals(Quality.LOW, RenderThermalPolicy.effectiveQuality(requested, PowerManager.THERMAL_STATUS_SEVERE))
        }
    }
}
