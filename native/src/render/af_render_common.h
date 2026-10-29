/*
 * ============================================================================================
 *  AvatarForge :: af_render_common.h  --  maths, camera, light fitting and material mapping
 * ============================================================================================
 *
 *  Everything here is header-only: one matrix convention, material mapping and UBO layout. The uniform
 *  block structs below are the C++ mirror of the std140 blocks in shaders/common.glsl; a static
 *  assert on each size keeps the two in step (std140 rounds every member up to 16 bytes, which
 *  is why the blocks are written as arrays of vec4 rather than as matrices plus scalars).
 *
 *  Matrix convention matches the kernel: column-major, translation in m[12..14], +Y up, -Z
 *  forward, right-handed, with OpenGL clip depth [-1,1] and +Y up.
 */
#ifndef AF_RENDER_COMMON_H
#define AF_RENDER_COMMON_H

#include <cmath>
#include <cstdint>
#include <cstring>

#include "af_render.h"
#include "af_environment.h"

namespace af {

/* ------------------------------------------------------------------ mat4 ------------------- */
struct Mat4 {
    float m[16];   /* column-major: m[col * 4 + row] */
};

inline Mat4 matIdentity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat4 matMul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline void matMulVec4(const Mat4& a, const float v[4], float out[4]) {
    for (int row = 0; row < 4; ++row)
        out[row] = a.m[0 * 4 + row] * v[0] + a.m[1 * 4 + row] * v[1] +
                   a.m[2 * 4 + row] * v[2] + a.m[3 * 4 + row] * v[3];
}

/** Right-handed look-at; the eye/target/up triplet is the conventional one (forward = -Z). */
inline Mat4 matLookAt(const float eye[3], const float target[3], const float up[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-8f) fl = 1.0f;
    for (int i = 0; i < 3; ++i) f[i] /= fl;
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-8f) { s[0] = 1.0f; s[1] = s[2] = 0.0f; sl = 1.0f; }
    for (int i = 0; i < 3; ++i) s[i] /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};

    Mat4 r{};
    r.m[0] = s[0]; r.m[4] = s[1]; r.m[8]  = s[2];  r.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    r.m[1] = u[0]; r.m[5] = u[1]; r.m[9]  = u[2];  r.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    r.m[2] = -f[0]; r.m[6] = -f[1]; r.m[10] = -f[2]; r.m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    r.m[15] = 1.0f;
    return r;
}

/** OpenGL perspective projection (clip depth [-1,1]). */
inline Mat4 matPerspective(float fovYDeg, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovYDeg * 3.14159265358979f / 360.0f);
    Mat4 r{};
    r.m[0] = f / (aspect > 1e-5f ? aspect : 1.0f);
    r.m[5] = f;
    r.m[10] = -((zfar + znear) / (zfar - znear));
    r.m[11] = -1.0f;
    r.m[14] = -((2.0f * zfar * znear) / (zfar - znear));
    return r;
}

