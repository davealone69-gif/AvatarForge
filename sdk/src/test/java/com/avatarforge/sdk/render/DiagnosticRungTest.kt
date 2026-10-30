package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DiagnosticRungTest {
    @Test
    fun `rungs are exactly eight values in native order`() {
        assertEquals(8, DiagnosticRung.entries.size)
        assertEquals((0..7).toList(), DiagnosticRung.entries.map { it.value })
        assertEquals("clear", DiagnosticRung.entries.first().displayName)
        assertEquals("production avatar", DiagnosticRung.entries.last().displayName)
        assertEquals("the production avatar renders end-to-end", DiagnosticRung.PRODUCTION_AVATAR.passCriterion)
    }

    @Test
    fun `invalid values do not produce a selectable rung and fall back to production`() {
        assertNull(DiagnosticRung.fromValue(-1))
        assertNull(DiagnosticRung.fromValue(8))
        assertEquals(DiagnosticRung.PRODUCTION_AVATAR, DiagnosticRung.orDefault(-1))
        assertEquals(DiagnosticRung.PRODUCTION_AVATAR, DiagnosticRung.orDefault(99))
    }
}
