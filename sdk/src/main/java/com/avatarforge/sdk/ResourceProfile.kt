package com.avatarforge.sdk

import android.os.PowerManager
import com.avatarforge.sdk.render.Quality
import com.avatarforge.sdk.render.RenderSettings

/** Qualitative presets, not measured device-performance limits. */
enum class ResourceProfile(val title: String) {
    SAFE("Safe"),
    BALANCED("Balanced"),
    HIGH("High"),
    CINEMATIC("Cinematic"),
}

data class AppliedResourceProfile(
    val profile: ResourceProfile,
    val settings: RenderSettings,
    val message: String,
)

/**
 * Maps qualitative choices onto renderer controls already supported by AvatarForge.
 *
 * No new RAM, storage, FPS, or temperature thresholds are introduced here. Cinematic is allowed
 * unless Android reports low memory or the existing device governor reports a low-memory or severe
 * thermal state. Its AUTO setting lets the native renderer adjust from observed frame timing.
 * The generator's 2048x2048 texture atlas is currently fixed by the native pipeline, so these
 * profiles do not claim to resize generated texture maps.
 */
object ResourceProfilePolicy {
    fun infer(settings: RenderSettings): ResourceProfile = when (settings.quality) {
        Quality.LOW -> ResourceProfile.SAFE
        Quality.MEDIUM -> ResourceProfile.BALANCED
        Quality.HIGH -> ResourceProfile.HIGH
        Quality.AUTO -> ResourceProfile.CINEMATIC
    }

    fun apply(
        requested: ResourceProfile,
        base: RenderSettings,
        resources: DeviceResourceSnapshot,
    ): AppliedResourceProfile {
        val constrained = resources.lowMemory ||
            resources.state == DeviceResourceState.LOW_MEMORY ||
            resources.state == DeviceResourceState.THERMAL_LIMIT
        if (requested == ResourceProfile.CINEMATIC && constrained) {
            val safe = settingsFor(ResourceProfile.SAFE, base)
            return AppliedResourceProfile(
                ResourceProfile.SAFE,
                safe,
                "Cinematic is unavailable under the current OS-reported memory or thermal state; Safe rendering is active. Texture-map resolution is unchanged.",
            )
        }
        val moderateThermal = resources.thermalStatus != null &&
            resources.thermalStatus >= PowerManager.THERMAL_STATUS_MODERATE
        if (requested == ResourceProfile.CINEMATIC && moderateThermal) {
            return AppliedResourceProfile(
                ResourceProfile.BALANCED,
                settingsFor(ResourceProfile.BALANCED, base),
                "Cinematic is temporarily limited by Android's moderate thermal report; Balanced rendering is active. Texture-map resolution is unchanged.",
            )
        }

        val settings = settingsFor(requested, base)
        val message = when (requested) {
            ResourceProfile.SAFE ->
                "One rendered character with conservative renderer effects. The current engine fixes generated texture-map resolution, so this profile does not reduce it."
            ResourceProfile.BALANCED ->
                "Standard character and animation path with balanced renderer settings. Generated texture-map resolution is unchanged."
            ResourceProfile.HIGH ->
                "Higher supported shadow and renderer quality. Generated texture-map resolution is unchanged."
            ResourceProfile.CINEMATIC ->
                "Maximum adaptive renderer profile: quality follows measured frame timing. This is not a device-performance guarantee; generated texture-map resolution is unchanged."
        }
        return AppliedResourceProfile(requested, settings, message)
    }

    private fun settingsFor(profile: ResourceProfile, base: RenderSettings): RenderSettings = when (profile) {
        ResourceProfile.SAFE -> base.copy(
            quality = Quality.LOW,
            shadows = false,
            bloom = false,
            sss = false,
            msaa = 1,
            shadowMapSize = 1024,
            bloomLevels = 2,
        )
        ResourceProfile.BALANCED -> base.copy(
            quality = Quality.MEDIUM,
            shadows = true,
            bloom = false,
            sss = true,
            msaa = 2,
            shadowMapSize = 1024,
            bloomLevels = 3,
        )
        ResourceProfile.HIGH -> base.copy(
            quality = Quality.HIGH,
            shadows = true,
            bloom = true,
            sss = true,
            msaa = 4,
            shadowMapSize = 2048,
            bloomLevels = 4,
        )
        ResourceProfile.CINEMATIC -> base.copy(
            quality = Quality.AUTO,
            shadows = true,
            bloom = true,
            sss = true,
            msaa = 4,
            shadowMapSize = 2048,
            bloomLevels = 5,
        )
    }
}