/** Orthographic projection used by the sun's shadow map. */
inline Mat4 matOrtho(float l, float r_, float b, float t, float zn, float zf) {
    Mat4 r{};
    r.m[0] = 2.0f / (r_ - l);
    r.m[5] = 2.0f / (t - b);
    r.m[12] = -(r_ + l) / (r_ - l);
    r.m[13] = -(t + b) / (t - b);
    r.m[10] = -2.0f / (zf - zn);
    r.m[14] = -(zf + zn) / (zf - zn);
    r.m[15] = 1.0f;
    return r;
}

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 vadd(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 vsub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 vscale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float vdot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 vcross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 vnormalize(const Vec3& a) {
    const float l = std::sqrt(vdot(a, a));
    return l > 1e-8f ? vscale(a, 1.0f / l) : Vec3{0, 1, 0};
}

/* ------------------------------------------------------------------ uniform blocks --------- */
/* Mirrors CameraBlock in shaders/common.glsl. */
struct CameraUbo {
    float viewProj[16];
    float view[16];
    float lightViewProj[16];
    float cameraPos[4];
    float sunDir[4];
    float sunColor[4];
    float skyProps[4];
    float envFlags[4];
    float uiParams[4];
    float ground[4];
    float sh[9][4];
};
static_assert(sizeof(CameraUbo) == 448, "CameraUbo must match the std140 layout of CameraBlock");

/* Mirrors MaterialBlock in shaders/common.glsl. 80 bytes of data; the backends pad each entry to
   the device's minimum uniform-buffer offset alignment (256 on every Android GPU we care about)
   so one buffer can hold every submesh's block and be bound with a dynamic offset. */
struct MaterialUbo {
    float baseColorFactor[4];
    float pbr[4];
    float pbr2[4];
    float texFlags[4];
    float region[4];
};
static_assert(sizeof(MaterialUbo) == 80, "MaterialUbo must match MaterialBlock");

/* Mirrors PostBlock in shaders/common.glsl. */
struct PostUbo {
    float params[4];
    float texel[4];
};
static_assert(sizeof(PostUbo) == 32, "PostUbo must match PostBlock");

/* ------------------------------------------------------------------ vertex layout ---------- */
/* Byte offsets into af_vertex. Both backends bind the engine's buffer directly with these. */
struct VertexLayout {
    static constexpr uint32_t stride = sizeof(af_vertex);   /* 88 */
    static constexpr uint32_t posOffset = 0;
    static constexpr uint32_t normalOffset = 12;
    static constexpr uint32_t tangentOffset = 24;
    static constexpr uint32_t uvOffset = 40;
    static constexpr uint32_t colorOffset = 48;
};
static_assert(VertexLayout::stride == 88, "af_vertex must stay 88 bytes for the renderer's stride");

/* Local copies of the shader-side constants, so af_render_common.h does not depend on GLSL. */
enum {
    AF_KIND_GENERIC = 0, AF_KIND_SKIN = 1, AF_KIND_EYE = 2, AF_KIND_HAIR = 3,
    AF_KIND_CLOTH = 4, AF_KIND_METAL = 5, AF_KIND_TEETH = 6, AF_KIND_NAIL = 7
};

/* ------------------------------------------------------------------ material mapping ------- */
/* af_region -> AfSurface kind. The region ids are the kernel's (0..12) and are part of the ABI. */
inline int materialKindForRegion(int region, float metallic) {
    switch (region) {
        case 0:  return AF_KIND_SKIN;    /* skin            */
        case 1:  return AF_KIND_EYE;     /* eye_left        */
        case 2:  return AF_KIND_EYE;     /* eye_right       */
        case 3:  return AF_KIND_TEETH;   /* teeth           */
        case 4:  return AF_KIND_SKIN;    /* tongue: wet tissue, same lobe set as skin */
        case 5:  return AF_KIND_SKIN;    /* lips            */
        case 6:  return AF_KIND_HAIR;    /* hair            */
        case 7:  return AF_KIND_HAIR;    /* lash: same anisotropic strand model */
        case 8:  return AF_KIND_NAIL;    /* nail            */
        case 9:
        case 10: return AF_KIND_CLOTH;   /* cloth_upper / cloth_lower */
        case 11: return AF_KIND_CLOTH;   /* shoe: leather is cloth-like with less sheen */
        case 12: return (metallic > 0.35f) ? AF_KIND_METAL : AF_KIND_CLOTH;   /* gear */
        default: return AF_KIND_GENERIC;
    }
}

/*
 * Iris reference size for the eye shader.
 *
 * The kernel bakes each eye into its own atlas chart (sclera + iris + pupil painted through one
 * uv island). The renderer's eye model refracts the view ray at the cornea and re-samples the iris
 * where the refracted ray lands, so it needs to convert a displacement in metres into a
 * displacement in atlas uv - and the only honest source for that scale is the island itself.
 * Measuring the uv extent of the eye submesh's vertices gives it, exactly, at whatever atlas
 * resolution the projector picked. Returns 0 for non-eye submeshes, which the shader treats as
 * "refraction off".
 */
inline float irisUvExtent(const af_engine* engine, const af_submesh& sm) {
    if (!engine) return 0.0f;
    const af_vertex* v = af_engine_vertices(engine);
    const uint32_t* idx = af_engine_indices(engine);
    if (!v || !idx || sm.index_count <= 0) return 0.0f;
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    for (int32_t i = 0; i < sm.index_count; ++i) {
        const uint32_t vi = idx[sm.index_offset + i];
        const float u = v[vi].uv[0], w = v[vi].uv[1];
        minU = u < minU ? u : minU; maxU = u > maxU ? u : maxU;
        minV = w < minV ? w : minV; maxV = w > maxV ? w : maxV;
    }
    const float du = maxU - minU, dv = maxV - minV;
    const float extent = du > dv ? du : dv;
    /* The chart covers sclera + iris + pupil, so the visible iris is roughly half of it. The shader
       clamps the resulting offset, so a chart that happens to be mostly sclera cannot smear the
       iris across the eye. */
    return extent * 0.5f;
}

/* ------------------------------------------------------------------ scene ------------------ */
/*
 *  Everything the two backends need to know about the current frame, derived from the engine and
 *  the CameraState. Building it in one place is what keeps the backends' images consistent.
 */
struct FrameScene {
    CameraUbo camera;
    Vec3 eye;
    float modelHeight = 1.75f;
    float modelMin[3] = {0, 0, 0};
    float modelMax[3] = {0, 0, 0};
    bool hasModel = false;
    PostUbo post;
    int screenWidth = 1;
    int screenHeight = 1;
};

/* Union of the submesh bounds; cheap, and exact for a mesh that is a union of its submeshes. */
inline void modelBounds(const af_engine* engine, float outMin[3], float outMax[3], bool* has) {
    const int n = af_engine_submesh_count(engine);
    const af_submesh* sm = af_engine_submeshes(engine);
    *has = (n > 0 && sm != nullptr);
    for (int i = 0; i < 3; ++i) { outMin[i] = 1e9f; outMax[i] = -1e9f; }
    if (!*has) { outMin[0] = outMin[1] = outMin[2] = -1.0f; outMax[0] = outMax[1] = outMax[2] = 1.0f; return; }
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            if (sm[i].bounds_min[k] < outMin[k]) outMin[k] = sm[i].bounds_min[k];
            if (sm[i].bounds_max[k] > outMax[k]) outMax[k] = sm[i].bounds_max[k];
        }
}

