/*
 * tools/export_probe.cpp -- writes one .glb through the public C ABI and stops.
 *
 * Used by tools/check_export.sh: it is the smallest thing that proves the export path end to end
 * (create engine -> run every stage -> export). The output is validated with validate_glb.py.
 */
#include "afcore.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstring>
#include <chrono>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/tmp/export_probe.glb";
    af_spec spec;
    af_spec_init(&spec);
    spec.struct_size = sizeof(af_spec);
    spec.height_m = 1.72f;
    spec.sex = AF_SEX_FEMALE;
    spec.outfit = AF_OUTFIT_TSHIRT;
    spec.hair_style = AF_HAIR_PONYTAIL;
    spec.footwear = AF_FOOTWEAR_SNEAKER;
    std::snprintf(spec.prompt, sizeof(spec.prompt), "1.72 m woman, ponytail, t-shirt");

    af_engine* engine = nullptr;
    if (af_engine_create(&spec, nullptr, &engine) != AF_OK) {
        std::printf("create failed: %s\n", af_engine_last_error(engine));
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int32_t rc = af_engine_run_all(engine);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (rc != AF_OK) {
        std::printf("run_all failed (%d): %s\n", rc, af_engine_last_error(engine));
        return 1;
    }
    af_stats stats{};
    af_engine_stats(engine, &stats);
    std::printf("pipeline %.0f ms: %d verts, %d tris, %d submeshes, %d joints, %d blendshapes\n",
                ms, stats.vertex_count, stats.triangle_count, stats.submesh_count, stats.joint_count,
                stats.blendshape_count);
    if (af_engine_export_glb(engine, path) != AF_OK) {
        std::printf("export failed: %s\n", af_engine_last_error(engine));
        return 1;
    }
    uint8_t* bytes = nullptr;
    size_t size = 0;
    if (af_engine_export_glb_to_memory(engine, &bytes, &size) != AF_OK) {
        std::printf("memory export failed: %s\n", af_engine_last_error(engine));
        return 1;
    }
    /*
     * The two export paths have to agree: `af_engine_export_glb` writes a file and
     * `af_engine_export_glb_to_memory` returns the same container without touching the disk. They
     * are separate code paths (one streams to a FILE*, one grows a vector), so this compares the
     * bytes instead of asserting it - a caller that embeds the in-memory model in an APK asset and
     * the user who shares the exported file must not end up with two different avatars.
     */
    size_t fileSize = 0;
    bool identical = false;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len > 0 && static_cast<size_t>(len) == size) {
            std::vector<uint8_t> disk(static_cast<size_t>(len));
            fileSize = std::fread(disk.data(), 1, disk.size(), f);
            identical = (fileSize == size) && (std::memcmp(disk.data(), bytes, size) == 0);
        }
        std::fclose(f);
    }
    std::printf("exported %s (%zu bytes on disk) and %zu bytes in memory; byte-identical: %s\n",
                path, fileSize, size, identical ? "yes" : "NO");
    af_arena_free(bytes);
    af_engine_destroy(engine);
    return 0;
}
