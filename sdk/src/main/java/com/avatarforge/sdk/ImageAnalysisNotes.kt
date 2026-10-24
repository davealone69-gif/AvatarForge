/*
 * AvatarForge :: ImageAnalysisNotes.kt
 *
 * Pure-Kotlin mapping from an image-analysis outcome to the single-line note shown on the input
 * screen. It deliberately contains no Android types so the wording is locked by a JVM unit test.
 *
 * Why this exists (finding AF-003): a failed analyseImages() call used to be swallowed and the
 * generation silently fell back to the text-only spec, so users believed their reference photos
 * had influenced the avatar. The note must state one of three honest outcomes:
 *   1. measured facts for every analysed image;
 *   2. an explicit "analysis unavailable" message when the engine returned no records;
 *   3. an explicit failure message carrying the exception reason.
 * No analysis value is ever invented for a missing record.
 */
package com.avatarforge.sdk

object ImageAnalysisNotes {

    /** One human-readable line per analysed image, or null when there is nothing to describe. */
    fun describeOne(ref: ImageReference): String {
        val an = ref.analysis
            ?: return "no analysis available for the ${ref.role.name.lowercase()} reference"
        return buildString {
            append("subject %.0f%% of frame".format(an.subjectCoverage * 100f))
            append(" · ")
            append(if (an.isFullBody) "full body detected" else "upper body: lower body inferred")
            append(" · skin L* %.0f".format(an.skinLab[0]))
            append(" · hair L* %.0f".format(an.hairLab[0]))
            append(" · garment metalness %.2f".format(an.metalConfidence))
            append(" · key light %.2f".format(an.keyLightIntensity))
        }
    }

    /**
     * The note for the input card, or null when analysis has not been attempted. [failure] is the
     * exception message when analyseImages threw; [results] is the engine return otherwise.
     */
    fun describe(results: List<ImageReference>?, failure: String?): String? {
        if (failure != null) {
            return "Photo analysis failed ($failure); generation continues from the text prompt only."
        }
        if (results == null) return null
        if (results.isEmpty()) {
            return "Photo analysis returned no measurements; generation continues from the text prompt only."
        }
        return results.joinToString("\n") { describeOne(it) }
    }
}
