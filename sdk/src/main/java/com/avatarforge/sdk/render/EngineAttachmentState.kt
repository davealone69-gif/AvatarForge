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

    /** Returns false for a delayed retirement callback from a replaced attachment. */
    @Synchronized
    fun retire(attachment: Attachment): Boolean {
        if (current !== attachment) return false
        current = null
        return true
    }
}
