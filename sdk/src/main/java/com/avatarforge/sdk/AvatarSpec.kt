/*
 * AvatarForge :: AvatarSpec.kt
 *
 * The avatar's specification: who it is, how it is built, what it wears. This is the *only* input
 * the generator takes besides the prompt text and optional reference photos, and it is what makes
 * a generation reproducible: the same spec + the same seed produces byte-identical geometry
 * (asserted by the native test harness, test [6]).
 *
 * Every field mirrors a member of `af_spec` in native/include/afcore.h, and pack()/unpack() are
 * the authoritative translation - the JNI layer reads exactly this array layout. Fields left at
 * their defaults mean "derive this from the height and build", which is what the anthropometry
 * solver does best.
 */
package com.avatarforge.sdk

/** Values match `af_sex`. */
enum class Sex(val value: Int) {
    UNSPECIFIED(0), MALE(1), FEMALE(2);

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: UNSPECIFIED
    }
}

/** Values match `af_build`. */
enum class Build(val value: Int) {
    AVERAGE(0), SLIM(1), ATHLETIC(2), MUSCULAR(3), HEAVY(4);

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: AVERAGE
    }
}

/** Values match `af_hair_style`. */
enum class HairStyle(val value: Int) {
    NONE(0), BUZZ(1), SHORT(2), MEDIUM(3), LONG(4), PONYTAIL(5), BUN(6), BRAIDS(7), AFRO(8), MOHAWK(9);

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: MEDIUM
    }
}

/** Values match `af_outfit`. */
enum class Outfit(val value: Int) {
    NONE(0), TSHIRT(1), DRESS(2), SUIT(3), JEANS_JACKET(4), ARMOR_PLATE(5),
    ARMOR_LEATHER(6), SPACESUIT(7), ATHLETIC(8), COAT(9);

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: NONE
    }
}

/** Values match `af_footwear`. */
enum class Footwear(val value: Int) {
    NONE(0), SNEAKER(1), BOOT(2), SABATON(3), HEEL(4);

    companion object {
        fun of(value: Int) = entries.firstOrNull { it.value == value } ?: NONE
    }
}

/** Which reference image is which, matching `af_image_role`. */
enum class ImageRole(val value: Int) {
    /**
     * ABI-stable name for Image A. The current analyser samples coarse framing, proportions and
     * colours only; it does not reconstruct or preserve facial identity.
     */
    IDENTITY(0),

    /** Image B: garment colour/material and generic outfit-class hint; no garment transfer. */
    CLOTHING(1),

    /** A reference known to show the whole body, enabling a coarse head-count stature estimate. */
    FULL_BODY(2),

    /** An additional independently analysed view. The current engine does not perform view fusion. */
    EXTRA_VIEW(3),
}

/**
 * A complete avatar description.
 *
 * Only [sex], [build] and [heightM] are usually set by hand; the rest are either measured from a
 * reference photo (see [ImageReference]) or derived. Tape measurements are honoured exactly when
 * supplied: the waist you ask for is the waist you get, to within 0.5% (harness test [1]).
 */
