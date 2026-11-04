/* Focused host regression for the shared renderer camera math.
 * Build with:
 *   g++ -std=c++17 -Wall -Wextra -Werror -I native/include -I native/src -I native/src/render \
 *       native/tests/render/camera_math_tests.cpp -o /tmp/af-camera-math-tests
 *   /tmp/af-camera-math-tests
 */
#include "../../src/render/af_render_common.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}
af::Mat4 matrixFrom(const float values[16]) {
    af::Mat4 result{};
    std::memcpy(result.m, values, sizeof(result.m));
    return result;
}
af::FrameScene buildFrame(const af::CameraState& camera, const af::RendererOptions& options,
                          const af::EnvironmentParams& environment, const af::ShGlobal& irradiance) {
    af::FrameScene frame;
    af::buildCameraUbo(&frame, camera, options, environment, irradiance, 480, 900);
    return frame;
}
void checkTargetStillCentered(const af::FrameScene& frame, const af::CameraState& camera) {
    const af::EffectiveOrbitCamera orbit = af::effectiveOrbitCamera(camera);
    const af::Mat4 view = matrixFrom(frame.camera.view);
    const af::Mat4 viewProjection = matrixFrom(frame.camera.viewProj);
    const float target[4] = {orbit.target[0], orbit.target[1], orbit.target[2], 1.0f};
    const float dx = orbit.target[0] - orbit.eye[0];
    const float dy = orbit.target[1] - orbit.eye[1];
    const float dz = orbit.target[2] - orbit.eye[2];
    const float orbitDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
    float viewTarget[4]{};
    float clipTarget[4]{};
    af::matMulVec4(view, target, viewTarget);
    af::matMulVec4(viewProjection, target, clipTarget);
    check(near(viewTarget[0], 0.0f) && near(viewTarget[1], 0.0f) &&
              near(viewTarget[2], -orbitDistance),
          "right-handed look-at keeps the sanitized orbit target straight ahead");
    check(near(clipTarget[0], 0.0f) && near(clipTarget[1], 0.0f) && clipTarget[3] > 0.0f,
          "the target projects to the GLES viewport center with positive clip w");
    check(clipTarget[2] >= -clipTarget[3] && clipTarget[2] <= clipTarget[3],
          "the target lies within GLES homogeneous depth clipping");
}
void checkPickingMatchesFrame(const af::CameraState& camera, const af::FrameScene& frame) {
    float rayOrigin[3]{};
    float rayDirection[3]{};
    af::screenRay(camera, 0.0f, 0.0f, 480, 900, rayOrigin, rayDirection);
    check(near(rayOrigin[0], frame.camera.cameraPos[0]) &&
              near(rayOrigin[1], frame.camera.cameraPos[1]) &&
              near(rayOrigin[2], frame.camera.cameraPos[2]),
          "center picking-ray origin matches the effective UBO camera position");
    const af::EffectiveOrbitCamera orbit = af::effectiveOrbitCamera(camera);
    const float toTarget[3] = {orbit.target[0] - rayOrigin[0], orbit.target[1] - rayOrigin[1],
                               orbit.target[2] - rayOrigin[2]};
    const float targetLength = std::sqrt(toTarget[0] * toTarget[0] + toTarget[1] * toTarget[1] +
                                         toTarget[2] * toTarget[2]);
    check(targetLength > 0.0f && near(rayDirection[0], toTarget[0] / targetLength) &&
              near(rayDirection[1], toTarget[1] / targetLength) &&
              near(rayDirection[2], toTarget[2] / targetLength),
          "center picking ray points toward the sanitized orbit target");
}
} // namespace