/* Builds the camera UBO for one frame: orbit camera, sun direction, SH irradiance and flags. */
inline void buildCameraUbo(FrameScene* scene, const CameraState& cam, const RendererOptions& opt,
                           const EnvironmentParams& env, const ShGlobal& sh, int width, int height) {
    const float aspect = float(width) / float(height > 0 ? height : 1);
    const float yaw = cam.yawDeg * 3.14159265358979f / 180.0f;
    const float pitch = cam.pitchDeg * 3.14159265358979f / 180.0f;
    const float dist = cam.distance < 0.3f ? 0.3f : cam.distance;
    const float tx = 0.0f, ty = cam.targetY, tz = 0.0f;
    const Vec3 eye{
        tx + dist * std::cos(pitch) * std::sin(yaw),
        ty + dist * std::sin(pitch),
        tz + dist * std::cos(pitch) * std::cos(yaw)
    };
    const float target[3] = {tx, ty, tz};
    const float eyeArr[3] = {eye.x, eye.y, eye.z};
    const float up[3] = {0.0f, 1.0f, 0.0f};

    const Mat4 proj = matPerspective(cam.fovDeg, aspect, 0.05f, 60.0f);
    const Mat4 view = matLookAt(eyeArr, target, up);
    const Mat4 vp = matMul(proj, view);

    std::memcpy(scene->camera.viewProj, vp.m, sizeof(vp.m));
    std::memcpy(scene->camera.view, view.m, sizeof(view.m));

    /* ---- sun ------------------------------------------------------------------------------ */
    float sd[3];
    env.sunDirection(sd);
    const Vec3 sun{sd[0], sd[1], sd[2]};

    /* ---- shadow light fit ----------------------------------------------------------------- */
    /* A single orthographic map wrapped tightly around the figure: the avatar is under 2.5 m in
       every dimension, so one 2048 map covers it at ~1.2 mm/texel - far more than enough for the
       PCF footprint used here, with no cascade machinery. */
    float centre[3], extent = 1.3f;
    if (scene->hasModel) {
        for (int i = 0; i < 3; ++i) centre[i] = 0.5f * (scene->modelMin[i] + scene->modelMax[i]);
        float ex = 0.0f;
        for (int i = 0; i < 3; ++i) ex = std::max(ex, scene->modelMax[i] - scene->modelMin[i]);
        extent = 0.5f * ex * 1.25f + 0.15f;
    } else {
        centre[0] = 0.0f; centre[1] = 1.0f; centre[2] = 0.0f;
    }
    const Vec3 c{centre[0], centre[1], centre[2]};
    const Vec3 lightEye = vadd(c, vscale(sun, extent * 2.4f));
    const float le[3] = {lightEye.x, lightEye.y, lightEye.z};
    const Vec3 upRef = (std::fabs(sun.y) > 0.98f) ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const float upArr[3] = {upRef.x, upRef.y, upRef.z};
    const Mat4 lightView = matLookAt(le, centre, upArr);
    const Mat4 lightProj = matOrtho(-extent, extent, -extent, extent, 0.05f, extent * 5.0f);
    const Mat4 lightVp = matMul(lightProj, lightView);
    std::memcpy(scene->camera.lightViewProj, lightVp.m, sizeof(lightVp.m));

    /* ---- the rest of the block ------------------------------------------------------------ */
    scene->camera.cameraPos[0] = eye.x; scene->camera.cameraPos[1] = eye.y;
    scene->camera.cameraPos[2] = eye.z; scene->camera.cameraPos[3] = 0.0f;

    scene->camera.sunDir[0] = sun.x; scene->camera.sunDir[1] = sun.y;
    scene->camera.sunDir[2] = sun.z; scene->camera.sunDir[3] = opt.sunIntensity;

    /* A slightly warm sun tint at low elevation, neutral when high: it is what makes the
       late-afternoon preset read as late afternoon rather than as underexposure. */
    const float warm = std::max(0.0f, 1.0f - std::max(0.0f, sun.y) * 1.6f);
    scene->camera.sunColor[0] = 1.00f;
    scene->camera.sunColor[1] = 1.00f - 0.14f * warm;
    scene->camera.sunColor[2] = 1.00f - 0.32f * warm;
    scene->camera.sunColor[3] = 0.0f;

    scene->camera.skyProps[0] = opt.turbidity;
    scene->camera.skyProps[1] = opt.groundAlbedo;
    scene->camera.skyProps[2] = opt.environmentIntensity;
    scene->camera.skyProps[3] = opt.exposure;

    scene->camera.envFlags[0] = opt.enableShadows ? 1.0f : 0.0f;
    scene->camera.envFlags[1] = opt.enableBloom ? 1.0f : 0.0f;
    scene->camera.envFlags[2] = opt.enableSss ? 1.0f : 0.0f;
    scene->camera.envFlags[3] = 0.0f;   /* flat shading, set per draw by the backend */
    for (int i = 0; i < 9; ++i) {
        scene->camera.sh[i][0] = sh.coef[i][0];
        scene->camera.sh[i][1] = sh.coef[i][1];
        scene->camera.sh[i][2] = sh.coef[i][2];
        scene->camera.sh[i][3] = 0.0f;
    }
    scene->camera.ground[0] = 0.0f;
    scene->camera.ground[1] = opt.groundPlane ? 1.0f : 0.0f;
    scene->camera.ground[2] = 0.55f;
    scene->eye = eye;
    scene->screenWidth = width;
    scene->screenHeight = height;
}

