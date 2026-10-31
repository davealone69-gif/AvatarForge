package com.avatarforge.sdk.render

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.sqrt

class FilamentViewerMathTest {
    @Test
    fun defaultCameraFramesHumanScaleAvatarFromItsFront() {
        val pose = filamentCameraPose(
            CameraSettings(),
            centerX = 0f,
            centerY = 0.95f,
            centerZ = 0f,
            halfExtentX = 0.45f,
            halfExtentY = 0.95f,
            halfExtentZ = 0.35f,
        )

        assertEquals(0f, pose.targetX, 0.0001f)
        assertEquals(0.95f, pose.targetY, 0.0001f)
        assertTrue("yaw 180° must place the camera on the avatar's -Z front", pose.eyeZ < pose.targetZ)
        assertEquals(3.5f, eyeToTargetDistance(pose), 0.001f)
        assertTrue(pose.near > 0f)
        assertTrue(pose.far > pose.near)
    }

    @Test
    fun cameraDistanceScalesToNonHumanAssetBounds() {
        val pose = filamentCameraPose(
            CameraSettings(),
            centerX = 2f,
            centerY = 0.475f,
            centerZ = -1f,
            halfExtentX = 0.225f,
            halfExtentY = 0.475f,
            halfExtentZ = 0.175f,
        )

        assertEquals(2f, pose.targetX, 0.0001f)
        assertEquals(0.475f, pose.targetY, 0.0001f)
        assertEquals(-1f, pose.targetZ, 0.0001f)
        assertEquals(1.75f, eyeToTargetDistance(pose), 0.001f)
    }

    @Test
    fun animationTimeWrapsAtClipDuration() {
        assertEquals(0.05f, advanceLoopingAnimationTime(1.95f, 0.1f, 2f), 0.0001f)
    }

    @Test
    fun animationTimeDoesNotAdvanceWhilePausedOrOnInvalidDuration() {
        assertEquals(0.4f, advanceLoopingAnimationTime(0.4f, 0f, 1f), 0.0001f)
        assertEquals(0f, advanceLoopingAnimationTime(0.4f, 0.1f, 0f), 0f)
    }

    @Test
    fun animationTimeClampsLongFrameAfterStall() {
        assertEquals(0.1f, advanceLoopingAnimationTime(0f, 5f, 10f), 0.0001f)
    }

    private fun eyeToTargetDistance(pose: FilamentCameraPose): Float {
        val dx = pose.eyeX - pose.targetX
        val dy = pose.eyeY - pose.targetY
        val dz = pose.eyeZ - pose.targetZ
        return sqrt(dx * dx + dy * dy + dz * dz)
    }
}