data class AvatarSpec(
    val sex: Sex = Sex.UNSPECIFIED,
    val build: Build = Build.AVERAGE,
    val hairStyle: HairStyle = HairStyle.MEDIUM,
    val outfit: Outfit = Outfit.TSHIRT,
    val footwear: Footwear = Footwear.NONE,
    /** RNG seed. Identical seed => identical avatar, bit for bit. */
    val seed: Int = 1337,

    /** Stature in metres. Always resolved; defaults to [DEFAULT_HEIGHT_M]. */
    val heightM: Float = DEFAULT_HEIGHT_M,
    /** 16 .. 70: drives skin micro-detail, facial volume and hair colour desaturation. */
    val ageYears: Float = 30f,
    /** 0..1 lean mass. */
    val muscle: Float = 0.45f,
    /** 0..1; 0.10 shredded, 0.45 heavy. */
    val bodyFat: Float = 0.22f,
    /** 0..1 feminine chest volume. */
    val bust: Float = 0.45f,

    /* Tape measurements, centimetres. 0 = derive. */
    val neckCm: Float = 0f,
    val chestCm: Float = 0f,
    val underbustCm: Float = 0f,
    val waistCm: Float = 0f,
    val hipCm: Float = 0f,
    val thighCm: Float = 0f,
    val calfCm: Float = 0f,
    val bicepCm: Float = 0f,
    val forearmCm: Float = 0f,
    val wristCm: Float = 0f,
    val shoulderWidthCm: Float = 0f,
    val inseamCm: Float = 0f,
    val headCircumferenceCm: Float = 0f,

    /** Linear sRGB skin reflectance; all zero = derived from [skinMelanin]. */
    val skinTone: FloatArray = FloatArray(3),
    /** 0..1 melanin content - the parameter that actually matters. */
    val skinMelanin: Float = 0.35f,
    /** Zero = derived from [skinMelanin]. */
    val hairTone: FloatArray = FloatArray(3),
    /** Primary garment colour; zero = neutral grey. */
    val clothTone: FloatArray = FloatArray(3),
    /** Armour / equipment colour; zero = steel. */
    val gearTone: FloatArray = FloatArray(3),
    /** 0..1 override, negative = material default. */
    val clothRoughness: Float = -1f,
    /** 0..1 override, negative = material default. */
    val gearMetalness: Float = -1f,

    /** The prompt this spec came from; informational, stored in the exported GLB. */
    val prompt: String = "",
) {
    init {
        require(skinTone.size == 3 && hairTone.size == 3 && clothTone.size == 3 && gearTone.size == 3) {
            "colour triplets must be 3 floats each (linear sRGB)"
        }
    }

    /** Human-facing centimetres, derived from the canonical metre value. */
    val heightCm: Float get() = heightM * 100f

    /**
     * Enforces the safety invariant shared with the native ABI. [Outfit.NONE] is the engine's real
     * unclothed body surface, so it is adult-only even while detailed sexual-anatomy controls remain
     * hidden. This is called again below the UI before every build/export.
     */
    fun requireGeneratable(effectivePrompt: String = prompt): AvatarSpec {
        if (outfit == Outfit.NONE) {
            val minorMarkers = setOf(
                "boy", "child", "girl", "kid", "minor", "schoolboy", "schoolgirl", "teen",
                "teenage", "underage",
            )
            val hasMinorMarker = Regex("[A-Za-z]+").findAll(effectivePrompt)
                .map { it.value.lowercase() }
                .any { it in minorMarkers }
            require(!hasMinorMarker) {
                "Nude/anatomical generation rejects minor or teen prompt terms"
            }
            require(ageYears.isFinite() && ageYears >= 18f) {
                "Nude/anatomical generation requires an adult age of 18 or older"
            }
        }
        return this
    }

    /**
     * Applies a centimetre override. Conversion happens here exactly once; JNI and the native
     * anatomy solver continue to receive SI metres through [heightM].
     */
    fun withHeightCm(value: Float): AvatarSpec {
        require(value.isFinite() && value > 0f) { "height centimetres must be finite and positive" }
        return copy(heightM = (value * 0.01f).coerceIn(MIN_HEIGHT_M, MAX_HEIGHT_M))
    }

    /* ------------------------------------------------------------------ packing -------------- */

    /** Number of int32 fields in the packed spec, including the leading struct-size field. */
    val intCount: Int get() = INT_FIELDS

    /** Number of float fields. Must stay equal to [FLOAT_FIELDS] - the JNI checks both. */
    val floatCount: Int get() = FLOAT_FIELDS

    fun packInts(): IntArray {
        val out = IntArray(INT_FIELDS)
        out[0] = 0 // struct_size: filled in by the kernel's own reader, 0 here means "current"
        out[1] = sex.value
        out[2] = build.value
        out[3] = hairStyle.value
        out[4] = outfit.value
        out[5] = footwear.value
        out[6] = seed
        return out
    }

    fun packFloats(): FloatArray {
        val out = FloatArray(FLOAT_FIELDS)
        var i = 0
        out[i++] = heightM
        out[i++] = ageYears
        out[i++] = muscle
        out[i++] = bodyFat
        out[i++] = bust
        out[i++] = neckCm
        out[i++] = chestCm
        out[i++] = underbustCm
        out[i++] = waistCm
        out[i++] = hipCm
        out[i++] = thighCm
        out[i++] = calfCm
        out[i++] = bicepCm
        out[i++] = forearmCm
        out[i++] = wristCm
        out[i++] = shoulderWidthCm
        out[i++] = inseamCm
        out[i++] = headCircumferenceCm
        for (c in 0 until 3) out[i++] = skinTone[c]
        out[i++] = skinMelanin
        for (c in 0 until 3) out[i++] = hairTone[c]
        for (c in 0 until 3) out[i++] = clothTone[c]
        for (c in 0 until 3) out[i++] = gearTone[c]
        out[i++] = clothRoughness
        out[i++] = gearMetalness
        check(i == FLOAT_FIELDS) { "spec packing drifted: wrote $i of $FLOAT_FIELDS floats" }
        return out
    }

    companion object {
        /** Canonical non-zero stature; must match `AF_DEFAULT_HEIGHT_M` in afcore.h. */
        const val DEFAULT_HEIGHT_M = 1.75f
        const val MIN_HEIGHT_M = 1.40f
        const val MAX_HEIGHT_M = 2.20f

        /** Must match `kSpecIntCount` in native/src/android/af_jni.cpp. */
        internal const val INT_FIELDS = 7

        /** Must match `kSpecFloatCount` in native/src/android/af_jni.cpp. */
        internal const val FLOAT_FIELDS = 33

        /**
         * Reads a packed spec back out of the arrays the native side wrote (used by
         * [AvatarEngine.generateFromText] to report what the prompt parser understood, and by the
         * UI to show the user the measurements it is about to use).
         */
        fun unpack(ints: IntArray, floats: FloatArray, prompt: String = ""): AvatarSpec {
            require(ints.size >= INT_FIELDS && floats.size >= FLOAT_FIELDS) {
                "packed spec is ${ints.size} ints / ${floats.size} floats, expected $INT_FIELDS / $FLOAT_FIELDS"
            }
            var i = 0
            fun f() = floats[i++]
            val spec = AvatarSpec(
                sex = Sex.of(ints[1]),
                build = Build.of(ints[2]),
                hairStyle = HairStyle.of(ints[3]),
                outfit = Outfit.of(ints[4]),
                footwear = Footwear.of(ints[5]),
                seed = ints[6],
                heightM = f().let { if (it.isFinite() && it > 0f) it else DEFAULT_HEIGHT_M },
                ageYears = f(),
                muscle = f(),
                bodyFat = f(),
                bust = f(),
                neckCm = f(),
                chestCm = f(),
                underbustCm = f(),
                waistCm = f(),
                hipCm = f(),
                thighCm = f(),
                calfCm = f(),
                bicepCm = f(),
                forearmCm = f(),
                wristCm = f(),
                shoulderWidthCm = f(),
                inseamCm = f(),
                headCircumferenceCm = f(),
                skinTone = floatArrayOf(f(), f(), f()),
                skinMelanin = f(),
                hairTone = floatArrayOf(f(), f(), f()),
                clothTone = floatArrayOf(f(), f(), f()),
                gearTone = floatArrayOf(f(), f(), f()),
                clothRoughness = f(),
                gearMetalness = f(),
                prompt = prompt,
            )
            check(i == FLOAT_FIELDS) { "spec unpacking drifted: read $i of $FLOAT_FIELDS floats" }
            return spec
        }
    }
}
