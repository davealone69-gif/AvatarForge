package com.avatarforge.sdk.recovery

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class RecoveryJournalTest {
    @get:Rule
    val temp = TemporaryFolder()

    @Test
    fun `journal round trips bounded records and never stores raw driver messages`() {
        val journal = RecoveryJournal(temp.root)
        assertTrue(journal.record(
            RecoveryRecordKind.FAILURE,
            stage = RecoveryStage.STARTUP,
            category = RecoveryFaultCategory.SHADER_COMPILE,
            fingerprint = "0123456789abcdef",
            backend = "GLES",
            attempt = 1,
            candidate = "safe-gles",
        ))
        val records = journal.recent(10)
        assertEquals(1, records.size)
        assertEquals(RecoveryFaultCategory.SHADER_COMPILE, records[0].category)
        assertEquals("safe-gles", records[0].candidate)
        val text = File(temp.root, "avatarforge/recovery/journal.tsv").readText()
        assertTrue(text.startsWith("AFRJ1\t"))
        assertFalse(text.contains("fragment shader"))
        assertFalse(text.contains("prompt"))
    }

    @Test
    fun `backend fallback is journaled as evidence, not hidden`() {
        val journal = RecoveryJournal(temp.root)
        assertTrue(journal.record(
            RecoveryRecordKind.BACKEND_FALLBACK,
            backend = "gles31",
            candidate = "vulkan->gles",
        ))
        val record = journal.recent(1).single()
        assertEquals(RecoveryRecordKind.BACKEND_FALLBACK, record.kind)
        assertEquals("gles31", record.backend)
        assertEquals("vulkan->gles", record.candidate)
        assertTrue(record.toReportLine().contains("BACKEND_FALLBACK"))
    }

    @Test
    fun `journal enforces record and byte caps`() {
        val journal = RecoveryJournal(temp.root, maxRecords = 5, maxBytes = 512)
        repeat(20) { i ->
            journal.record(RecoveryRecordKind.MANUAL_RETRY, backend = "GLES", attempt = i)
        }
        val records = journal.recent(100)
        assertTrue(records.size <= 5)
        val file = File(temp.root, "avatarforge/recovery/journal.tsv")
        assertTrue(file.length() <= 512)
    }

    @Test
    fun `corrupt lines are skipped and clear removes the journal`() {
        val journal = RecoveryJournal(temp.root)
        journal.record(RecoveryRecordKind.MANUAL_RETRY, backend = "GLES")
        val file = File(temp.root, "avatarforge/recovery/journal.tsv")
        file.appendText("not-a-valid-record\nAFRJ1\tbad\tfields\n")
        assertEquals(1, journal.recent(10).size)
        assertTrue(journal.clear())
        assertTrue(journal.recent(10).isEmpty())
    }
}
