package com.avatarforge.sdk.render

/** Tracks ownership of the latest requested engine attachment. */
internal class EngineAttachmentState {
    internal class Attachment internal constructor(val engineHandle: Long)

    private var current: Attachment? = null

    @Synchronized
    fun attach(engineHandle: Long): Attachment =
        Attachment(engineHandle).also { current = it }

    @Synchronized
    fun isCurrent(attachment: Attachment): Boolean = current === attachment

    /** Runs the renderer mutation while replacement is excluded by the same ownership lock. */
    @Synchronized
    fun attachIfCurrent(attachment: Attachment, attach: (Long) -> Unit): Boolean {
        if (current !== attachment) return false
        attach(attachment.engineHandle)
        return true
    }

    /** Attaches the latest requested engine to a newly created renderer, if one exists. */
    @Synchronized
    fun attachCurrent(attach: (Long) -> Unit): Boolean {
        val attachment = current ?: return false
        attach(attachment.engineHandle)
        return true
    }

    /** Returns false for a delayed retirement callback from a replaced attachment. */
    @Synchronized
    fun retire(attachment: Attachment): Boolean {
        if (current !== attachment) return false
        current = null
        return true
    }
}
