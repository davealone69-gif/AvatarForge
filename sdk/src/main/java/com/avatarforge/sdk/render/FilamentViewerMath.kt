/*
 * AvatarForge :: pure Filament viewer camera and animation math.
 *
 * These helpers are deliberately independent of Filament and Android GPU state, making framing and
 * playback edge cases deterministic in local JVM tests.
 */
package com.avatarforge.sdk.render

import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

internal data class FilamentCameraPose(
    val eyeX: Float,
    val eyeY: Float,
    val eyeZ: Float,
    val targetX: Float,
    val targetY: Float,
    val targetZ: Float,
    val near: Float,
    val far: Float,
)

/**
 * Fits a default human-scale camera to the asset bounds while preserving CameraSettings' existing
 * orbit convention (yaw 180° views the model from its -Z front) and gesture distance scaling.
 */
internal fun filamentCameraPose(
    camera: CameraSettings,
    centerX: Float,
    centerY: Float,
    centerZ: Float,
    halfExtentX: Float,
    halfExtentY: Float,
    halfExtentZ: Float,
): FilamentCameraPose {
    val settings = CameraGesture.clamped(camera)
    val safeHalfX = halfExtentX.takeIf { it.isFinite() && it > 0f } ?: 0.45f
    val safeHalfY = halfExtentY.takeIf { it.isFinite() && it > 0f } ?: 0.95f
    val safeHalfZ = halfExtentZ.takeIf { it.isFinite() && it > 0f } ?: 0.35f
    val safeCenterX = centerX.takeIf(Float::isFinite) ?: 0f
    val safeCenterY = centerY.takeIf(Float::isFinite) ?: safeHalfY
    val safeCenterZ = centerZ.takeIf(Float::isFinite) ?: 0f
    val width = safeHalfX * 2f
    val height = safeHalfY * 2f
    val depth = safeHalfZ * 2f
    val characteristicSize = max(max(width, height), depth).coerceAtLeast(0.01f)
    val framingScale = characteristicSize / HUMAN_REFERENCE_HEIGHT

    val targetX = safeCenterX
    val targetY = safeCenterY - safeHalfY + height * (settings.targetY / HUMAN_REFERENCE_HEIGHT)
    val targetZ = safeCenterZ
    val distance = settings.distance * framingScale
    val yaw = Math.toRadians(settings.yawDeg.toDouble())
    val pitch = Math.toRadians(settings.pitchDeg.toDouble())
    val horizontal = distance * cos(pitch).toFloat()
    val eyeX = targetX + horizontal * sin(yaw).toFloat()
    val eyeY = targetY + distance * sin(pitch).toFloat()
    val eyeZ = targetZ + horizontal * cos(yaw).toFloat()

    val near = (distance - characteristicSize * 1.5f).coerceAtLeast(0.01f)
    val far = max(distance + characteristicSize * 4f, near + 1f)
    return FilamentCameraPose(eyeX, eyeY, eyeZ, targetX, targetY, targetZ, near, far)
}

/** Advances a looping clip safely, limiting a delayed frame to avoid a large background-resume jump. */
internal fun advanceLoopingAnimationTime(currentSeconds: Float, deltaSeconds: Float, durationSeconds: Float): Float {
    if (!durationSeconds.isFinite() || durationSeconds <= 0f) return 0f
    val current = currentSeconds.takeIf(Float::isFinite)?.coerceAtLeast(0f) ?: 0f
    val delta = deltaSeconds.takeIf(Float::isFinite)?.coerceIn(0f, MAX_FRAME_DELTA_SECONDS) ?: 0f
    return (current + delta) % durationSeconds
}

private const val HUMAN_REFERENCE_HEIGHT = 1.9f
private const val MAX_FRAME_DELTA_SECONDS = 0.1f
