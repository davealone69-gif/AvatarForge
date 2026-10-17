package com.avatarforge.sdk

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException

/** Asset categories are an index, not a claim that every category has a runtime consumer. */
enum class AssetType(val wire: String) {
    CHARACTER_PRESET("character_preset"), MATERIAL("material"), SKELETON("skeleton"),
    ANIMATION("animation"), BLENDSHAPE("blendshape"), PROCEDURAL_PARAMETERS("procedural_parameters"),
    RENDERER_SETTINGS("renderer_settings"), REFERENCE_IMAGE("reference_image");

    companion object {
        fun parse(value: String): AssetType = entries.firstOrNull { it.wire == value }
            ?: throw AssetRegistryException("Unsupported asset type: " + value)
    }
}

data class AssetRecord(
    val id: String, val type: AssetType, val path: String, val version: Int,
    val dependencies: List<String>, val license: String?, val source: String?
)

class AssetRegistry private constructor(private val root: File, private val records: Map<String, AssetRecord>) {
    val assets: List<AssetRecord> get() = records.values.toList()
    fun find(id: String): AssetRecord? = records[id]

    /** This category is connected to the real generator through AvatarSpec. Other categories are indexed only. */
    fun loadCharacterPreset(id: String): AvatarSpec {
        val record = records[id] ?: throw AssetRegistryException("Unknown asset id: " + id)
        if (record.type != AssetType.CHARACTER_PRESET) {
            throw AssetRegistryException("Asset " + id + " is " + record.type.wire + ", not character_preset")
        }
        val file = safeFile(record.path)
        val obj = try { JSONObject(readBounded(file, MAX_FILE_BYTES).toString(Charsets.UTF_8)) }
        catch (e: Exception) { throw AssetRegistryException("Cannot parse preset " + id + ": " + e.message, e) }
        rejectKeys(obj, PRESET_KEYS, "preset " + id)
        if (exactInt(obj, "schemaVersion", 1, 1) != 1 || obj.optString("id") != id) {
            throw AssetRegistryException("Preset schemaVersion must be 1 and id must match manifest id")
        }
        fun number(key: String, fallback: Float, min: Float, max: Float): Float {
            if (!obj.has(key)) return fallback
            val v = obj.opt(key)
            if (v !is Number || !v.toFloat().isFinite() || v.toFloat() < min || v.toFloat() > max) {
                throw AssetRegistryException("Preset " + id + ": " + key + " must be a finite number in " + min + ".." + max)
            }
            return v.toFloat()
        }
        fun triplet(key: String, fallback: FloatArray): FloatArray {
            if (!obj.has(key)) return fallback
            val a = obj.optJSONArray(key) ?: throw AssetRegistryException(key + " must be a 3-number array")
            if (a.length() != 3) throw AssetRegistryException(key + " must have exactly 3 values")
            return FloatArray(3) { i ->
                val v = a.opt(i)
                if (v !is Number || !v.toFloat().isFinite() || v.toFloat() !in 0f..1f) {
                    throw AssetRegistryException(key + "[" + i + "] must be in 0..1")
                }
                v.toFloat()
            }
        }
        fun <T> enumValue(key: String, values: Array<T>, name: (T) -> String, fallback: T): T {
            if (!obj.has(key)) return fallback
            val raw = obj.opt(key)
            if (raw !is String) throw AssetRegistryException(key + " must be a string enum")
            return values.firstOrNull { name(it) == raw.uppercase() }
                ?: throw AssetRegistryException("Unsupported " + key + ": " + raw)
        }
        val defaults = AvatarSpec()
        return try {
            AvatarSpec(
                sex = enumValue("sex", Sex.entries.toTypedArray(), { it.name }, defaults.sex),
                build = enumValue("build", Build.entries.toTypedArray(), { it.name }, defaults.build),
                hairStyle = enumValue("hairStyle", HairStyle.entries.toTypedArray(), { it.name }, defaults.hairStyle),
                outfit = enumValue("outfit", Outfit.entries.toTypedArray(), { it.name }, defaults.outfit),
                footwear = enumValue("footwear", Footwear.entries.toTypedArray(), { it.name }, defaults.footwear),
                seed = if (obj.has("seed")) exactInt(obj, "seed", Int.MIN_VALUE, Int.MAX_VALUE) else defaults.seed,
                heightM = number("heightM", defaults.heightM, 0f, 2.2f).also { if (it != 0f && it < 1.4f) throw AssetRegistryException("heightM must be 0 or 1.4..2.2") },
                ageYears = number("ageYears", defaults.ageYears, 16f, 70f),
                muscle = number("muscle", defaults.muscle, 0f, 1f),
                bodyFat = number("bodyFat", defaults.bodyFat, 0f, 1f),
                bust = number("bust", defaults.bust, 0f, 1f),
                neckCm = number("neckCm", 0f, 0f, 100f), chestCm = number("chestCm", 0f, 0f, 250f),
                underbustCm = number("underbustCm", 0f, 0f, 250f), waistCm = number("waistCm", 0f, 0f, 250f),
                hipCm = number("hipCm", 0f, 0f, 300f), thighCm = number("thighCm", 0f, 0f, 150f),
                calfCm = number("calfCm", 0f, 0f, 100f), bicepCm = number("bicepCm", 0f, 0f, 100f),
                forearmCm = number("forearmCm", 0f, 0f, 100f), wristCm = number("wristCm", 0f, 0f, 60f),
                shoulderWidthCm = number("shoulderWidthCm", 0f, 0f, 120f), inseamCm = number("inseamCm", 0f, 0f, 150f),
                headCircumferenceCm = number("headCircumferenceCm", 0f, 0f, 100f),
                skinTone = triplet("skinTone", defaults.skinTone), skinMelanin = number("skinMelanin", defaults.skinMelanin, 0f, 1f),
                hairTone = triplet("hairTone", defaults.hairTone), clothTone = triplet("clothTone", defaults.clothTone),
                gearTone = triplet("gearTone", defaults.gearTone),
                clothRoughness = number("clothRoughness", -1f, -1f, 1f),
                gearMetalness = number("gearMetalness", -1f, -1f, 1f),
                prompt = obj.optString("prompt", "")
            )
        } catch (e: IllegalArgumentException) {
            throw AssetRegistryException("Invalid character preset " + id + ": " + e.message, e)
        }
    }

