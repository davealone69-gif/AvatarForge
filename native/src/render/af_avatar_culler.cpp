/*
 * af_avatar_culler.cpp -- adapts the current renderer camera/model to the host-tested Scene pass.
 */
#include "af_avatar_culler.h"
#include "../af_scene.h"

#include <algorithm>
#include <cmath>

namespace af {
namespace {

constexpr float kRadiansPerDegree = 0.01745329251994329577f;
constexpr float kAnimationBoundsPadding = 0.15f;
constexpr float kLodDistance1 = 4.5f;
constexpr float kLodDistance2 = 8.0f;

bool finiteCamera(const CameraState& state) {
    return std::isfinite(state.yawDeg) && std::isfinite(state.pitchDeg) &&
           std::isfinite(state.distance) && std::isfinite(state.targetY) &&
           std::isfinite(state.fovDeg) && state.fovDeg > 0.1f && state.fovDeg < 179.0f;
}

} /* namespace */

struct AvatarCuller::Impl {
    Scene scene;
    Visibility visibility;
    int node = -1;
    int geometry = -1;
    int instance = -1;

    Impl() {
        node = scene.addNode("avatar");
        Geometry mesh;
        mesh.name = "avatar";
        mesh.lodCount = 1;
        mesh.lodRanges[0] = {0, 0};
        geometry = scene.addGeometry(mesh);
        instance = scene.addInstance("avatar", geometry, node);
        scene.updateTransforms();
    }
};

AvatarCuller::AvatarCuller() : impl_(new Impl()) {}
AvatarCuller::~AvatarCuller() = default;

AvatarCullResult AvatarCuller::update(const float modelMin[3], const float modelMax[3], bool hasModel,
                                      int indexCount, const CameraState& state, int width, int height,
                                      float quality, float lodDistanceScale) {
    return update(modelMin, modelMax, hasModel, &indexCount, 1, state, width, height,
                  quality, lodDistanceScale);
}

AvatarCullResult AvatarCuller::update(const float modelMin[3], const float modelMax[3], bool hasModel,
                                      const int* lodIndexCounts, int lodCount,
                                      const CameraState& state, int width, int height, float quality,
                                      float lodDistanceScale) {
    AvatarCullResult result;
    if (!impl_ || !hasModel) {
        if (impl_) impl_->scene.instance(impl_->instance).visible = false;
        return result;
    }

    int counts[MAX_LOD_LEVELS] = {};
    int available = std::max(1, std::min(MAX_LOD_LEVELS, lodCount));
    for (int i = 0; i < available; ++i) {
        counts[i] = std::max(0, lodIndexCounts ? lodIndexCounts[i] : 0);
    }
    while (available > 1 && counts[available - 1] == 0) --available;
    auto failOpen = [&]() {
        result.visible = true;
        result.lod = 0;
        result.triangleCount = counts[0] / 3;
        return result;
    };

    if (!modelMin || !modelMax || !finiteCamera(state)) return failOpen();

    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(modelMin[axis]) || !std::isfinite(modelMax[axis]) ||
            modelMin[axis] > modelMax[axis]) {
            return failOpen();
        }
    }

    const Vec3 low(modelMin[0], modelMin[1], modelMin[2]);
    const Vec3 high(modelMax[0], modelMax[1], modelMax[2]);
    const Vec3 centre = (low + high) * 0.5f;
    const Vec3 halfExtent = (high - low) * 0.5f;
    float radius = halfExtent.length();
    if (!std::isfinite(radius)) return failOpen();
    /* Submesh bounds describe the bind mesh. A small margin keeps a skinned pose from being
       clipped at the screen edge when a hand or foot swings beyond that bind-pose AABB. */
    radius += std::max(kAnimationBoundsPadding, radius * 0.08f);

    Geometry& mesh = impl_->scene.geometry(impl_->geometry);
    mesh.bounds = Bounds{centre, radius};
    mesh.vertexCount = 0; /* The renderer draws the engine's shared vertex buffer directly. */
    mesh.lodCount = available;
    mesh.indexCount = static_cast<size_t>(counts[0]);
    for (int level = 0; level < available; ++level) {
        mesh.lodRanges[level] = {0, static_cast<size_t>(counts[level])};
    }
    SceneInstance& avatar = impl_->scene.instance(impl_->instance);
    avatar.localBounds = mesh.bounds;
    avatar.lodCount = available;
    const float distanceScale = std::isfinite(lodDistanceScale)
        ? std::max(0.65f, std::min(1.35f, lodDistanceScale)) : 1.0f;
    avatar.lodDistances[1] = kLodDistance1 * distanceScale;
    avatar.lodDistances[2] = kLodDistance2 * distanceScale;
    avatar.visible = true;

    const float yaw = state.yawDeg * kRadiansPerDegree;
    const float pitch = state.pitchDeg * kRadiansPerDegree;
    const float distance = std::max(state.distance, 0.3f);
    Camera camera;
    camera.position = Vec3(distance * std::cos(pitch) * std::sin(yaw),
                           state.targetY + distance * std::sin(pitch),
                           distance * std::cos(pitch) * std::cos(yaw));
    camera.target = Vec3(0.0f, state.targetY, 0.0f);
    camera.up = Vec3(0.0f, 1.0f, 0.0f);
    camera.fovYRadians = state.fovDeg * kRadiansPerDegree;
    camera.nearPlane = 0.05f;
    camera.farPlane = 60.0f;
    camera.aspect = static_cast<float>(width) / static_cast<float>(height > 0 ? height : 1);

    if (!camera.position.isFinite() || !std::isfinite(camera.fovYRadians) ||
        !std::isfinite(camera.aspect) || camera.aspect <= 0.0f) {
        return failOpen();
    }

    impl_->scene.buildVisibility(camera, std::isfinite(quality) ? quality : 1.0f,
                                 &impl_->visibility);
    result.culledInstances = impl_->visibility.culledInstances;
    result.visible = !impl_->visibility.draws.empty();
    if (result.visible) {
        result.lod = impl_->visibility.draws.front().lod;
        result.triangleCount = impl_->visibility.totalTriangles;

        /* Keep high-poly imported assets inside a predictable mobile triangle budget even when
           the camera is close. Choose the finest available LOD <= 100k triangles; if the
           simplifier cannot reach the budget, choose the coarsest available level. */
        constexpr int kMobileTriangleBudget = 100000;
        if (counts[0] / 3 > kMobileTriangleBudget) {
            int budgetLod = -1;
            for (int level = 0; level < available; ++level) {
                if (counts[level] > 0 && counts[level] / 3 <= kMobileTriangleBudget) {
                    budgetLod = level;
                    break;
                }
            }
            if (budgetLod < 0) budgetLod = available - 1;
            result.lod = budgetLod;
            result.triangleCount = counts[budgetLod] / 3;
        }
    }
    return result;
}

} /* namespace af */
