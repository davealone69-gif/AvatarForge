package com.avatarforge.sdk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Locks finding AF-003: a failed or empty image analysis must produce a visible, honest note.
 * Per-image lines require an ImageReference (built via the Android Bitmap factory), so those are
 * covered on-device; the failure/empty branches that used to be swallowed are covered here.
 */
class ImageAnalysisNotesTest {

    @Test
    fun `no analysis attempted yields no note`() {
        assertNull(ImageAnalysisNotes.describe(results = null, failure = null))
    }

    @Test
    fun `analysis failure is never silent and carries the reason`() {
        val note = ImageAnalysisNotes.describe(results = null, failure = "native abort 42")
        requireNotNull(note)
        assertTrue(note.contains("Photo analysis failed"))
        assertTrue(note.contains("native abort 42"))
        assertTrue(note.contains("text prompt only"))
    }

    @Test
    fun `empty result list is reported explicitly rather than ignored`() {
        val note = ImageAnalysisNotes.describe(results = emptyList(), failure = null)
        requireNotNull(note)
        assertTrue(note.contains("no measurements"))
        assertTrue(note.contains("text prompt only"))
    }

    @Test
    fun `failure takes precedence over results`() {
        val note = ImageAnalysisNotes.describe(results = emptyList(), failure = "boom")
        requireNotNull(note)
        assertEquals("Photo analysis failed (boom); generation continues from the text prompt only.", note)
    }
}
