package com.avatarforge.sdk.render

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class EngineAttachmentStateTest {
    @Test
    fun `retiring A after rapid A to B replacement leaves B current`() {
        val state = EngineAttachmentState()
        val attachmentA = state.attach(101L)
        val attachmentB = state.attach(202L)

        assertFalse(state.isCurrent(attachmentA))
        assertFalse(state.retire(attachmentA))
        assertTrue(state.isCurrent(attachmentB))
    }
}
