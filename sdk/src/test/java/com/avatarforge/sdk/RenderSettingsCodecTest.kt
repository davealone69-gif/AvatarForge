package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Locks finding AF-004: persisted renderer settings must round-trip exactly, and a corrupted or
 * partially-written preference map must degrade to defaults instead of crashing the app launch.
 */
class RenderSettingsCodecTest {

    private val nonDefault = RenderSettings(
        quality = Quality.LOW,
        preferVulkan = false,
        shadows = false,
        bloom = true,
        sss = false,
        groundPlane = false,
        exposure = 1.25f,
        sunAzimuthDeg = 200f,
        sunElevationDeg = 55f,
        sunIntensity = 2.5f,
        turbidity = 4.0f,
        environmentIntensity = 0.9f,
        shadowSoftness = 1.75f,
        msaa = 2,
        shadowMapSize = 1024,
        bloomLevels = 3,
    )

    @Test
    fun `encode then decode reproduces every field`() {
        val decoded = RenderSettingsCodec.decode(RenderSettingsCodec.encode(nonDefault))
        assertEquals(nonDefault, decoded)
    }

    @Test
    fun `empty map decodes to the defaults`() {
        assertEquals(RenderSettings(), RenderSettingsCodec.decode(emptyMap()))
    }

    @Test
    fun `malformed values keep their defaults and do not throw`() {
        val decoded = RenderSettingsCodec.decode(
            mapOf(
                "msaa" to "banana",
                "preferVulkan" to "maybe",
                "exposure" to "not-a-float",
                "quality" to "99",
                "shadowMapSize" to "-50",
            ),
        )
        assertEquals(RenderSettings().msaa, decoded.msaa)
        assertEquals(RenderSettings().preferVulkan, decoded.preferVulkan)
        assertEquals(RenderSettings().exposure, decoded.exposure, 0f)
        // Unknown quality integers are coerced to AUTO, never to an out-of-range enum.
        assertEquals(Quality.AUTO, decoded.quality)
        assertEquals(-50, decoded.shadowMapSize)
    }

    @Test
    fun `unknown keys from a newer version are ignored`() {
        val encoded = RenderSettingsCodec.encode(nonDefault) + ("future_setting" to "x")
        assertEquals(nonDefault, RenderSettingsCodec.decode(encoded))
    }

    @Test
    fun `partial map fills missing keys from the supplied defaults`() {
        val decoded = RenderSettingsCodec.decode(mapOf("bloom" to "true"), RenderSettings())
        assertTrue(decoded.bloom)
        // Untouched fields remain default.
        assertEquals(RenderSettings().shadows, decoded.shadows)
        assertEquals(RenderSettings().quality, decoded.quality)
    }

    @Test
    fun `every quality enum survives the integer encoding`() {
        Quality.entries.forEach { quality ->
            val encoded = RenderSettingsCodec.encode(RenderSettings(quality = quality))
            assertEquals(quality, RenderSettingsCodec.decode(encoded).quality)
        }
    }
}
