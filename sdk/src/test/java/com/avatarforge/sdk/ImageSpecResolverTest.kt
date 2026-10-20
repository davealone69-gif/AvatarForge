package com.avatarforge.sdk

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ImageSpecResolverTest {

    @Test
    fun `renderer lease release action is idempotent`() {
        var released = 0
        val lease = NativeEngineLease(42L) { released++ }

        lease.release()
        lease.release()

        assertEquals(1, released)
    }

    @Test
    fun `full body skin hair garment and plate footwear mirror native analysis rules`() {
        val identity = analysis(
            fullBody = true,
            poseConfidence = 0.95f,
            metalConfidence = 0.1f,
            skinLab = floatArrayOf(50f, 8f, 4f),
            hairLab = floatArrayOf(35f, 2f, 3f),
            garmentLab = floatArrayOf(30f, 0f, 0f),
            backgroundLab = floatArrayOf(80f, 0f, 0f),
        )
        val clothing = analysis(
            metalConfidence = 0.7f,
            garmentLab = floatArrayOf(55f, 12f, 18f),
        )

        val resolved = ImageSpecResolver.resolve(identity, clothing, AvatarSpec())

        assertEquals(1.88f, resolved.heightM, 0.0001f)
        assertEquals(0.295f, resolved.skinMelanin, 0.0001f)
        assertTrue(resolved.hairTone.all { it in 0.01f..1f })
        assertEquals(Outfit.ARMOR_PLATE, resolved.outfit)
        assertEquals(Footwear.SABATON, resolved.footwear)
        assertEquals(-1f, resolved.gearMetalness, 0f)

        val expectedCloth = floatArrayOf(0.36290284f, 0.19952522f, 0.13062638f)
        assertArrayEquals(expectedCloth, resolved.clothTone, 0.0001f)
        assertArrayEquals(
            FloatArray(3) { (resolved.clothTone[it] * 1.05f).coerceIn(0.01f, 1f) },
            resolved.gearTone,
            0.0001f,
        )
    }

    @Test
    fun `explicit stature and footwear survive while a medium metal tshirt hint selects leather`() {
        val identity = analysis(fullBody = true, poseConfidence = 1f)
        val clothing = analysis(metalConfidence = 0.5f)
        val base = AvatarSpec(
            heightM = 1.9f,
            outfit = Outfit.TSHIRT,
            footwear = Footwear.SNEAKER,
        )

        val resolved = ImageSpecResolver.resolve(identity, clothing, base)

        assertEquals(1.9f, resolved.heightM, 0f)
        assertEquals(Outfit.ARMOR_LEATHER, resolved.outfit)
        assertEquals(Footwear.SNEAKER, resolved.footwear)
    }

    @Test
    fun `last identity and clothing role match native selection`() {
        val firstIdentity = analysis(fullBody = false, skinLab = floatArrayOf(25f, 0f, 0f))
        val lastIdentity = analysis(
            fullBody = true,
            poseConfidence = 1f,
            skinLab = floatArrayOf(58f, 0f, 0f),
        )
        val firstClothing = analysis(metalConfidence = 0.1f, garmentLab = floatArrayOf(25f, 0f, 0f))
        val lastClothing = analysis(metalConfidence = 0.8f, garmentLab = floatArrayOf(60f, 0f, 0f))
        val inputs = listOf(
            ImageRole.IDENTITY to firstIdentity,
            ImageRole.CLOTHING to firstClothing,
            ImageRole.IDENTITY to lastIdentity,
            ImageRole.CLOTHING to lastClothing,
        )

        val resolved = ImageSpecResolver.resolveImages(inputs, AvatarSpec())
        val expected = ImageSpecResolver.resolve(lastIdentity, lastClothing, AvatarSpec())

        assertEquals(expected.heightM, resolved.heightM, 0f)
        assertEquals(expected.skinMelanin, resolved.skinMelanin, 0f)
        assertEquals(expected.outfit, resolved.outfit)
        assertArrayEquals(expected.clothTone, resolved.clothTone, 0f)
    }

    @Test
    fun `portrait uses default height and a nonmetal identity hint resolves none to tshirt`() {
        val identity = analysis(fullBody = false, metalConfidence = 0.3f)

        val resolved = ImageSpecResolver.resolve(identity, null, AvatarSpec())

        assertEquals(AvatarSpec.DEFAULT_HEIGHT_M, resolved.heightM, 0f)
        assertEquals(Outfit.TSHIRT, resolved.outfit)
        assertEquals(Footwear.NONE, resolved.footwear)
    }

    private fun analysis(
        fullBody: Boolean = false,
        poseConfidence: Float = 0.8f,
        metalConfidence: Float = 0f,
        skinLab: FloatArray = floatArrayOf(62f, 12f, 18f),
        hairLab: FloatArray = floatArrayOf(30f, 0f, 0f),
        garmentLab: FloatArray = floatArrayOf(60f, 0f, 0f),
        backgroundLab: FloatArray = floatArrayOf(90f, 0f, 0f),
    ) = ImageReference.Analysis(
        subjectCoverage = 0.6f,
        headBox = floatArrayOf(0.2f, 0.1f, 0.4f, 0.1f),
        shoulderY = 0.25f,
        hipY = 0.55f,
        kneeY = 0.75f,
        ankleY = 0.9f,
        isFullBody = fullBody,
        poseConfidence = poseConfidence,
        skinLab = skinLab,
        hairLab = hairLab,
        garmentLab = garmentLab,
        metalConfidence = metalConfidence,
        keyLight = floatArrayOf(0f, 1f, 0f),
        keyLightIntensity = 1f,
        backgroundLab = backgroundLab,
    )
}
