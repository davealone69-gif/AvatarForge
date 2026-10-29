/*
 * AvatarForge :: bounded local recovery journal.
 *
 * Private app storage only. Records contain fault category, a one-way fingerprint, backend and
 * attempt metadata -- never prompts, photos, mesh data, or raw driver messages. The journal is
 * evidence for the next deterministic regression test, not an autonomous learning dataset.
 */
package com.avatarforge.sdk.recovery

import java.io.File
import java.io.RandomAccessFile
import java.net.URLDecoder
import java.net.URLEncoder

enum class RecoveryRecordKind {
    FAILURE, AUTOMATIC_ATTEMPT, MANUAL_RETRY, MANUAL_SAFE_RECOVERY,
    // Records an explicit native Vulkan request that initialized using GLES instead.
    FRAME_SUBMISSION_STABLE, PROBE_TIMEOUT, BACKEND_FALLBACK
}

data class RecoveryRecord(
    val timestampMs: Long,
    val kind: RecoveryRecordKind,
    val stage: RecoveryStage? = null,
    val category: RecoveryFaultCategory? = null,
    val fingerprint: String = "",
    val backend: String = "",
    val attempt: Int = 0,
    val candidate: String = "",
) {
    fun toReportLine(): String {
        val time = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT)
            .format(java.util.Date(timestampMs))
        val fault = if (category != null) " ${category.name}/${fingerprint}" else ""
        val stageText = if (stage != null) " ${stage.name}" else ""
        val candidateText = if (candidate.isNotBlank()) " -> $candidate" else ""
        return "$time $kind$stageText$fault backend=$backend attempt=$attempt$candidateText"
    }
}

/**
 * A small, bounded, crash-tolerant journal. All instances sharing the same root use one lock, so the
 * UI can read/clear while the render thread appends. IO failures are reported but never crash recovery.
 */
class RecoveryJournal(
    root: File,
    private val nowMs: () -> Long = { System.currentTimeMillis() },
    private val maxRecords: Int = 100,
    private val maxBytes: Int = 64 * 1024,
) {
    val file: File = File(root, "avatarforge/recovery/journal.tsv")

    @Volatile var lastStorageError: String? = null
        private set

    val isAvailable: Boolean get() = lastStorageError == null

    fun record(
        kind: RecoveryRecordKind,
        stage: RecoveryStage? = null,
        category: RecoveryFaultCategory? = null,
        fingerprint: String = "",
        backend: String = "",
        attempt: Int = 0,
        candidate: String = "",
    ): Boolean = append(
        RecoveryRecord(nowMs(), kind, stage, category, fingerprint.take(16), backend.take(32), attempt, candidate.take(64))
    )

    fun append(record: RecoveryRecord): Boolean = synchronized(lock) {
        try {
            val existing = readLocked()
            val records = (existing + record).takeLast(maxRecords).toMutableList()
            while (records.size > 1 && encode(records).toByteArray(Charsets.UTF_8).size > maxBytes) {
                records.removeAt(0)
            }
            writeLocked(encode(records))
            lastStorageError = null
            true
        } catch (t: Throwable) {
            lastStorageError = t.javaClass.simpleName
            false
        }
    }

    fun recent(limit: Int = maxRecords): List<RecoveryRecord> = synchronized(lock) {
        try {
            lastStorageError = null
            readLocked().takeLast(limit.coerceAtLeast(0))
        } catch (t: Throwable) {
            lastStorageError = t.javaClass.simpleName
            emptyList()
        }
    }

    fun clear(): Boolean = synchronized(lock) {
        try {
            file.parentFile?.mkdirs()
            val temp = File(file.parentFile, "journal.tmp")
            temp.writeText("", Charsets.UTF_8)
            if (!temp.renameTo(file)) {
                file.writeText("", Charsets.UTF_8)
                temp.delete()
            }
            lastStorageError = null
            true
        } catch (t: Throwable) {
            lastStorageError = t.javaClass.simpleName
            false
        }
    }

    private fun encode(records: List<RecoveryRecord>): String {
        if (records.isEmpty()) return ""
        val body = records.joinToString("\n") { r ->
            listOf(
                VERSION, r.timestampMs.toString(), r.kind.name, r.stage?.name ?: "-",
                r.category?.name ?: "-", r.fingerprint, r.backend, r.attempt.toString(), r.candidate,
            ).joinToString("\t") { URLEncoder.encode(it, "UTF-8") }
        }
        return body + "\n"
    }

    private fun writeLocked(text: String) {
        file.parentFile?.mkdirs()
        val temp = File(file.parentFile, "journal.tmp")
        temp.writeText(text, Charsets.UTF_8)
        if (!temp.renameTo(file)) {
            file.writeText(text, Charsets.UTF_8)
            temp.delete()
        }
    }

    private fun readLocked(): List<RecoveryRecord> {
        if (!file.exists()) return emptyList()
        val length = file.length()
        if (length <= 0L) return emptyList()
        val window = (maxBytes * 2).toLong()
        val start = maxOf(0L, length - window)
        val bytes = RandomAccessFile(file, "r").use { raf ->
            raf.seek(start)
            ByteArray((length - start).toInt()).also { raf.readFully(it) }
        }
        val lines = String(bytes, Charsets.UTF_8).lineSequence()
        return (if (start > 0L) lines.drop(1) else lines).mapNotNull(::decode).toList()
    }

    private fun decode(line: String): RecoveryRecord? {
        if (line.isBlank()) return null
        val parts = line.split('\t')
        if (parts.size != 9) return null
        val fields = parts.map { URLDecoder.decode(it, "UTF-8") }
        if (fields[0] != VERSION) return null
        val timestamp = fields[1].toLongOrNull() ?: return null
        val kind = runCatching { RecoveryRecordKind.valueOf(fields[2]) }.getOrNull() ?: return null
        val stage = if (fields[3] == "-") null else runCatching { RecoveryStage.valueOf(fields[3]) }.getOrNull()
        val category = if (fields[4] == "-") null else runCatching { RecoveryFaultCategory.valueOf(fields[4]) }.getOrNull()
        return RecoveryRecord(
            timestampMs = timestamp,
            kind = kind,
            stage = stage,
            category = category,
            fingerprint = fields[5].take(16),
            backend = fields[6].take(32),
            attempt = fields[7].toIntOrNull() ?: 0,
            candidate = fields[8].take(64),
        )
    }

    private companion object {
        const val VERSION = "AFRJ1"
        val lock = Any()
    }
}