/* ------------------------------------------------------------------ picking ---------------- */
inline int pickRegion(const af_engine* engine, const float rayOrigin[3], const float rayDir[3],
                      float* outDistance, float* outPoint) {
    if (!engine) return -1;
    const int vcount = af_engine_vertex_count(engine);
    const int icount = af_engine_index_count(engine);
    const af_vertex* verts = af_engine_vertices(engine);
    const uint32_t* idx = af_engine_indices(engine);
    const af_submesh* sm = af_engine_submeshes(engine);
    const int smCount = af_engine_submesh_count(engine);
    if (!verts || !idx || !sm || vcount <= 0 || icount < 3) return -1;

    const float ox = rayOrigin[0], oy = rayOrigin[1], oz = rayOrigin[2];
    const float dx = rayDir[0], dy = rayDir[1], dz = rayDir[2];
    float best = 1e30f;
    int bestRegion = -1;
    for (int s = 0; s < smCount; ++s) {
        const int i0 = sm[s].index_offset;
        const int i1 = sm[s].index_offset + sm[s].index_count;
        for (int i = i0; i + 2 < i1; i += 3) {
            const af_vertex& a = verts[idx[i]];
            const af_vertex& b = verts[idx[i + 1]];
            const af_vertex& c = verts[idx[i + 2]];
            /* Moller-Trumbore, with a 1e-7 epsilon: the mesh has sliver triangles by design. */
            const float e1x = b.pos[0] - a.pos[0], e1y = b.pos[1] - a.pos[1], e1z = b.pos[2] - a.pos[2];
            const float e2x = c.pos[0] - a.pos[0], e2y = c.pos[1] - a.pos[1], e2z = c.pos[2] - a.pos[2];
            const float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
            const float det = e1x * px + e1y * py + e1z * pz;
            if (det > -1e-9f && det < 1e-9f) continue;
            const float invDet = 1.0f / det;
            const float tx = ox - a.pos[0], ty = oy - a.pos[1], tz = oz - a.pos[2];
            const float u = (tx * px + ty * py + tz * pz) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const float qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
            const float v = (dx * qx + dy * qy + dz * qz) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float t = (e2x * qx + e2y * qy + e2z * qz) * invDet;
            if (t > 1e-5f && t < best) { best = t; bestRegion = sm[s].region; }
        }
    }
    if (bestRegion >= 0) {
        if (outDistance) *outDistance = best;
        if (outPoint) {
            outPoint[0] = ox + dx * best;
            outPoint[1] = oy + dy * best;
            outPoint[2] = oz + dz * best;
        }
    }
    return bestRegion;
}