    private fun safeFile(path: String): File {
        val base = File(root, "assets").canonicalFile
        val candidate = File(base, path).canonicalFile
        if (!candidate.path.startsWith(base.path + File.separator)) {
            throw AssetRegistryException("Asset path escapes assets directory: " + path)
        }
        if (!candidate.isFile) throw AssetRegistryException("Missing asset file: " + path)
        return candidate
    }

    companion object {
        private const val MAX_MANIFEST_BYTES = 2 * 1024 * 1024
        private const val MAX_FILE_BYTES = 2 * 1024 * 1024
        private val ID = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
        private val PATH = Regex("[A-Za-z0-9._/-]{1,240}")
        private val MANIFEST_KEYS = setOf("schemaVersion", "assets")
        private val RECORD_KEYS = setOf("id", "type", "path", "version", "dependencies", "license", "source")
        private val PRESET_KEYS = setOf(
            "schemaVersion", "id", "sex", "build", "hairStyle", "outfit", "footwear", "seed",
            "heightM", "ageYears", "muscle", "bodyFat", "bust", "neckCm", "chestCm", "underbustCm",
            "waistCm", "hipCm", "thighCm", "calfCm", "bicepCm", "forearmCm", "wristCm",
            "shoulderWidthCm", "inseamCm", "headCircumferenceCm", "skinTone", "skinMelanin",
            "hairTone", "clothTone", "gearTone", "clothRoughness", "gearMetalness", "prompt"
        )

        /** Reads filesDir/avatarforge/manifest.json; data assets live under filesDir/avatarforge/assets/. */
        fun open(context: Context): AssetRegistry {
            val root = File(context.filesDir, "avatarforge")
            val manifest = File(root, "manifest.json")
            val obj = try { JSONObject(readBounded(manifest, MAX_MANIFEST_BYTES).toString(Charsets.UTF_8)) }
            catch (e: Exception) { throw AssetRegistryException("Cannot read asset manifest: " + e.message, e) }
            rejectKeys(obj, MANIFEST_KEYS, "asset manifest")
            if (exactInt(obj, "schemaVersion", 1, 1) != 1) throw AssetRegistryException("Manifest schemaVersion must be 1")
            val array = obj.optJSONArray("assets") ?: throw AssetRegistryException("Manifest assets must be an array")
            if (array.length() > 1000) throw AssetRegistryException("Manifest has more than 1000 assets")
            val map = linkedMapOf<String, AssetRecord>()
            for (i in 0 until array.length()) {
                val item = array.optJSONObject(i) ?: throw AssetRegistryException("assets[" + i + "] must be an object")
                rejectKeys(item, RECORD_KEYS, "assets[" + i + "]")
                val id = requiredString(item, "id")
                if (!ID.matches(id)) throw AssetRegistryException("Invalid asset id: " + id)
                if (map.containsKey(id)) throw AssetRegistryException("Duplicate asset id: " + id)
                val path = requiredString(item, "path")
                if (!PATH.matches(path) || path.startsWith("/") || path.split('/').any { it == ".." || it.isEmpty() }) {
                    throw AssetRegistryException("Unsafe asset path: " + path)
                }
                val version = exactInt(item, "version", 1, Int.MAX_VALUE)
                val depArray = item.optJSONArray("dependencies") ?: JSONArray()
                val deps = (0 until depArray.length()).map { n ->
                    val dep = depArray.opt(n)
                    if (dep !is String || !ID.matches(dep)) throw AssetRegistryException("Invalid dependency on asset " + id)
                    dep
                }
                if (deps.distinct().size != deps.size) throw AssetRegistryException("Asset " + id + " has duplicate dependencies")
                map[id] = AssetRecord(id, AssetType.parse(requiredString(item, "type")), path, version, deps,
                    item.optString("license").takeIf { it.isNotBlank() },
                    item.optString("source").takeIf { it.isNotBlank() })
            }
            map.values.forEach { record ->
                record.dependencies.forEach { dep ->
                    if (dep !in map) throw AssetRegistryException("Asset " + record.id + " depends on missing asset " + dep)
                }
            }
            val visiting = mutableSetOf<String>()
            val visited = mutableSetOf<String>()
            fun visit(id: String) {
                if (id in visiting) throw AssetRegistryException("Dependency cycle includes asset " + id)
                if (id in visited) return
                visiting += id
                map.getValue(id).dependencies.forEach(::visit)
                visiting -= id
                visited += id
            }
            map.keys.forEach(::visit)
            return AssetRegistry(root, map)
        }

        private fun exactInt(obj: JSONObject, key: String, min: Int, max: Int): Int {
            val value = obj.opt(key)
            if (value !is Number || !value.toDouble().isFinite() || value.toDouble() != value.toInt().toDouble() || value.toDouble() < min.toDouble() || value.toDouble() > max.toDouble()) {
                throw AssetRegistryException("\u0027" + key + "\u0027 must be an integer in " + min + ".." + max)
            }
            return value.toInt()
        }
        private fun requiredString(obj: JSONObject, key: String): String {
            val value = obj.opt(key)
            if (value !is String || value.isBlank()) throw AssetRegistryException("'" + key + "' must be a non-empty string")
            return value
        }
        private fun rejectKeys(obj: JSONObject, allowed: Set<String>, where: String) {
            val keys = obj.keys()
            while (keys.hasNext()) {
                val key = keys.next()
                if (key !in allowed) throw AssetRegistryException(where + " has unsupported field '" + key + "'")
            }
        }
        private fun readBounded(file: File, max: Int): ByteArray {
            if (!file.isFile) throw AssetRegistryException("File not found: " + file.absolutePath)
            if (file.length() !in 1..max.toLong()) throw AssetRegistryException("File size outside 1.." + max + " bytes")
            try {
                file.inputStream().use { input ->
                    val out = java.io.ByteArrayOutputStream()
                    val buffer = ByteArray(8192)
                    var total = 0
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        total += n
                        if (total > max) throw AssetRegistryException("File exceeds " + max + " bytes")
                        out.write(buffer, 0, n)
                    }
                    return out.toByteArray()
                }
            } catch (e: IOException) {
                throw AssetRegistryException("Cannot read " + file.name + ": " + e.message, e)
            }
        }
    }
}

class AssetRegistryException(message: String, cause: Throwable? = null) : Exception(message, cause)


/** Load one installed JSON preset and hand its typed spec to the existing native generation pipeline. */
fun AvatarEngine.generateFromPreset(
    context: Context,
    assetId: String,
    memoryBudgetBytes: Long = 1536L * 1024 * 1024
): JobHandle {
    val preset = AssetRegistry.open(context).loadCharacterPreset(assetId)
    return generateFromText(preset.prompt, preset, memoryBudgetBytes)
}
