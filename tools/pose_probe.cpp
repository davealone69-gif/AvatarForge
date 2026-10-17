/*
 * ==============================================================================================
 *  AvatarForge :: tools/pose_probe.cpp
 * ==============================================================================================
 *
 *  Verifies the two properties the viewer depends on, through the public C ABI:
 *
 *    1. composing poses is not accumulative - walk, then REST, then a second walk produce the same
 *       geometry as a single walk (the deformation must always be evaluated from the bind pose);
 *    2. a clip actually moves vertices, and REST restores them exactly.
 *
 *  Build:
 *      tools/build_host.sh kernel
 *      g++ -std=c++17 -O2 -o /tmp/pose_probe tools/pose_probe.cpp -Inative/include \
 *          -Lbuild-host -lafcore -Wl,-rpath,$PWD/build-host && /tmp/pose_probe
 */
#include "afcore.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<af_vertex> snapshot(const af_engine* e) {
    const int32_t n = af_engine_vertex_count(e);
    std::vector<af_vertex> v(static_cast<size_t>(n));
    std::memcpy(v.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
    return v;
}

static size_t moved(const std::vector<af_vertex>& a, const std::vector<af_vertex>& b, float* maxD) {
    size_t count = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const float dx = a[i].pos[0] - b[i].pos[0];
        const float dy = a[i].pos[1] - b[i].pos[1];
        const float dz = a[i].pos[2] - b[i].pos[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > 1e-6f) ++count;
        if (d > worst) worst = d;
    }
    if (maxD) *maxD = worst;
    return count;
}

int main() {
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE; spec.build = AF_BUILD_ATHLETIC; spec.height_m = 1.72f;
    spec.hair_style = AF_HAIR_PONYTAIL; spec.outfit = AF_OUTFIT_TSHIRT; spec.footwear = AF_FOOTWEAR_SNEAKER;

    af_engine* e = nullptr;
    if (af_engine_create(&spec, nullptr, &e) != AF_OK) { std::printf("create failed\n"); return 1; }
    if (af_engine_run_all(e) != AF_OK) { std::printf("run_all failed: %s\n", af_engine_last_error(e)); return 1; }

    const std::vector<af_vertex> rest = snapshot(e);
    std::printf("%d vertices, %d joints\n", af_engine_vertex_count(e), af_engine_joint_count(e));

    float d1 = 0.0f, d2 = 0.0f, d3 = 0.0f;
    af_engine_apply_clip(e, AF_CLIP_WALK, 0.35f);
    const std::vector<af_vertex> walkA = snapshot(e);
    const size_t walkMoved = moved(rest, walkA, &d1);

    af_engine_apply_clip(e, AF_CLIP_REST, 0.0f);
    const std::vector<af_vertex> backToRest = snapshot(e);
    const size_t restLeft = moved(rest, backToRest, &d2);

    af_engine_apply_clip(e, AF_CLIP_WALK, 0.35f);
    const std::vector<af_vertex> walkB = snapshot(e);
    const size_t repeatDiff = moved(walkA, walkB, &d3);

    std::printf("walk         : %zu verts moved, max %.4f m\n", walkMoved, d1);
    std::printf("rest restores: %zu verts still off the bind pose, max %.6f m\n", restLeft, d2);
    std::printf("walk again   : %zu verts differ from the first walk, max %.6f m\n", repeatDiff, d3);

    const bool ok = walkMoved > 1000 && restLeft == 0 && repeatDiff == 0;
    std::printf("%s\n", ok ? "OK: poses compose from the bind pose and REST is exact"
                           : "FAIL: pose state is accumulating");
    af_engine_destroy(e);
    return ok ? 0 : 2;
}
