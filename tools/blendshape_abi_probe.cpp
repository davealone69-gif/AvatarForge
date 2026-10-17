/*
 * ==============================================================================================
 *  AvatarForge :: tools/blendshape_abi_probe.cpp
 * ==============================================================================================
 *
 *  Diagnostic: for every one of the 52 ARKit channels, measure the displacement the *exported*
 *  model will carry - i.e. exactly what the .glb writer puts into a morph target.
 *
 *  Why this exists. `shape_probe.cpp` measures the rig's own delta tables through the internal API;
 *  this one goes through the public one (af_engine_apply_blendshapes + af_engine_vertices_rest), so
 *  it sees what a consumer of the exported file sees. The exporter stores each channel as a sparse
 *  accessor and *skips a channel entirely* when every component of its delta is below 5e-5 m
 *  (0.05 mm, af_glb.cpp). A skipped channel is not just a missing morph target: glTF morph targets
 *  are positional, so dropping channel c silently renumbers every channel after it. This tool
 *  prints which channels would be dropped and how much they actually move, which is the difference
 *  between "the file is a bit smaller" and "the file lies about its channel indices".
 *
 *  Build and run (host kernel library):
 *
 *      tools/build_host.sh kernel
 *      g++ -std=c++17 -O2 -o /tmp/blendshape_abi_probe tools/blendshape_abi_probe.cpp \
 *          -Inative/include -Lbuild-host -lafcore -Wl,-rpath,$PWD/build-host
 *      /tmp/blendshape_abi_probe          # ~45 s: one full engine pipeline run
 */
#include "afcore.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE;
    spec.build = AF_BUILD_ATHLETIC;
    spec.height_m = (argc > 1) ? static_cast<float>(std::atof(argv[1])) : 1.72f;
    spec.outfit = AF_OUTFIT_TSHIRT;
    spec.hair_style = AF_HAIR_PONYTAIL;
    spec.footwear = AF_FOOTWEAR_SNEAKER;

    af_engine* e = nullptr;
    if (af_engine_create(&spec, nullptr, &e) != AF_OK) { std::printf("create failed\n"); return 1; }
    if (af_engine_run_all(e) != AF_OK) {
        std::printf("run_all failed: %s\n", af_engine_last_error(e));
        return 1;
    }

    const int32_t n = af_engine_vertex_count(e);
    const int32_t channels = af_engine_blendshape_names(nullptr, 0) > 0 ? af_engine_blendshape_names(nullptr, 0) : 52;
    char names[64][64];
    const int32_t named = af_engine_blendshape_names(names, 64);
    const af_vertex* rest = af_engine_vertices_rest(e);
    if (!rest) { std::printf("no rest buffer through the ABI\n"); return 1; }

    std::printf("avatar %.3f m: %d vertices, %d channels exposed by the ABI\n", static_cast<double>(spec.height_m), n, named);

    /* The exporter's threshold, af_glb.cpp: a channel below this moves less than a tenth of a pixel
       at any sane viewing distance. */
    const float kEps = 5.0e-5f;

    std::vector<float> w(static_cast<size_t>(channels), 0.0f);
    int dropped = 0, weak = 0;
    for (int32_t c = 0; c < named; ++c) {
        for (int32_t k = 0; k < channels; ++k) w[static_cast<size_t>(k)] = 0.0f;
        w[static_cast<size_t>(c)] = 1.0f;
        if (af_engine_apply_blendshapes(e, w.data(), channels) != AF_OK) {
            std::printf("  %-18s apply failed: %s\n", names[c], af_engine_last_error(e));
            continue;
        }
        const af_vertex* moved = af_engine_vertices(e);
        double amplitude = 0.0;
        size_t aboveEps = 0, nonzero = 0;
        for (int32_t i = 0; i < n; ++i) {
            const float dx = moved[i].pos[0] - rest[i].pos[0];
            const float dy = moved[i].pos[1] - rest[i].pos[1];
            const float dz = moved[i].pos[2] - rest[i].pos[2];
            const double l = std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz));
            if (l > 1e-6) ++nonzero;
            if (std::fabs(dx) > kEps || std::fabs(dy) > kEps || std::fabs(dz) > kEps) ++aboveEps;
            if (l > amplitude) amplitude = l;
        }
        const bool wouldDrop = aboveEps == 0;
        if (wouldDrop) ++dropped;
        if (amplitude > 0.0 && amplitude < 1e-3) ++weak;
        std::printf("  %-18s moved %6zu (>=eps %6zu)  max %.5f m%s\n", names[c], nonzero, aboveEps, amplitude,
                    wouldDrop ? "   <-- WOULD BE DROPPED FROM THE .glb" : (amplitude < 1e-3 ? "   <-- sub-millimetre" : ""));
        /* Back to the bind pose for the next channel: weights are per-call, so a zero vector resets
           the face, and this keeps each measurement independent. */
        std::vector<float> zero(static_cast<size_t>(channels), 0.0f);
        af_engine_apply_blendshapes(e, zero.data(), channels);
    }
    std::printf("%d of %d channels move less than %.5f m anywhere (exporter drops these); %d are sub-millimetre\n",
                dropped, named, kEps, weak);
    af_engine_destroy(e);
    return dropped == 0 ? 0 : 2;
}
