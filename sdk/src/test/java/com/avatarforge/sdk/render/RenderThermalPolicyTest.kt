package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Test

class RenderThermalPolicyTest {
    @Test fun severeCapsEveryPresetToLowAndHalvesPacing() {
        for (quality in Quality.entries) {
            assertEquals(Quality.LOW, RenderThermalPolicy.effectiveQuality(quality, RenderThermalPolicy.THERMAL_SEVERE))
        }
        assertEquals(33_333_333L, RenderThermalPolicy.frameIntervalNs(RenderThermalPolicy.THERMAL_SEVERE))
    }

    @Test fun moderateStepsDownWithoutChangingLow() {
        assertEquals(Quality.MEDIUM, RenderThermalPolicy.effectiveQuality(Quality.HIGH, RenderThermalPolicy.THERMAL_MODERATE))
        assertEquals(Quality.MEDIUM, RenderThermalPolicy.effectiveQuality(Quality.AUTO, RenderThermalPolicy.THERMAL_MODERATE))
        assertEquals(Quality.LOW, RenderThermalPolicy.effectiveQuality(Quality.MEDIUM, RenderThermalPolicy.THERMAL_MODERATE))
        assertEquals(Quality.LOW, RenderThermalPolicy.effectiveQuality(Quality.LOW, RenderThermalPolicy.THERMAL_MODERATE))
    }

    @Test fun unknownAndNoneDoNotInventACap() {
        val settings = RenderSettings(quality = Quality.HIGH)
        assertEquals(settings, RenderThermalPolicy.cap(settings, null))
        assertEquals(settings, RenderThermalPolicy.cap(settings, RenderThermalPolicy.THERMAL_NONE))
        assertEquals("unknown", RenderThermalPolicy.describe(null))
    }

    @Test fun capPreservesAvatarAndCameraRelevantSettings() {
        val settings = RenderSettings(quality = Quality.HIGH, exposure = 0.7f, sunIntensity = 1.2f)
        val capped = RenderThermalPolicy.cap(settings, RenderThermalPolicy.THERMAL_CRITICAL)
        assertEquals(Quality.LOW, capped.quality)
        assertEquals(settings.exposure, capped.exposure)
        assertEquals(settings.sunIntensity, capped.sunIntensity)
    }
}
