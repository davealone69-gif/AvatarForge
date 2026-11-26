package com.avatarforge.sdk

import android.os.PowerManager
import com.avatarforge.sdk.render.Quality
import com.avatarforge.sdk.render.RenderBackend
import com.avatarforge.sdk.render.RenderSettings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ResourceProfilePolicyTest {
    private fun snapshot(
        state: DeviceResourceState = DeviceResourceState.NORMAL,
        lowMemory: Boolean = false,
        thermalStatus: Int? = null,
    ) = DeviceResourceSnapshot(
        state = state,
        availableRamBytes = 0L,
        totalRamBytes = 0L,
        availableStorageBytes = 0L,
        lowMemory = lowMemory,
        cpuCores = 0,
        thermalStatus = thermalStatus,
    )

    @Test fun safeIsConservativeAndDoesNotChangeBackendOrClaimTextureDownscaling() {
        val base = RenderSettings(backend = RenderBackend.VULKAN)
        val applied = ResourceProfilePolicy.apply(ResourceProfile.SAFE, base, snapshot())

        assertEquals(ResourceProfile.SAFE, applied.profile)
        assertEquals(Quality.LOW, applied.settings.quality)
        assertFalse(applied.settings.shadows)
        assertFalse(applied.settings.bloom)
        assertFalse(applied.settings.sss)
        assertEquals(RenderBackend.VULKAN, applied.settings.backend)
        assertTrue(applied.message.contains("does not reduce"))
    }

    @Test fun balancedAndHighUseExistingQualityControls() {
        val base = RenderSettings()
        val balanced = ResourceProfilePolicy.apply(ResourceProfile.BALANCED, base, snapshot()).settings
        val high = ResourceProfilePolicy.apply(ResourceProfile.HIGH, base, snapshot()).settings

        assertEquals(Quality.MEDIUM, balanced.quality)
        assertTrue(balanced.shadows)
        assertFalse(balanced.bloom)
        assertEquals(Quality.HIGH, high.quality)
        assertTrue(high.shadows)
        assertTrue(high.bloom)
        assertTrue(high.shadowMapSize > balanced.shadowMapSize)
    }

    @Test fun cinematicUsesAdaptiveRendererOnlyWhenResourceStateAllowsIt() {
        val applied = ResourceProfilePolicy.apply(ResourceProfile.CINEMATIC, RenderSettings(), snapshot())
        assertEquals(ResourceProfile.CINEMATIC, applied.profile)
        assertEquals(Quality.AUTO, applied.settings.quality)
        assertTrue(applied.settings.shadows)
        assertTrue(applied.settings.bloom)

        val moderateThermal = ResourceProfilePolicy.apply(
            ResourceProfile.CINEMATIC,
            RenderSettings(),
            snapshot(thermalStatus = PowerManager.THERMAL_STATUS_MODERATE),
        )
        assertEquals(ResourceProfile.BALANCED, moderateThermal.profile)
        assertEquals(Quality.MEDIUM, moderateThermal.settings.quality)
    }

    @Test fun cinematicDowngradesOnReportedLowMemoryOrSevereThermalState() {
        val lowMemory = ResourceProfilePolicy.apply(
            ResourceProfile.CINEMATIC,
            RenderSettings(),
            snapshot(state = DeviceResourceState.LOW_MEMORY, lowMemory = true),
        )
        assertEquals(ResourceProfile.SAFE, lowMemory.profile)
        assertEquals(Quality.LOW, lowMemory.settings.quality)

        val severeThermal = ResourceProfilePolicy.apply(
            ResourceProfile.CINEMATIC,
            RenderSettings(),
            snapshot(state = DeviceResourceState.THERMAL_LIMIT),
        )
        assertEquals(ResourceProfile.SAFE, severeThermal.profile)
    }

    @Test fun lowStorageDoesNotPretendToLimitRenderingQuality() {
        val applied = ResourceProfilePolicy.apply(
            ResourceProfile.CINEMATIC,
            RenderSettings(),
            snapshot(state = DeviceResourceState.LOW_STORAGE),
        )
        assertEquals(ResourceProfile.CINEMATIC, applied.profile)
    }
}
