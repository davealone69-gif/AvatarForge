package com.avatarforge.sdk

import android.content.Context
import org.json.JSONObject

/** Runtime types accepted by an offline, data-driven customization contract. */
enum class CustomInputType { FLOAT, INT, BOOL, ENUM, COLOR }

/** Honest capability labels. Only [VERIFIED] definitions may become production controls. */
enum class CustomInputStatus {
    VERIFIED,
    UNVERIFIED,
    PARTIAL,
    BLOCKED_ENGINE_GEOMETRY,
    BLOCKED_TEXTURE,
    BLOCKED_RIG,
    BLOCKED_DEVICE_TEST;

    companion object {
        internal fun parse(value: String?): CustomInputStatus =
            entries.firstOrNull { it.name == value } ?: UNVERIFIED
    }
}

data class CustomInputDefinition(
    val key: String,
    val group: String,
    val label: String,
    val type: CustomInputType,
    val unit: String?,
    val minimum: Double?,
    val maximum: Double?,
    val step: Double?,
    /** Canonical JSON scalar text; interpret according to [type]. */
    val defaultValue: String,
    val options: List<String>,
    val optionLabels: Map<String, String>,
    val requiresRebuild: Boolean,
    val status: CustomInputStatus,
    val engineTarget: String?,
    val blockedReason: String?,
) {
    /** NO FAKES: schema presence alone never makes a control visible. */
    val productionVisible: Boolean get() = status == CustomInputStatus.VERIFIED
}

data class CustomizationSchema(
    val id: String,
    val title: String,
    val schemaVersion: Int,
    val adultOnly: Boolean,
    val minimumAgeYears: Int?,
    val inputs: List<CustomInputDefinition>,
) {
    private val byKey = inputs.associateBy { it.key }

    init {
        require(byKey.size == inputs.size) { "customization schema '$id' contains duplicate keys" }
        if (adultOnly) require((minimumAgeYears ?: 0) >= 18) {
            "adult-only customization schema '$id' requires minimumAgeYears >= 18"
        }
    }

    fun input(key: String): CustomInputDefinition? = byKey[key]

    /** Definitions safe for a production UI after implementation and device verification. */
    fun productionInputs(): List<CustomInputDefinition> = inputs.filter { it.productionVisible }
}

/**
 * Loads normalized contracts bundled in the AAR. It performs no I/O beyond local APK assets and
 * does not turn PARTIAL/BLOCKED vocabulary into controls.
 */
object CustomizationSchemas {
    const val AVATAR_CUSTOMIZATION_ASSET = "customization/avatar-customization.inputs.json"
    const val ADULT_ANATOMY_ASSET = "customization/adult-anatomy.inputs.json"

    fun loadAvatarCustomization(context: Context): CustomizationSchema =
        load(context, AVATAR_CUSTOMIZATION_ASSET)

    fun loadAdultAnatomy(context: Context): CustomizationSchema =
        load(context, ADULT_ANATOMY_ASSET)

    fun load(context: Context, assetPath: String): CustomizationSchema {
        val text = context.assets.open(assetPath).bufferedReader(Charsets.UTF_8).use { it.readText() }
        val root = JSONObject(text)
        val array = root.getJSONArray("inputs")
        val inputs = ArrayList<CustomInputDefinition>(array.length())
        for (index in 0 until array.length()) {
            val item = array.getJSONObject(index)
            val type = CustomInputType.valueOf(item.getString("type").uppercase())
            val options = item.optJSONArray("options")?.let { values ->
                List(values.length()) { optionIndex -> values.getString(optionIndex) }
            } ?: emptyList()
            val labelsObject = item.optJSONObject("option_labels")
            val labels = if (labelsObject == null) {
                options.associateWith { it.replace('_', ' ') }
            } else {
                buildMap {
                    labelsObject.keys().forEach { key -> put(key, labelsObject.getString(key)) }
                }
            }
            val binding = item.optJSONObject("engine_binding")
            val rawDefault = item.get("default")
            inputs += CustomInputDefinition(
                key = item.getString("key"),
                group = item.getString("group"),
                label = item.getString("label"),
                type = type,
                unit = item.optString("unit").takeIf { it.isNotEmpty() },
                minimum = item.optDouble("minimum").takeUnless { it.isNaN() },
                maximum = item.optDouble("maximum").takeUnless { it.isNaN() },
                step = item.optDouble("step").takeUnless { it.isNaN() },
                defaultValue = rawDefault.toString(),
                options = options,
                optionLabels = labels,
                requiresRebuild = item.getBoolean("requires_rebuild"),
                status = CustomInputStatus.parse(item.optString("implementation_status")),
                engineTarget = binding?.optString("target")?.takeIf { it.isNotEmpty() },
                blockedReason = item.optString("blocked_reason").takeIf { it.isNotEmpty() },
            )
        }
        return CustomizationSchema(
            id = root.getString("id"),
            title = root.getString("title"),
            schemaVersion = root.getInt("schema_version"),
            adultOnly = root.optBoolean("adult_only", false),
            minimumAgeYears = if (root.has("minimum_age_years")) root.getInt("minimum_age_years") else null,
            inputs = inputs,
        )
    }
}
