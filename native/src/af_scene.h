/*
 * af_scene.h -- the scene the renderer draws, and the CPU work that decides what reaches the GPU.
 *
 * Until now the renderer could draw exactly one thing: the engine's mesh, in bind space, lit by one
 * analytic sun. That is enough for a turntable and not much else, and it is the wrong shape for
 * Phase 2 (LOD, multiple lights with culling) and Phase 4 (GPU-driven rendering), both of which
 * need a scene graph and a per-frame visibility pass to exist first.
 *
 * What this file is:
 *
 *   - a small node graph (parent, children, local TRS, world matrix, cached normal matrix);
 *   - cameras (perspective, with the six frustum planes extracted once per frame);
 *   - lights with a bounded count and per-light culling against the frustum;
 *   - mesh instances referencing an imported or generated geometry, with bounding spheres;
 *   - a `Visibility` pass that produces the per-frame draw list: visible instances, visible lights,
 *     and the LOD level each instance should draw at.
 *
 * What this file is NOT: a render pass. It does no GPU work and includes no graphics headers, so it
 * compiles and is tested on the host against synthetic scenes. A wrong cull or a wrong LOD
 * threshold is a visible bug (a popping arm, a light that vanishes) and this is where it gets
 * caught, before any of it can reach a phone.
 *
 * Conventions are the engine's: +Y up, -Z forward, metres, column-major Mat4 (af_math.h).
 */
#ifndef AF_SCENE_H
#define AF_SCENE_H

#include "af_math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace af {

constexpr int MAX_LIGHTS = 64;       /* per scene; Phase 4 raises this behind a storage buffer */
constexpr int MAX_LOD_LEVELS = 4;

enum LightType : int {
    LIGHT_DIRECTIONAL = 0,
    LIGHT_POINT = 1,
    LIGHT_SPOT = 2,
};

struct Light {
    std::string name;
    int type = LIGHT_DIRECTIONAL;
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    /* Directional: direction the light travels. Point/spot: unused. */
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Vec3 position{0.0f, 0.0f, 0.0f};
    /* Spot cone in radians; ignored by the other types. */
    float innerAngle = 0.35f;
    float outerAngle = 0.52f;
    /* Physical falloff radius for point/spot; a light never affects anything beyond this. */
    float range = 10.0f;
    bool castsShadow = false;
    bool enabled = true;
};

/* A conservative bounding sphere. Cheap to test, and generous by construction: an object outside
 * the sphere is definitely outside the object, which is what makes a cull safe. */
struct Bounds {
    Vec3 centre{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
};

struct Camera {
    Vec3 position{0.0f, 1.0f, 3.0f};
    Vec3 target{0.0f, 0.9f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    float fovYRadians = 0.9f;
    float nearPlane = 0.05f;
    float farPlane = 50.0f;
    float aspect = 9.0f / 16.0f;

    Mat4 view() const;
    Mat4 projection() const;
    Mat4 viewProjection() const { return projection() * view(); }
    Vec3 forward() const;
};

/* The six frustum planes, each as (a, b, c, d) with the inward normal, normalised. */
struct Frustum {
    float planes[6][4];

