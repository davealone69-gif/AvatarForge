/*
 * af_scene.cpp -- scene graph, frustum culling and LOD selection.
 *
 * The two decisions here that a user can see, and the reasoning behind each:
 *
 * 1. CULLING IS CONSERVATIVE. An instance is drawn when its bounding sphere intersects the
 *    frustum, even if the mesh inside it is entirely outside. The alternative - testing the real
 *    geometry - costs more than it saves at these counts, and the failure mode of a too-tight test
 *    is a limb that disappears at the screen edge, which is far worse than drawing a triangle.
 *
 * 2. LOD SELECTION IS HYSTERESIS-FREE AND MONOTONE IN DISTANCE. Level = the number of thresholds
 *    the camera distance exceeds, scaled by the quality setting. Distance is measured from the
 *    camera to the instance's bounding-sphere centre, not to its origin, so an object whose origin
 *    is behind the camera but whose body is in front does not flip LOD. There is deliberately no
 *    hysteresis: a hysteresis band needs per-instance state that has to be reset when the camera
 *    jumps (a cut, a teleport), and getting that reset wrong produces a stuck low-detail model.
 *    Phase 2 can add it behind a measurement.
 */
#include "af_scene.h"

#include <cmath>
#include <cstring>

namespace af {
namespace {

constexpr int MAX_NODE_DEPTH = 256;

void normalisePlane(float p[4]) {
    const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (len < 1e-8f) return;
    const float inv = 1.0f / len;
    for (int i = 0; i < 4; ++i) p[i] *= inv;
}

} /* namespace */

/* ----------------------------------------------------------------------------- Camera ------- */
Vec3 Camera::forward() const {
    return (target - position).normalizedOr({0.0f, 0.0f, -1.0f});
}

Mat4 Camera::view() const {
    /* Right-handed look-at in the engine's frame: +Y up, camera looks down -Z. Written out here
       rather than shared with the renderer so the scene can be exercised without linking it. */
    const Vec3 f = forward();
    Vec3 s = f.cross(up);
    if (s.lengthSq() < 1e-10f) {
        /* Camera looking straight up or down: pick any perpendicular axis rather than producing a
           degenerate view matrix and a black screen. */
        s = f.cross(Vec3(0.0f, 0.0f, 1.0f));
        if (s.lengthSq() < 1e-10f) s = Vec3(1.0f, 0.0f, 0.0f);
    }
    s = s.normalizedOr({1.0f, 0.0f, 0.0f});
    const Vec3 u = s.cross(f);
    Mat4 m = Mat4::identity();
    m.m[0] = s.x;  m.m[4] = s.y;  m.m[8]  = s.z;
    m.m[1] = u.x;  m.m[5] = u.y;  m.m[9]  = u.z;
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
    m.m[12] = -s.dot(position);
    m.m[13] = -u.dot(position);
    m.m[14] = f.dot(position);
    return m;
}

Mat4 Camera::projection() const {
    /* Right-handed projection into clip space with z in [-1, 1]: the engine's matrices follow the
       convention its shaders use, and the renderer adapts to the backend's depth range. */
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    const float aspect = this->aspect > 1e-4f ? this->aspect : 1e-4f;
    const float n = nearPlane, far = farPlane;
    Mat4 m;
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[10] = (far + n) / (n - far);
    m.m[11] = -1.0f;
    m.m[14] = (2.0f * far * n) / (n - far);
    return m;
}

/* ---------------------------------------------------------------------------- Frustum ------- */
Frustum Frustum::fromViewProjection(const Mat4& vp) {
    Frustum fr;
    const float* m = vp.m;
    /* Gribb-Hartmann: the planes are sums and differences of the matrix rows, given a
       column-major matrix whose rows are (m[0], m[4], m[8], m[12]) etc. */
    auto row = [&](int r, float out[4]) {
        out[0] = m[r];
        out[1] = m[4 + r];
        out[2] = m[8 + r];
        out[3] = m[12 + r];
    };
    float r0[4], r1[4], r2[4], r3[4];
    row(0, r0);
    row(1, r1);
    row(2, r2);
    row(3, r3);
    for (int i = 0; i < 4; ++i) {
        fr.planes[0][i] = r3[i] + r0[i];   /* left   */
        fr.planes[1][i] = r3[i] - r0[i];   /* right  */
        fr.planes[2][i] = r3[i] + r1[i];   /* bottom */
        fr.planes[3][i] = r3[i] - r1[i];   /* top    */
        fr.planes[4][i] = r3[i] + r2[i];   /* near   */
        fr.planes[5][i] = r3[i] - r2[i];   /* far    */
    }
    for (int p = 0; p < 6; ++p) normalisePlane(fr.planes[p]);
    return fr;
}

bool Frustum::intersects(const Vec3& centre, float radius) const {
    for (int p = 0; p < 6; ++p) {
        const float d = planes[p][0] * centre.x + planes[p][1] * centre.y + planes[p][2] * centre.z +
                        planes[p][3];
        if (d < -radius) return false;   /* fully outside this plane => fully outside the frustum */
    }
    return true;
}

/* ------------------------------------------------------------------------------ Scene ------- */
int Scene::addNode(const std::string& name, int parent) {
    SceneNode n;
    n.name = name;
    n.parent = -1;
    const int index = static_cast<int>(nodes_.size());
    nodes_.push_back(n);
    if (parent >= 0 && static_cast<size_t>(parent) < nodes_.size() - 1) {
        nodes_[static_cast<size_t>(index)].parent = parent;
        nodes_[static_cast<size_t>(parent)].children.push_back(index);
    }
    return index;
}

int Scene::addGeometry(Geometry geometry) {
    geometries_.push_back(std::move(geometry));
    return static_cast<int>(geometries_.size()) - 1;
}

int Scene::addInstance(const std::string& name, int geometryIndex, int nodeIndex) {
    SceneInstance inst;
    inst.name = name;
    inst.geometry = geometryIndex;
    inst.node = nodeIndex;
    if (geometryIndex >= 0 && static_cast<size_t>(geometryIndex) < geometries_.size()) {
        inst.localBounds = geometries_[static_cast<size_t>(geometryIndex)].bounds;
        inst.lodCount = geometries_[static_cast<size_t>(geometryIndex)].lodCount;
    }
    instances_.push_back(std::move(inst));
    return static_cast<int>(instances_.size()) - 1;
}

int Scene::addLight(const Light& light) {
    if (lights_.size() >= MAX_LIGHTS) return -1;
    lights_.push_back(light);
    return static_cast<int>(lights_.size()) - 1;
}

void Scene::clear() {
    nodes_.clear();
    geometries_.clear();
    instances_.clear();
    lights_.clear();
}

bool Scene::updateTransforms() {
    const size_t n = nodes_.size();
    if (n == 0) return true;
    std::vector<uint8_t> state(n, 0);   /* 0 unvisited, 1 in progress, 2 done */
    std::vector<int> chain;
    for (size_t start = 0; start < n; ++start) {
        if (state[start] == 2) continue;
        chain.clear();
        int cur = static_cast<int>(start);
        int hops = 0;
        while (cur >= 0 && state[static_cast<size_t>(cur)] == 0) {
            state[static_cast<size_t>(cur)] = 1;
            chain.push_back(cur);
            cur = nodes_[static_cast<size_t>(cur)].parent;
            if (++hops > MAX_NODE_DEPTH) return false;
        }
        if (cur >= 0 && state[static_cast<size_t>(cur)] == 1) return false;   /* cycle */
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            SceneNode& node = nodes_[static_cast<size_t>(*it)];
            node.local = Mat4::trs(node.translation, node.rotation, node.scale);
            if (node.parent >= 0) {
                node.world = nodes_[static_cast<size_t>(node.parent)].world * node.local;
            } else {
                node.world = node.local;
            }
            state[static_cast<size_t>(*it)] = 2;
        }
    }
    return true;
}