/* Builds a picking ray through a normalised device position (x,y in -1..1, y up). */
inline void screenRay(const CameraState& cam, float ndcX, float ndcY, int width, int height,
                      float outOrigin[3], float outDir[3]) {
    const float aspect = float(width) / float(height > 0 ? height : 1);
    const float yaw = cam.yawDeg * 3.14159265358979f / 180.0f;
    const float pitch = cam.pitchDeg * 3.14159265358979f / 180.0f;
    const float dist = cam.distance < 0.3f ? 0.3f : cam.distance;
    const Vec3 eye{ dist * std::cos(pitch) * std::sin(yaw),
                    cam.targetY + dist * std::sin(pitch),
                    dist * std::cos(pitch) * std::cos(yaw) };
    const Vec3 fwd = vnormalize(vsub(Vec3{0.0f, cam.targetY, 0.0f}, eye));
    const Vec3 right = vnormalize(vcross(fwd, Vec3{0, 1, 0}));
    const Vec3 up = vcross(right, fwd);
    const float tanHalf = std::tan(cam.fovDeg * 3.14159265358979f / 360.0f);

    outOrigin[0] = eye.x; outOrigin[1] = eye.y; outOrigin[2] = eye.z;
    const Vec3 dir = vnormalize(vadd(vadd(fwd, vscale(right, ndcX * tanHalf * aspect)),
                                     vscale(up, ndcY * tanHalf)));
    outDir[0] = dir.x; outDir[1] = dir.y; outDir[2] = dir.z;
}

} /* namespace af */

#endif /* AF_RENDER_COMMON_H */
