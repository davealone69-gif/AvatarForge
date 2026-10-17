/*
 * ==============================================================================================
 *  AvatarForge :: tools/shape_probe.cpp
 * ==============================================================================================
 *
 *  Diagnostic: builds the avatar the rig test uses and prints, for every one of the 52
 *  ARKit-compatible blendshape channels, how many vertices it moves and by how much.
 *
 *  This exists because "the blendshape exists" and "the blendshape does something" are different
 *  claims, and the second one is the one that matters. A channel whose mask misses the surface it
 *  is aimed at still produces a morph target - an all-zero one, which exports fine, weighs nothing
 *  and is invisible. The harness ([7]) asserts every channel carries real displacement; this tool
 *  is how that assertion gets debugged when it fails.
 *
 *  Build and run (uses the host kernel library):
 *
 *      tools/build_host.sh kernel
 *      g++ -std=c++17 -O2 -o /tmp/shape_probe tools/shape_probe.cpp \
 *          -Inative/include -Lbuild-host -lafcore -Wl,-rpath,$PWD/build-host
 *      /tmp/shape_probe
 *
 *  Takes ~35 s (one full geometry build at the tested cell size).
 */
#include "af_anatomy.h"
#include "af_body.h"
#include "af_field.h"
#include "af_mesh.h"
#include "af_rig.h"
#include <cstdio>
#include <cstdlib>

using namespace af;

int main(int argc, char** argv) {
    /* The same specification the rig harness uses, so a reading here matches a failing check
       there. `shapes <height>` overrides the stature for spot checks. */
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE;
    spec.build = AF_BUILD_ATHLETIC;
    spec.height_m = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 1.70f;
    spec.muscle = 0.55f;
    spec.body_fat = 0.20f;
    spec.seed = 4242;

    const BodyMetrics m = Anatomy::solve(spec);
    BodyField field(m);
    BodyBuildOptions opt;
    opt.bodyCell = 0.008f;      /* the cell size the harness rigs at                   */
    opt.detailRounds = 2;
    opt.threads = 2;
    Mesh mesh;
    BodyBuildReport report;
    if (!buildBodyMesh(field, opt, &mesh, &report)) {
        std::printf("geometry build failed\n");
        return 1;
    }
    Rig rig;
    if (!rig.build(field, mesh)) {
        std::printf("rig build failed\n");
        return 1;
    }
    std::printf("avatar %.3f m: %zu verts, %d joints, %d channels\n", static_cast<double>(spec.height_m),
                mesh.vertexCount(), rig.jointCount(), kBlendshapeCount);

    int empty = 0, negligible = 0;
    for (int c = 0; c < kBlendshapeCount; ++c) {
        const std::vector<Vec3>& deltas = rig.blendshapeDeltas(c);
        double amplitude = 0.0;
        size_t moved = 0;
        for (const Vec3& d : deltas) {
            const double l = static_cast<double>(d.length());
            if (l > 1e-6) ++moved;
            if (l > amplitude) amplitude = l;
        }
        const bool isEmpty = amplitude < 1e-5;
        const bool isSmall = !isEmpty && amplitude < 1e-3;
        if (isEmpty) ++empty;
        if (isSmall) ++negligible;
        std::printf("  %-18s moved %6zu  max %.4f m%s\n", kBlendshapeNames[c], moved, amplitude,
                    isEmpty ? "   <-- EMPTY" : (isSmall ? "   <-- sub-millimetre" : ""));
    }
    std::printf("%d empty, %d sub-millimetre\n", empty, negligible);
    return empty == 0 ? 0 : 2;
}
