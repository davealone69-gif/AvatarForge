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

    /*
     * Every numeric field is clamped to the range the renderer will actually honour (af_gl.cpp
     * sanitiseOptions enforces the same bounds). Stored preferences are input, not a promise: an
     * old build's file, a hand-edited value or a corrupted write must not be able to ask for a
     * 65,536 x 65,536 shadow map, and the settings screen must not display a value the renderer has
     * already silently reduced.
     */
    fun decode(values: Map<String, String>, defaults: RenderSettings = RenderSettings()): RenderSettings {
        fun bool(key: String, current: Boolean) = values[key]?.toBooleanStrictOrNull() ?: current
        fun float(key: String, current: Float, lo: Float, hi: Float) =
            values[key]?.toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(lo, hi) ?: current
        fun int(key: String, current: Int, lo: Int, hi: Int) =
            (values[key]?.toIntOrNull() ?: current).coerceIn(lo, hi)
        /* Quality is matched exactly: an out-of-range stored id means "unknown", which must fall
           back to the caller's default rather than clamp onto the highest preset. */
        val quality = Quality.entries.firstOrNull { it.value == values["quality"]?.toIntOrNull() }
            ?: defaults.quality
        return RenderSettings(
            quality = quality,
            shadows = bool("shadows", defaults.shadows),
            bloom = bool("bloom", defaults.bloom),
            sss = bool("sss", defaults.sss),
            groundPlane = bool("groundPlane", defaults.groundPlane),
            exposure = float("exposure", defaults.exposure, 0.05f, 8f),
            sunAzimuthDeg = float("sunAzimuthDeg", defaults.sunAzimuthDeg, 0f, 360f),
            sunElevationDeg = float("sunElevationDeg", defaults.sunElevationDeg, -5f, 90f),
            sunIntensity = float("sunIntensity", defaults.sunIntensity, 0f, 16f),
            turbidity = float("turbidity", defaults.turbidity, 0.5f, 20f),
            environmentIntensity = float("environmentIntensity", defaults.environmentIntensity, 0f, 4f),
            shadowSoftness = float("shadowSoftness", defaults.shadowSoftness, 0.25f, 8f),
            msaa = int("msaa", defaults.msaa, 1, 4),
            shadowMapSize = int("shadowMapSize", defaults.shadowMapSize, 512, 2048),
            bloomLevels = int("bloomLevels", defaults.bloomLevels, 0, 5),
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
