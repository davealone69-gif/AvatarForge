package com.avatarforge.sdk

import org.junit.Assert.assertSame
import org.junit.Assert.assertThrows
import org.junit.Test

class AdultSafetyTest {
    @Test
    fun `adult nude anatomical base is accepted`() {
        val spec = AvatarSpec(outfit = Outfit.NONE, ageYears = 22f, adultModeConfirmed = true)
        assertSame(spec, spec.requireGeneratable("adult woman, nude"))
    }

    @Test
    fun `unclothed base requires explicit adult confirmation`() {
        val spec = AvatarSpec(outfit = Outfit.NONE, ageYears = 22f)
        assertThrows(IllegalArgumentException::class.java) {
            spec.requireGeneratable("adult woman, nude")
        }
    }

    @Test
    fun `minor nude age is rejected`() {
        val spec = AvatarSpec(outfit = Outfit.NONE, ageYears = 17f)
        assertThrows(IllegalArgumentException::class.java) {
            spec.requireGeneratable("woman")
        }
    }

    @Test
    fun `minor and teen prompt terms are rejected for the nude base`() {
        val spec = AvatarSpec(outfit = Outfit.NONE, ageYears = 22f)
        listOf("teen", "Teenage", "girl", "boy", "child", "underage", "schoolgirl").forEach { marker ->
            assertThrows("marker=$marker", IllegalArgumentException::class.java) {
                spec.requireGeneratable("nude, $marker")
            }
        }
    }

    @Test
    fun `clothed age seventeen is not blocked by nude-only rule`() {
        val spec = AvatarSpec(outfit = Outfit.TSHIRT, ageYears = 17f)
        assertSame(spec, spec.requireGeneratable("teen wearing a t-shirt"))
    }

    @Test
    fun `minor markers use word boundaries`() {
        val spec = AvatarSpec(outfit = Outfit.NONE, ageYears = 22f)
        assertSame(spec, spec.requireGeneratable("adult woman with her girlfriend"))
    }
}
