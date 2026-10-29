package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class CameraGestureTest {
    @Test
    fun `sequential drag deltas accumulate instead of resetting to the first camera`() {
        var camera = CameraSettings()
        camera = CameraGesture.orbit(camera, panX = 10f, panY = 0f)
        camera = CameraGesture.orbit(camera, panX = 10f, panY = 0f)
        assertEquals(175f, camera.yawDeg, 0.0001f)

        camera = CameraGesture.orbit(camera, panX = 0f, panY = 10f)
        camera = CameraGesture.orbit(camera, panX = 0f, panY = 10f)
        assertEquals(8f, camera.pitchDeg, 0.0001f)
    }

    @Test
    fun `pinch zoom moves distance in the expected direction and clamps`() {
        val start = CameraSettings(distance = 3.5f)
        val closer = CameraGesture.orbit(start, 0f, 0f, zoom = 2f)
        assertEquals(1.75f, closer.distance, 0.0001f)
        val farther = CameraGesture.orbit(start, 0f, 0f, zoom = 0.5f)
        assertEquals(7f, farther.distance, 0.0001f)

        assertEquals(CameraGesture.MIN_DISTANCE, CameraGesture.orbit(start, 0f, 0f, zoom = 1000f).distance, 0f)
        assertEquals(CameraGesture.MAX_DISTANCE, CameraGesture.orbit(start, 0f, 0f, zoom = 0.001f).distance, 0f)
    }

    @Test
    fun `yaw wraps and pitch stays inside the orbit limits`() {
        assertEquals(350f, CameraGesture.normalizeYaw(-10f), 0.0001f)
        assertEquals(10f, CameraGesture.normalizeYaw(370f), 0.0001f)

        val up = CameraGesture.orbit(CameraSettings(pitchDeg = 79f), 0f, panY = 100f)
        assertEquals(CameraGesture.MAX_PITCH, up.pitchDeg, 0f)
        val down = CameraGesture.orbit(CameraSettings(pitchDeg = -79f), 0f, panY = -100f)
        assertEquals(CameraGesture.MIN_PITCH, down.pitchDeg, 0f)
    }

    @Test
    fun `non-finite or zero gesture input cannot collapse the camera`() {
        val start = CameraSettings()
        val unchanged = CameraGesture.orbit(start, Float.NaN, Float.POSITIVE_INFINITY, zoom = 0f)
        assertEquals(start.yawDeg, unchanged.yawDeg, 0f)
        assertEquals(start.pitchDeg, unchanged.pitchDeg, 0f)
        assertEquals(start.distance, unchanged.distance, 0f)

        val negativeZoom = CameraGesture.orbit(start, 0f, 0f, zoom = -2f)
        assertEquals(start.distance, negativeZoom.distance, 0f)
    }

    @Test
    fun `clamped repairs invalid state and preserves valid framing fields`() {
        val repaired = CameraGesture.clamped(
            CameraSettings(
                yawDeg = Float.NaN,
                pitchDeg = Float.POSITIVE_INFINITY,
                distance = 0f,
                targetY = -1f,
                fovDeg = Float.NaN,
            )
        )
        assertEquals(180f, repaired.yawDeg, 0f)
        assertEquals(4f, repaired.pitchDeg, 0f)
        assertEquals(3.5f, repaired.distance, 0f)
        assertEquals(0f, repaired.targetY, 0f)
        assertEquals(32f, repaired.fovDeg, 0f)

        val valid = CameraSettings(yawDeg = 90f, pitchDeg = 10f, distance = 2f, targetY = 1.2f, fovDeg = 40f)
        assertEquals(valid, CameraGesture.clamped(valid))
        assertTrue(CameraGesture.orbit(valid, 0f, 0f, 1f) == valid)
    }
}
