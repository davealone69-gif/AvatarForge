package com.avatarforge.sdk.render

import android.content.Context
import android.content.SharedPreferences

/** Versioned, tolerant codec between RenderSettings and a flat string map. */
object RenderSettingsCodec {
    private const val KEY_VERSION = "version"
    private const val FORMAT_VERSION = 1

    fun encode(settings: RenderSettings): Map<String, String> = buildMap {
        put(KEY_VERSION, FORMAT_VERSION.toString())
        put("quality", settings.quality.value.toString())
        put("shadows", settings.shadows.toString())
        put("bloom", settings.bloom.toString())
        put("sss", settings.sss.toString())
        put("groundPlane", settings.groundPlane.toString())
        put("exposure", settings.exposure.toString())
        put("sunAzimuthDeg", settings.sunAzimuthDeg.toString())
        put("sunElevationDeg", settings.sunElevationDeg.toString())
        put("sunIntensity", settings.sunIntensity.toString())
        put("turbidity", settings.turbidity.toString())
        put("environmentIntensity", settings.environmentIntensity.toString())
        put("shadowSoftness", settings.shadowSoftness.toString())
        put("msaa", settings.msaa.toString())
        put("shadowMapSize", settings.shadowMapSize.toString())
        put("bloomLevels", settings.bloomLevels.toString())
    }

    fun decode(values: Map<String, String>, defaults: RenderSettings = RenderSettings()): RenderSettings {
        fun bool(key: String, current: Boolean) = values[key]?.toBooleanStrictOrNull() ?: current
        fun float(key: String, current: Float) = values[key]?.toFloatOrNull()?.takeIf { it.isFinite() } ?: current
        fun int(key: String, current: Int) = values[key]?.toIntOrNull() ?: current
        return RenderSettings(
            quality = Quality.of(int("quality", defaults.quality.value)),
            shadows = bool("shadows", defaults.shadows),
            bloom = bool("bloom", defaults.bloom),
            sss = bool("sss", defaults.sss),
            groundPlane = bool("groundPlane", defaults.groundPlane),
            exposure = float("exposure", defaults.exposure),
            sunAzimuthDeg = float("sunAzimuthDeg", defaults.sunAzimuthDeg),
            sunElevationDeg = float("sunElevationDeg", defaults.sunElevationDeg),
            sunIntensity = float("sunIntensity", defaults.sunIntensity),
            turbidity = float("turbidity", defaults.turbidity),
            environmentIntensity = float("environmentIntensity", defaults.environmentIntensity),
            shadowSoftness = float("shadowSoftness", defaults.shadowSoftness),
            msaa = int("msaa", defaults.msaa),
            shadowMapSize = int("shadowMapSize", defaults.shadowMapSize),
            bloomLevels = int("bloomLevels", defaults.bloomLevels),
        )
    }
}

/** SharedPreferences-backed persistence for renderer settings only. Prompts/photos are not persisted. */
class SettingsStore(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun loadRenderSettings(): RenderSettings? {
        if (!prefs.contains(KEY_QUALITY)) return null
        val values = prefs.all.mapValues { it.value?.toString() ?: "" }
        return RenderSettingsCodec.decode(values)
    }

    fun saveRenderSettings(settings: RenderSettings) {
        val encoded = RenderSettingsCodec.encode(settings)
        prefs.edit().apply {
            encoded.forEach { (k, v) -> putString(k, v) }
            apply()
        }
    }

    private companion object {
        const val PREFS_NAME = "avatarforge_settings"
        const val KEY_QUALITY = "quality"
    }
}
