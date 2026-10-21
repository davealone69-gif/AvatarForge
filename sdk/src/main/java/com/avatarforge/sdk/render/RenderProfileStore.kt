package com.avatarforge.sdk.render

import android.content.Context

/**
 * Small, versioned, offline render profile. Stores user intent, not transient adaptive quality:
 * AUTO is re-evaluated from live frame timings and thermal state on every renderer session.
 */
object RenderProfileStore {
    private const val PREFS = "avatarforge_render_profile_v1"

    fun load(context: Context, fallback: RenderSettings = RenderSettings()): RenderSettings {
        val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!p.contains("profile_version")) return fallback
        val quality = Quality.of(p.getInt("quality", fallback.quality.value))
        return RenderSettings(
            quality = quality,
            preferVulkan = p.getBoolean("preferVulkan", fallback.preferVulkan),
            shadows = p.getBoolean("shadows", fallback.shadows),
            bloom = p.getBoolean("bloom", fallback.bloom),
            sss = p.getBoolean("sss", fallback.sss),
            groundPlane = p.getBoolean("groundPlane", fallback.groundPlane),
            exposure = p.getFloat("exposure", fallback.exposure).finiteIn(0.3f, 2.5f, fallback.exposure),
            sunAzimuthDeg = p.getFloat("sunAzimuthDeg", fallback.sunAzimuthDeg).finiteIn(0f, 360f, fallback.sunAzimuthDeg),
            sunElevationDeg = p.getFloat("sunElevationDeg", fallback.sunElevationDeg).finiteIn(2f, 85f, fallback.sunElevationDeg),
            sunIntensity = p.getFloat("sunIntensity", fallback.sunIntensity).finiteIn(0.5f, 8f, fallback.sunIntensity),
            turbidity = p.getFloat("turbidity", fallback.turbidity).finiteIn(1.8f, 6f, fallback.turbidity),
            environmentIntensity = p.getFloat("environmentIntensity", fallback.environmentIntensity).finiteIn(0f, 2f, fallback.environmentIntensity),
            shadowSoftness = p.getFloat("shadowSoftness", fallback.shadowSoftness).finiteIn(0.5f, 3f, fallback.shadowSoftness),
            msaa = p.getInt("msaa", fallback.msaa).coerceIn(1, 8).let { if (it >= 4) 4 else if (it >= 2) 2 else 1 },
            shadowMapSize = p.getInt("shadowMapSize", fallback.shadowMapSize).let { if (it >= 4096) 4096 else if (it >= 2048) 2048 else 1024 },
            bloomLevels = p.getInt("bloomLevels", fallback.bloomLevels).coerceIn(0, 5),
        )
    }

    fun save(context: Context, settings: RenderSettings) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putInt("profile_version", 1)
            .putInt("quality", settings.quality.value)
            .putBoolean("preferVulkan", settings.preferVulkan)
            .putBoolean("shadows", settings.shadows)
            .putBoolean("bloom", settings.bloom)
            .putBoolean("sss", settings.sss)
            .putBoolean("groundPlane", settings.groundPlane)
            .putFloat("exposure", settings.exposure.finiteIn(0.3f, 2.5f, 0.65f))
            .putFloat("sunAzimuthDeg", settings.sunAzimuthDeg.finiteIn(0f, 360f, 42f))
            .putFloat("sunElevationDeg", settings.sunElevationDeg.finiteIn(2f, 85f, 38f))
            .putFloat("sunIntensity", settings.sunIntensity.finiteIn(0.5f, 8f, 1.5f))
            .putFloat("turbidity", settings.turbidity.finiteIn(1.8f, 6f, 2.6f))
            .putFloat("environmentIntensity", settings.environmentIntensity.finiteIn(0f, 2f, 0.65f))
            .putFloat("shadowSoftness", settings.shadowSoftness.finiteIn(0.5f, 3f, 1f))
            .putInt("msaa", settings.msaa.coerceIn(1, 8))
            .putInt("shadowMapSize", settings.shadowMapSize.coerceIn(1024, 4096))
            .putInt("bloomLevels", settings.bloomLevels.coerceIn(0, 5))
            .apply()
    }

    private fun Float.finiteIn(min: Float, max: Float, fallback: Float): Float =
        if (isFinite()) coerceIn(min, max) else fallback
}