Bounds Scene::instanceWorldBounds(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= instances_.size()) return Bounds{};
    const SceneInstance& inst = instances_[static_cast<size_t>(index)];
    Bounds b = inst.localBounds;
    if (inst.node < 0 || static_cast<size_t>(inst.node) >= nodes_.size()) return b;
    const Mat4& world = nodes_[static_cast<size_t>(inst.node)].world;
    const Vec3 c = world.transformPoint(b.centre);
    /* Scale the radius by the largest axis scale, so a non-uniformly scaled node cannot produce a
       sphere that is too small (which would cull visible geometry). */
    const float sx = std::sqrt(world.m[0] * world.m[0] + world.m[1] * world.m[1] + world.m[2] * world.m[2]);
    const float sy = std::sqrt(world.m[4] * world.m[4] + world.m[5] * world.m[5] + world.m[6] * world.m[6]);
    const float sz = std::sqrt(world.m[8] * world.m[8] + world.m[9] * world.m[9] + world.m[10] * world.m[10]);
    const float s = sx > sy ? (sx > sz ? sx : sz) : (sy > sz ? sy : sz);
    return Bounds{c, b.radius * s};
}

void Scene::buildVisibility(const Camera& camera, float quality, Visibility* out) const {
    if (!out) return;
    out->clear();
    if (quality < 0.0f) quality = 0.0f;
    if (quality > 1.0f) quality = 1.0f;

    const Mat4 vp = camera.viewProjection();
    const Frustum frustum = Frustum::fromViewProjection(vp);
    const Vec3 eye = camera.position;

    for (size_t i = 0; i < instances_.size(); ++i) {
        const SceneInstance& inst = instances_[i];
        if (!inst.visible) continue;
        if (inst.geometry < 0 || static_cast<size_t>(inst.geometry) >= geometries_.size()) continue;
        const Bounds world = instanceWorldBounds(static_cast<int>(i));
        if (!frustum.intersects(world.centre, world.radius)) {
            ++out->culledInstances;
            continue;
        }
        const Geometry& geo = geometries_[static_cast<size_t>(inst.geometry)];
        const float distance = (world.centre - eye).length();

        /* LOD: the level is how many of the instance's distance thresholds the camera has passed,
           with quality scaling the thresholds. Lower quality pulls every threshold CLOSER, so the
           same object drops to a coarser level sooner - that is the direction the thermal
           controller needs in Phase 3 (and the direction the first implementation had backwards,
           which a test caught by asserting that quality 0 never picks a finer level than
           quality 1). */
        int lod = 0;
        const int available = inst.lodCount < geo.lodCount ? inst.lodCount : geo.lodCount;
        const float qualityScale = 0.4f + 0.6f * quality;
        for (int level = 1; level < available; ++level) {
            const float threshold = inst.lodDistances[level] * qualityScale;
            if (threshold > 0.0f && distance > threshold) lod = level;
        }
        if (lod >= available) lod = available > 0 ? available - 1 : 0;

        DrawItem item;
        item.instance = static_cast<int>(i);
        item.geometry = inst.geometry;
        item.lod = lod;
        item.indexOffset = geo.lodRanges[lod].indexOffset;
        item.indexCount = geo.lodRanges[lod].indexCount;
        item.sortDepth = distance;
        out->totalTriangles += static_cast<int>(item.indexCount / 3);
        out->draws.push_back(item);
    }

    for (size_t i = 0; i < lights_.size(); ++i) {
        const Light& l = lights_[i];
        if (!l.enabled) continue;
        if (l.type == LIGHT_DIRECTIONAL) {
            /* A directional light reaches everything; its "distance" is 0 so it sorts first. */
            out->lights.push_back(CulledLight{static_cast<int>(i), 0.0f});
            continue;
        }
        const float distToEye = (l.position - eye).length();
        const bool inFrustum = frustum.intersects(l.position, l.range);
        if (!inFrustum) {
            ++out->culledLights;
            continue;
        }
        out->lights.push_back(CulledLight{static_cast<int>(i), distToEye});
    }
    /* Most significant first: directional lights, then the nearest point/spot lights. A stable
       insertion sort keeps the order deterministic for equal distances, so a capped light list
       does not flicker between frames. */
    for (size_t i = 1; i < out->lights.size(); ++i) {
        CulledLight key = out->lights[i];
        size_t j = i;
        while (j > 0 && out->lights[j - 1].distance > key.distance) {
            out->lights[j] = out->lights[j - 1];
            --j;
        }
        out->lights[j] = key;
    }
}

} /* namespace af */