int main() {
    af::RendererOptions options;
    af::EnvironmentParams environment;
    af::ShGlobal irradiance{};

    /* Regression: this exact reported orbit used to put the eye below y=0. */
    af::CameraState unsafe;
    unsafe.yawDeg = 180.0f;
    unsafe.pitchDeg = -29.7f;
    unsafe.distance = 3.5f;
    unsafe.targetY = 0.95f;
    unsafe.fovDeg = 32.0f;
    const af::FrameScene guarded = buildFrame(unsafe, options, environment, irradiance);
    const af::EffectiveOrbitCamera sharedPose = af::effectiveOrbitCamera(unsafe);
    check(near(sharedPose.eye[0], guarded.camera.cameraPos[0]) &&
              near(sharedPose.eye[1], guarded.camera.cameraPos[1]) &&
              near(sharedPose.eye[2], guarded.camera.cameraPos[2]) &&
              near(sharedPose.target[1], unsafe.targetY),
          "shared orbit pose matches the UBO camera and target");
    constexpr float kExpectedEyeHeight = 0.05f;
    check(near(guarded.camera.cameraPos[1], kExpectedEyeHeight),
          "targetY=0.95, distance=3.5, pitch=-29.7 clamps eyeY to 5 cm clearance");
    check(guarded.camera.cameraPos[1] >= kExpectedEyeHeight - 1e-4f,
          "the clamped camera eye stays above the avatar ground plane");
    check(near(guarded.camera.ground[0], 0.0f),
          "the fix preserves the contact plane at y=0");
    checkTargetStillCentered(guarded, unsafe);
    checkPickingMatchesFrame(unsafe, guarded);

    /* Supported pitch is explicitly limited to [-80, 80] degrees. */
    af::CameraState oversizedPitch = unsafe;
    oversizedPitch.pitchDeg = 210.0f;
    const af::FrameScene boundedPitch = buildFrame(oversizedPitch, options, environment, irradiance);
    const float maxPitchRadians = 80.0f * 3.14159265358979f / 180.0f;
    check(near(boundedPitch.camera.cameraPos[1],
               oversizedPitch.targetY + oversizedPitch.distance * std::sin(maxPitchRadians)) &&
              near(boundedPitch.camera.cameraPos[2],
                   oversizedPitch.distance * std::cos(maxPitchRadians) * -1.0f),
          "a finite 210-degree pitch clamps to the supported +80-degree boundary");
    check(boundedPitch.camera.cameraPos[1] >= kExpectedEyeHeight,
          "out-of-range positive pitch remains above ground");
    checkTargetStillCentered(boundedPitch, oversizedPitch);
    checkPickingMatchesFrame(oversizedPitch, boundedPitch);

    /* Unreachable target/distance pairs fall back to the documented default pose. */
    af::CameraState unreachable = unsafe;
    unreachable.yawDeg = 230.0f;
    unreachable.pitchDeg = -80.0f;
    unreachable.distance = 0.1f;
    unreachable.targetY = -0.25f;
    const af::FrameScene fallback = buildFrame(unreachable, options, environment, irradiance);
    const af::EffectiveOrbitCamera fallbackPose = af::effectiveOrbitCamera(unreachable);
    const float defaultPitchRadians = 4.0f * 3.14159265358979f / 180.0f;
    check(near(fallbackPose.target[1], 0.95f) &&
              near(fallback.camera.cameraPos[0], 0.0f) &&
              near(fallback.camera.cameraPos[1], 0.95f + 3.5f * std::sin(defaultPitchRadians)) &&
              near(fallback.camera.cameraPos[2], -3.5f * std::cos(defaultPitchRadians)),
          "an unreachable eye uses the complete documented default orbit pose");
    checkTargetStillCentered(fallback, unreachable);
    checkPickingMatchesFrame(unreachable, fallback);

    /* Minimum supported distance is 0.3 m; the floor rule remains satisfiable there. */
    af::CameraState minimumDistance = unsafe;
    minimumDistance.pitchDeg = -80.0f;
    minimumDistance.distance = 0.1f;
    minimumDistance.targetY = 0.0f;
    const af::FrameScene minimumFrame = buildFrame(minimumDistance, options, environment, irradiance);
    const af::EffectiveOrbitCamera minimumPose = af::effectiveOrbitCamera(minimumDistance);
    check(near(minimumPose.eye[1], kExpectedEyeHeight) &&
              near(std::sqrt(minimumPose.eye[0] * minimumPose.eye[0] + minimumPose.eye[2] * minimumPose.eye[2]),
                   std::sqrt(0.3f * 0.3f - kExpectedEyeHeight * kExpectedEyeHeight)),
          "below-minimum distance is bounded to 0.3 m and still keeps the eye above ground");
    checkTargetStillCentered(minimumFrame, minimumDistance);
    checkPickingMatchesFrame(minimumDistance, minimumFrame);

    /* Safe negative pitch still behaves exactly as requested. */
    af::CameraState safeNegative = unsafe;
    safeNegative.pitchDeg = -10.0f;
    const af::FrameScene safeFrame = buildFrame(safeNegative, options, environment, irradiance);
    const float safeExpectedEyeY = safeNegative.targetY + safeNegative.distance *
        std::sin(safeNegative.pitchDeg * 3.14159265358979f / 180.0f);
    check(near(safeFrame.camera.cameraPos[1], safeExpectedEyeY),
          "a safe negative pitch is not altered by the lower-bound guard");
    checkTargetStillCentered(safeFrame, safeNegative);
    checkPickingMatchesFrame(safeNegative, safeFrame);

    /* The normal default camera remains unchanged under the safety guards. */
    af::CameraState normal;
    const af::FrameScene normalFrame = buildFrame(normal, options, environment, irradiance);
    const float normalExpectedEyeY = normal.targetY + normal.distance *
        std::sin(normal.pitchDeg * 3.14159265358979f / 180.0f);
    check(near(normalFrame.camera.cameraPos[1], normalExpectedEyeY),
          "normal default camera height is unchanged");
    check(near(normalFrame.camera.ground[0], 0.0f),
          "normal default camera keeps the contact plane at y=0");
    checkTargetStillCentered(normalFrame, normal);
    checkPickingMatchesFrame(normal, normalFrame);

    std::printf("camera math: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
