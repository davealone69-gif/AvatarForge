/*
 * af_avatar_culler.h -- bridge between the renderer's camera contract and the tested scene layer.
 *
 * The renderer backends share CameraState and the POD orbit pose below. Keep the scene graph behind
 * this PIMPL so the scene and UBO vector/matrix types never collide in a renderer translation unit.
 */
#ifndef AF_AVATAR_CULLER_H
#define AF_AVATAR_CULLER_H

#include "af_render.h"

#include <cmath>
#include <memory>

namespace af {

struct EffectiveOrbitCamera {
    float eye[3];
    float target[3];
};

constexpr float kCameraGroundClearance = 0.05f;
constexpr float kCameraMinPitchDegrees = -80.0f;
constexpr float kCameraMaxPitchDegrees = 80.0f;

/* One sanitized orbit pose for rendering, picking, and frustum culling.
   Supported values are yaw [0,360), pitch [-80,80] degrees, targetY [0,2.5] m,
   and distance [0.3,8] m. Non-finite inputs use the standard camera defaults.
   If target/distance cannot reach the 5 cm clearance even at +80 degrees, use
   the complete default pose (yaw 180, pitch 4, targetY 0.95, distance 3.5). */
inline EffectiveOrbitCamera effectiveOrbitCamera(const CameraState& cam) {
    constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;
    float yawDegrees = std::isfinite(cam.yawDeg) ? cam.yawDeg : 180.0f;
    yawDegrees = std::fmod(yawDegrees, 360.0f);
    if (yawDegrees < 0.0f) yawDegrees += 360.0f;
    float pitchDegrees = std::isfinite(cam.pitchDeg) ? cam.pitchDeg : 4.0f;
    if (pitchDegrees < kCameraMinPitchDegrees) pitchDegrees = kCameraMinPitchDegrees;
    if (pitchDegrees > kCameraMaxPitchDegrees) pitchDegrees = kCameraMaxPitchDegrees;
    float dist = std::isfinite(cam.distance) && cam.distance > 0.0f ? cam.distance : 3.5f;
    float targetY = std::isfinite(cam.targetY) ? cam.targetY : 0.95f;

    const float maxPitch = kCameraMaxPitchDegrees * kRadiansPerDegree;
    if (targetY + dist * std::sin(maxPitch) < kCameraGroundClearance) {
        yawDegrees = 180.0f;
        pitchDegrees = 4.0f;
        dist = 3.5f;
        targetY = 0.95f;
    }
    if (dist < 0.3f) dist = 0.3f;
    if (dist > 8.0f) dist = 8.0f;
    if (targetY < 0.0f) targetY = 0.0f;
    if (targetY > 2.5f) targetY = 2.5f;

    float minPitchSin = (kCameraGroundClearance - targetY) / dist;
    if (minPitchSin < -1.0f) minPitchSin = -1.0f;
    if (minPitchSin > 1.0f) minPitchSin = 1.0f;
    const float minPitch = std::asin(minPitchSin);
    const float requestedPitch = pitchDegrees * kRadiansPerDegree;
    const float pitch = requestedPitch < minPitch ? minPitch : requestedPitch;
    const float yaw = yawDegrees * kRadiansPerDegree;
    return {{dist * std::cos(pitch) * std::sin(yaw),
             targetY + dist * std::sin(pitch),
             dist * std::cos(pitch) * std::cos(yaw)},
            {0.0f, targetY, 0.0f}};
}

struct AvatarCullResult {
    bool visible = false;
    int lod = -1;
    int triangleCount = 0;
    int culledInstances = 0;
    bool triangleBudgetExceeded = false; /* true when even the coarsest available LOD exceeds the mobile target */
};

/*
 * Conservative visibility/LOD selection for the one engine-owned avatar. The renderer provides
 * the triangle/index counts of its actual LOD ranges; the adapter never invents a level that the
 * backend cannot draw.
 *
 * Invalid bounds or camera values fail open (draw LOD 0) rather than risk disappearing geometry.
 * The camera/frustum/LOD policy is exercised by the host harness without Android or a GPU.
 */
class AvatarCuller {
public:
    AvatarCuller();
    ~AvatarCuller();

    AvatarCuller(const AvatarCuller&) = delete;
    AvatarCuller& operator=(const AvatarCuller&) = delete;

    AvatarCullResult update(const float modelMin[3], const float modelMax[3], bool hasModel,
                            int indexCount, const CameraState& camera, int width, int height,
                            float quality = 1.0f, float lodDistanceScale = 1.0f);
    AvatarCullResult update(const float modelMin[3], const float modelMax[3], bool hasModel,
                            const int* lodIndexCounts, int lodCount, const CameraState& camera,
                            int width, int height, float quality, float lodDistanceScale = 1.0f);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} /* namespace af */

#endif /* AF_AVATAR_CULLER_H */
