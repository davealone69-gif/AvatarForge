/*
 * af_avatar_culler.h -- bridge between the renderer's camera contract and the tested scene layer.
 *
 * The renderer backends share CameraState but their UBO maths lives in af_render_common.h, which
 * deliberately has its own small vector types. Keep the scene graph behind this PIMPL so the two
 * maths headers never collide in a renderer translation unit.
 */
#ifndef AF_AVATAR_CULLER_H
#define AF_AVATAR_CULLER_H

#include "af_render.h"

#include <memory>

namespace af {

struct AvatarCullResult {
    bool visible = false;
    int lod = -1;
    int triangleCount = 0;
    int culledInstances = 0;
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
