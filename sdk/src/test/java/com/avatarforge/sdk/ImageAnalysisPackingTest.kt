package com.avatarforge.sdk

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import com.avatarforge.sdk.internal.NativeBridge

class ImageAnalysisPackingTest {

    @Test
    fun `complete schema has 28 floats and decodes one through four records`() {
        assertEquals(28, ImageAnalysisPacking.FLOAT_COUNT)

        for (recordCount in 1..4) {
            val packed = FloatArray(recordCount * ImageAnalysisPacking.FLOAT_COUNT)
            for (record in 0 until recordCount) {
                val start = record * ImageAnalysisPacking.FLOAT_COUNT
                val base = record * 100f
                for (field in 0 until ImageAnalysisPacking.FLOAT_COUNT) {
                    packed[start + field] = base + field
                }
                packed[start + 9] = if (record % 2 == 0) 1f else 0f
            }

            for (record in 0 until recordCount) {
                val base = record * 100f
                val analysis = ImageAnalysisPacking.unpack(packed, record)
                assertEquals(base + 0f, analysis.subjectCoverage, 0f)
                assertArrayEquals(floatArrayOf(base + 1f, base + 2f, base + 3f, base + 4f), analysis.headBox, 0f)
                assertEquals(base + 5f, analysis.shoulderY, 0f)
                assertEquals(base + 6f, analysis.hipY, 0f)
                assertEquals(base + 7f, analysis.ankleY, 0f)
                assertEquals(base + 8f, analysis.kneeY, 0f)
                if (record % 2 == 0) assertTrue(analysis.isFullBody) else assertFalse(analysis.isFullBody)
                assertEquals(base + 10f, analysis.poseConfidence, 0f)
                assertArrayEquals(floatArrayOf(base + 11f, base + 12f, base + 13f), analysis.skinLab, 0f)
                assertArrayEquals(floatArrayOf(base + 14f, base + 15f, base + 16f), analysis.hairLab, 0f)
                assertArrayEquals(floatArrayOf(base + 17f, base + 18f, base + 19f), analysis.garmentLab, 0f)
                assertEquals(base + 20f, analysis.metalConfidence, 0f)
                assertArrayEquals(floatArrayOf(base + 21f, base + 22f, base + 23f), analysis.keyLight, 0f)
                assertEquals(base + 24f, analysis.keyLightIntensity, 0f)
                assertArrayEquals(floatArrayOf(base + 25f, base + 26f, base + 27f), analysis.backgroundLab, 0f)
            }
        }
    }

    @Test
    fun `short arrays and out of range record indices are rejected before reading`() {
        assertThrows(IllegalArgumentException::class.java) {
            ImageAnalysisPacking.unpack(FloatArray(ImageAnalysisPacking.FLOAT_COUNT - 1), 0)
        }
        assertThrows(IllegalArgumentException::class.java) {
            ImageAnalysisPacking.unpack(FloatArray(ImageAnalysisPacking.FLOAT_COUNT), 1)
        }
        assertThrows(IllegalArgumentException::class.java) {
            ImageAnalysisPacking.unpack(FloatArray(ImageAnalysisPacking.FLOAT_COUNT), -1)
        }
    }

    @Test
    fun `texture copy JNI declaration returns the native byte-count type`() {
        val method = NativeBridge::class.java.getDeclaredMethod(
            "engineCopyTexture",
            Long::class.javaPrimitiveType,
            Int::class.javaPrimitiveType,
            java.nio.ByteBuffer::class.java,
        )
        assertEquals(Long::class.javaPrimitiveType, method.returnType)
    }

}