    static Frustum fromViewProjection(const Mat4& vp);
    /* True when the sphere is at least partially inside. Conservative by one radius. */
    bool intersects(const Vec3& centre, float radius) const;
};

/* One drawable instance: geometry + transform + level of detail state. */
struct SceneInstance {
    std::string name;
    int geometry = -1;          /* index into Scene::geometries                    */
    int node = -1;              /* scene node whose world matrix places this        */
    Bounds localBounds;         /* bounds in geometry space                         */
    float lodDistances[MAX_LOD_LEVELS] = {0.0f, 0.0f, 0.0f, 0.0f};
    int lodCount = 1;           /* LOD 0 always exists; more only when geometry has them */
    bool visible = true;
};

struct Geometry {
    std::string name;
    size_t vertexCount = 0;
    size_t indexCount = 0;
    Bounds bounds;
    /* Per-LOD index ranges into one shared vertex buffer: LOD 0 is the full mesh, higher levels
       are progressively simplified. Only lodRanges[0..lodCount-1] are meaningful. */
    struct LodRange {
        size_t indexOffset = 0;
        size_t indexCount = 0;
    } lodRanges[MAX_LOD_LEVELS];
    int lodCount = 1;
};

/* One entry of the per-frame draw list. */
struct DrawItem {
    int instance = -1;
    int geometry = -1;
    int lod = 0;
    size_t indexOffset = 0;
    size_t indexCount = 0;
    float sortDepth = 0.0f;     /* view-space distance, for transparency sorting */
};

/* One entry of the per-frame light list. */
struct CulledLight {
    int light = -1;
    float distance = 0.0f;      /* for sorting by importance */
};

/* Per-frame visibility result. Reused between frames: the vectors keep their capacity. */
struct Visibility {
    std::vector<DrawItem> draws;
    std::vector<CulledLight> lights;
    int culledInstances = 0;
    int culledLights = 0;
    int totalTriangles = 0;     /* after LOD selection, before the GPU's own culling */

    void clear() {
        draws.clear();
        lights.clear();
        culledInstances = 0;
        culledLights = 0;
        totalTriangles = 0;
    }
};

struct SceneNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    Mat4 local = Mat4::identity();
    Mat4 world = Mat4::identity();

    /* Rebuilds `local` from TRS. Call after changing any of them. */
    void updateLocal() { local = Mat4::trs(translation, rotation, scale); }
};

/**
 * The scene. Owns nodes, geometries, instances and lights; produces a visibility list for one
 * camera. Not thread-safe by design: one scene belongs to the render thread, and the app hands it
 * finished assets (that is the job boundary Phase 3 will widen).
 */
class Scene {
public:
    /* ---- construction ---- */
    int addNode(const std::string& name, int parent = -1);
    int addGeometry(Geometry geometry);
    int addInstance(const std::string& name, int geometry, int node);
    int addLight(const Light& light);

    SceneNode& node(int index) { return nodes_[static_cast<size_t>(index)]; }
    const SceneNode& node(int index) const { return nodes_[static_cast<size_t>(index)]; }
    Geometry& geometry(int index) { return geometries_[static_cast<size_t>(index)]; }
    const Geometry& geometry(int index) const { return geometries_[static_cast<size_t>(index)]; }
    SceneInstance& instance(int index) { return instances_[static_cast<size_t>(index)]; }
    const SceneInstance& instance(int index) const { return instances_[static_cast<size_t>(index)]; }
    Light& light(int index) { return lights_[static_cast<size_t>(index)]; }
    const Light& light(int index) const { return lights_[static_cast<size_t>(index)]; }

    size_t nodeCount() const { return nodes_.size(); }
    size_t geometryCount() const { return geometries_.size(); }
    size_t instanceCount() const { return instances_.size(); }
    size_t lightCount() const { return lights_.size(); }

    void clear();

    /** Recomputes every world matrix, parents before children. Detects cycles and returns false
     *  rather than looping; the depth cap makes a malformed graph a clean error. */
    bool updateTransforms();

    /**
     * Fills `out` with everything visible from `camera` at `quality` in [0,1] (1 = highest LOD).
     *
     * Instance culling is sphere-versus-frustum. Light culling is by type: a directional light is
     * always visible, a point or spot light is tested as a sphere of its range, so a light that
     * cannot reach any visible geometry is dropped from the per-frame list. Sorted so that the
     * most significant lights come first, which is what makes a capped light list (Phase 2's
     * Forward+ tile list) stable instead of flickering.
     */
    void buildVisibility(const Camera& camera, float quality, Visibility* out) const;

    /** World-space bounds of an instance, from its geometry bounds and node transform. */
    Bounds instanceWorldBounds(int instance) const;

    int maxLodLevels() const { return MAX_LOD_LEVELS; }

private:
    std::vector<SceneNode> nodes_;
    std::vector<Geometry> geometries_;
    std::vector<SceneInstance> instances_;
    std::vector<Light> lights_;
};

} /* namespace af */
#endif /* AF_SCENE_H */
