/*
 * AvatarForge :: pure orbit-camera gesture maths.
 *
 * Kept in the SDK (not the Compose layer) so the exact transform used by touch gestures is covered
 * by JVM regression tests. The renderer remains the only owner of the GPU camera; this file only
 * produces the next CameraSettings value from a gesture delta.
 */
package com.avatarforge.sdk.render

object CameraGesture {
    const val MIN_DISTANCE = 0.6f
    const val MAX_DISTANCE = 8.0f
    const val MIN_PITCH = -80.0f
    const val MAX_PITCH = 80.0f
    const val YAW_DEGREES_PER_PIXEL = 0.25f
    const val PITCH_DEGREES_PER_PIXEL = 0.20f

    /** Normalises yaw into [0, 360) and clamps the orbit to safe, non-degenerate values. */
    fun clamped(camera: CameraSettings): CameraSettings {
        val yaw = if (camera.yawDeg.isFinite()) normalizeYaw(camera.yawDeg) else 180f
        val pitch = if (camera.pitchDeg.isFinite()) camera.pitchDeg.coerceIn(MIN_PITCH, MAX_PITCH) else 4f
        val distance = if (camera.distance.isFinite() && camera.distance > 0f) {
            camera.distance.coerceIn(MIN_DISTANCE, MAX_DISTANCE)
        } else {
            3.5f
        }
        val targetY = if (camera.targetY.isFinite()) camera.targetY.coerceIn(0f, 2.5f) else 0.95f
        val fov = if (camera.fovDeg.isFinite()) camera.fovDeg.coerceIn(20f, 60f) else 32f
        return camera.copy(yawDeg = yaw, pitchDeg = pitch, distance = distance, targetY = targetY, fovDeg = fov)
    }

    /**
     * Applies one transform-gesture delta. [panX]/[panY] are pixels since the previous event;
     * [zoom] is the multiplicative pinch factor since the previous event ( > 1 = pinch out ).
     * Non-finite or zero zoom is ignored rather than allowed to collapse the camera.
     */
    fun orbit(camera: CameraSettings, panX: Float, panY: Float, zoom: Float = 1f): CameraSettings {
        val base = clamped(camera)
        val dx = if (panX.isFinite()) panX else 0f
        val dy = if (panY.isFinite()) panY else 0f
        val z = if (zoom.isFinite() && zoom > 0f) zoom else 1f
        return clamped(
            base.copy(
                yawDeg = base.yawDeg - dx * YAW_DEGREES_PER_PIXEL,
                pitchDeg = base.pitchDeg + dy * PITCH_DEGREES_PER_PIXEL,
                distance = base.distance / z,
            )
        )
    }

    fun normalizeYaw(yaw: Float): Float {
        val wrapped = yaw % 360f
        return if (wrapped < 0f) wrapped + 360f else wrapped
    }
}
