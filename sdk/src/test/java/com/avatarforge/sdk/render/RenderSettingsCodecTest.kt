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

    @Test fun outOfRangeValuesAreClampedToWhatTheRendererHonours() {
        val decoded = RenderSettingsCodec.decode(mapOf(
            "shadowMapSize" to "65536", "msaa" to "64", "bloomLevels" to "99",
            "exposure" to "40", "sunElevationDeg" to "-90", "turbidity" to "0",
            "shadowSoftness" to "1e9", "sunIntensity" to "-3", "environmentIntensity" to "1000",
        ))
        assertEquals(2048, decoded.shadowMapSize)
        assertEquals(4, decoded.msaa)
        assertEquals(5, decoded.bloomLevels)
        assertEquals(8f, decoded.exposure, 0f)
        assertEquals(-5f, decoded.sunElevationDeg, 0f)
        assertEquals(0.5f, decoded.turbidity, 0f)
        assertEquals(8f, decoded.shadowSoftness, 0f)
        assertEquals(0f, decoded.sunIntensity, 0f)
        assertEquals(4f, decoded.environmentIntensity, 0f)
    }

    @Test fun aStaleVulkanPreferenceIsIgnoredRatherThanReintroducingABackend() {
        val decoded = RenderSettingsCodec.decode(mapOf("preferVulkan" to "true", "backend" to "vulkan"))
        assertEquals(RenderSettings(), decoded)
    }

    @Test fun missingKeysKeepDefaultsAndUnknownKeysAreIgnored() {
        val defaults = RenderSettings(quality = Quality.LOW)
        val decoded = RenderSettingsCodec.decode(mapOf("futureKey" to "1"), defaults)
        assertEquals(defaults, decoded)
    }
}
