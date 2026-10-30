package com.avatarforge.sdk.render

/**
 * Renderer-owned diagnostic fixtures, ordered from the simplest presentation check to the
 * production avatar. The values are part of the native ABI and intentionally have no COUNT entry.
 */
enum class DiagnosticRung(
    val value: Int,
    val displayName: String,
    val passCriterion: String,
) {
    CLEAR(0, "clear", "surface clears and presents with no geometry"),
    TRIANGLE(1, "triangle", "one non-indexed triangle is visible"),
    INDEXED_TRIANGLE(2, "indexed triangle", "one indexed triangle is visible"),
    TEXTURED_QUAD(3, "textured quad", "a sampled textured quad is visible"),
    LIT_SPHERE(4, "lit sphere", "a sphere responds to the lighting path"),
    SKINNED_MESH(5, "skinned mesh", "joint deformation changes the mesh"),
    BLENDSHAPE_MESH(6, "blendshape mesh", "a blendshape deformation is visible"),
    PRODUCTION_AVATAR(7, "production avatar", "the production avatar renders end-to-end");

    companion object {
        const val DEFAULT_VALUE: Int = 7

        fun fromValue(value: Int): DiagnosticRung? = entries.firstOrNull { it.value == value }

        /** Converts an external/native value without allowing an invalid rung into UI state. */
        fun orDefault(value: Int): DiagnosticRung = fromValue(value) ?: PRODUCTION_AVATAR
    }
}
