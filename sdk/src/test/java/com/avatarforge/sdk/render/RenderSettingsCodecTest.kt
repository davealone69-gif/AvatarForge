package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Test

class RenderSettingsCodecTest {
    @Test fun roundTripPreservesEveryField() {
        val settings = RenderSettings(
            quality = Quality.HIGH, shadows = false, bloom = true,
            sss = false, groundPlane = false, exposure = 0.7f, sunAzimuthDeg = 10f,
            sunElevationDeg = 20f, sunIntensity = 1.1f, turbidity = 2.2f,
            environmentIntensity = 0.3f, shadowSoftness = 0.4f, msaa = 2,
            shadowMapSize = 1024, bloomLevels = 1,
        )
        assertEquals(settings, RenderSettingsCodec.decode(RenderSettingsCodec.encode(settings)))
    }

    @Test fun malformedValuesFallBackToDefaults() {
        val defaults = RenderSettings()
        val decoded = RenderSettingsCodec.decode(mapOf(
            "quality" to "999", "exposure" to "NaN", "msaa" to "abc", "preferVulkan" to "maybe",
        ))
        assertEquals(defaults.quality, decoded.quality)
        assertEquals(defaults.exposure, decoded.exposure)
        assertEquals(defaults.msaa, decoded.msaa)
        assertEquals(Quality.AUTO, decoded.quality)
    }

    @Test fun missingKeysKeepDefaultsAndUnknownKeysAreIgnored() {
        val defaults = RenderSettings(quality = Quality.LOW)
        val decoded = RenderSettingsCodec.decode(mapOf("futureKey" to "1"), defaults)
        assertEquals(defaults, decoded)
    }
}
