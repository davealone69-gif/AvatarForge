/*
 * AvatarForge :: SettingsStore.kt
 *
 * Persists RenderSettings across process death (finding AF-004). The design splits the Android
 * dependency from the data logic:
 *
 *   - RenderSettingsCodec is pure Kotlin (String <-> typed values, tolerant of missing/garbage
 *     keys and forward-compatible with unknown keys) so a JVM unit test locks its behaviour;
 *   - SettingsStore is the thin SharedPreferences wrapper, the only Android-coupled piece.
 *
 * Only renderer settings are persisted in this iteration. The prompt, reference photos and spec
 * still live in Compose memory and are documented as the remaining AF-004 gap.
 */
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
        put("preferVulkan", settings.preferVulkan.toString())
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

    /**
     * Decodes [values] over [defaults]. Unknown keys are ignored (forward compatibility), missing
     * keys keep the default, malformed booleans/numbers keep the default rather than throwing, and
     * an unknown quality integer maps to AUTO via [Quality.of].
     */
    fun decode(values: Map<String, String>, defaults: RenderSettings = RenderSettings()): RenderSettings {
        fun str(key: String) = values[key]
        fun bool(key: String, current: Boolean) = str(key)?.toBooleanStrictOrNull() ?: current
        fun float(key: String, current: Float) = str(key)?.toFloatOrNull() ?: current
        fun int(key: String, current: Int) = str(key)?.toIntOrNull() ?: current
        return RenderSettings(
            quality = Quality.of(int("quality", defaults.quality.value)),
            preferVulkan = bool("preferVulkan", defaults.preferVulkan),
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

/** SharedPreferences-backed persistence for renderer settings. */
class SettingsStore(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /** Returns the saved settings, or null when none have been persisted yet. */
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
