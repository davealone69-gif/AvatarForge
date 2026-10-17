/*
 * afgen -- command-line avatar generator.
 *
 * This tool exists for three reasons, all of them practical:
 *   1. it is a working example of the public C ABI (it links nothing but afcore.h and libafcore),
 *      so the Kotlin SDK and the Rust crates have a reference implementation to compare against;
 *   2. it regenerates the sample assets in out/ from a single command, which is how the checked-in
 *      avatars and the validator run in CI;
 *   3. it makes the kernel testable on a desktop where a phone is not available - the same code
 *      path that runs in the app, with the renderer swapped for a file.
 *
 * Usage:
 *   afgen --prompt "athletic woman, 1.70 m, long hair, blue t-shirt, sneakers" --out out/a.glb
 *   afgen --preset armour --out out/knight.glb --texture-size 2048
 *   afgen --preset bare --out out/base.glb --no-textures --cell 0.008 --stats
 *
 * Exit codes: 0 success, 1 generation failed, 2 bad arguments.
 */
#include "../native/include/afcore.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string prompt;
    std::string preset;
    std::string out = "avatar.glb";
    bool noTextures = false;
    bool stats = false;
    bool verbose = false;
};

void usage() {
    std::printf(
        "afgen -- AvatarForge on-device avatar generator (CLI)\n\n"
        "  --prompt <text>      generate from a text prompt\n"
        "  --preset <name>      bare | athletic | knight | suit | dress | spacesuit | child | tall\n"
        "  --out <path.glb>     output file (default avatar.glb)\n"
        "  --no-textures        skip the texture bake (geometry-only export, much faster)\n"
        "  --stats              print stage timings and buffer sizes\n"
        "  --verbose            print every engine log line\n");
}

int applyPreset(const std::string& name, af_spec* spec) {
    af_spec_init(spec);
    if (name == "bare") { spec->prompt[0] = '\0'; }
    else if (name == "athletic") {
        spec->sex = AF_SEX_FEMALE; spec->build = AF_BUILD_ATHLETIC; spec->height_m = 1.70f;
        spec->outfit = AF_OUTFIT_ATHLETIC; spec->footwear = AF_FOOTWEAR_SNEAKER; spec->hair_style = AF_HAIR_PONYTAIL;
        spec->muscle = 0.6f; spec->body_fat = 0.18f;
    } else if (name == "knight") {
        spec->sex = AF_SEX_MALE; spec->build = AF_BUILD_MUSCULAR; spec->height_m = 1.86f;
        spec->outfit = AF_OUTFIT_ARMOR_PLATE; spec->footwear = AF_FOOTWEAR_SABATON; spec->hair_style = AF_HAIR_SHORT;
        spec->muscle = 0.8f; spec->body_fat = 0.20f; spec->gear_metalness = 0.9f;
        spec->seed = 20260101;
    } else if (name == "suit") {
        spec->sex = AF_SEX_MALE; spec->build = AF_BUILD_AVERAGE; spec->height_m = 1.80f;
        spec->outfit = AF_OUTFIT_SUIT; spec->footwear = AF_FOOTWEAR_BOOT; spec->hair_style = AF_HAIR_SHORT;
        spec->cloth_tone[0] = 0.02f; spec->cloth_tone[1] = 0.02f; spec->cloth_tone[2] = 0.03f;
        spec->seed = 77;
    } else if (name == "dress") {
        spec->sex = AF_SEX_FEMALE; spec->build = AF_BUILD_SLIM; spec->height_m = 1.75f;
        spec->outfit = AF_OUTFIT_DRESS; spec->footwear = AF_FOOTWEAR_HEEL; spec->hair_style = AF_HAIR_LONG;
        spec->cloth_tone[0] = 0.42f; spec->cloth_tone[1] = 0.05f; spec->cloth_tone[2] = 0.08f;
        spec->seed = 909;
    } else if (name == "spacesuit") {
        spec->sex = AF_SEX_FEMALE; spec->build = AF_BUILD_AVERAGE; spec->height_m = 1.68f;
        spec->outfit = AF_OUTFIT_SPACESUIT; spec->footwear = AF_FOOTWEAR_BOOT; spec->hair_style = AF_HAIR_BUN;
        spec->seed = 303;
    } else if (name == "child") {
        spec->sex = AF_SEX_UNSPECIFIED; spec->height_m = 1.40f; spec->age_years = 16.0f;
        spec->build = AF_BUILD_SLIM; spec->outfit = AF_OUTFIT_TSHIRT; spec->footwear = AF_FOOTWEAR_SNEAKER;
        spec->hair_style = AF_HAIR_SHORT;
    } else if (name == "tall") {
        spec->sex = AF_SEX_MALE; spec->height_m = 2.05f; spec->build = AF_BUILD_MUSCULAR;
        spec->outfit = AF_OUTFIT_JEANS_JACKET; spec->footwear = AF_FOOTWEAR_BOOT; spec->hair_style = AF_HAIR_BUZZ;
        spec->muscle = 0.75f;
    } else {
        return -1;
    }
    return 0;
}

void onProgress(void* user, int stage, float fraction, const char* message) {
    const bool verbose = *static_cast<bool*>(user);
    if (!verbose) return;
    std::printf("  [%2d] %5.1f%%  %s\n", stage, fraction * 100.0f, message ? message : "");
    std::fflush(stdout);
}

void onLog(void* user, int severity, const char* message) {
    const bool verbose = *static_cast<bool*>(user);
    if (!verbose && severity < AF_LOG_WARN) return;
    static const char* const kLevel[4] = {"debug", "info", "warn", "error"};
    std::printf("  %-5s %s\n", kLevel[severity & 3], message ? message : "");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "afgen: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--prompt") opt.prompt = next("--prompt");
        else if (a == "--preset") opt.preset = next("--preset");
        else if (a == "--out") opt.out = next("--out");
        else if (a == "--no-textures") opt.noTextures = true;
        else if (a == "--stats") opt.stats = true;
        else if (a == "--verbose" || a == "-v") opt.verbose = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "afgen: unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (opt.prompt.empty() && opt.preset.empty()) { usage(); return 2; }

    af_spec spec;
    if (!opt.preset.empty()) {
        if (applyPreset(opt.preset, &spec) != 0) {
            std::fprintf(stderr, "afgen: unknown preset '%s'\n", opt.preset.c_str());
            return 2;
        }
    } else {
        af_spec_init(&spec);
    }
    if (!opt.prompt.empty()) {
        af_spec_init(&spec);
        spec.seed = 1337;
    }

    af_callbacks cb{};
    cb.user = &opt.verbose;
    cb.on_progress = onProgress;
    cb.on_log = onLog;

    af_engine* engine = nullptr;
    if (af_engine_create(&spec, &cb, &engine) != AF_OK) {
        std::fprintf(stderr, "afgen: engine creation failed\n");
        return 1;
    }
    std::printf("AvatarForge %s (ABI %d)\n", af_engine_version(), af_engine_abi_version());

    if (!opt.prompt.empty()) {
        af_spec interpreted;
        const int matched = af_engine_analyze_prompt(engine, opt.prompt.c_str(), &interpreted);
        std::printf("  prompt: %d attributes recognised\n", matched);
        interpreted.seed = 1337;
        af_engine_reset(engine, &interpreted);
    }

    const auto t0 = std::chrono::steady_clock::now();
    int32_t rc;
    if (opt.noTextures) {
        /* Stage-by-stage, the way an app shows progress: geometry, UV, rig - no bake. */
        if ((rc = af_engine_fit_anatomy(engine)) != AF_OK ||
            (rc = af_engine_build_geometry(engine)) != AF_OK ||
            (rc = af_engine_unwrap_uv(engine)) != AF_OK ||
            (rc = af_engine_rig(engine)) != AF_OK) {
            std::fprintf(stderr, "afgen: stage failed (%d): %s\n", rc, af_engine_last_error(engine));
            af_engine_destroy(engine);
            return 1;
        }
    } else {
        if ((rc = af_engine_run_all(engine)) != AF_OK) {
            std::fprintf(stderr, "afgen: generation failed (%d): %s\n", rc, af_engine_last_error(engine));
            af_engine_destroy(engine);
            return 1;
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    af_stats st{};
    af_engine_stats(engine, &st);
    std::printf("  %d vertices, %d triangles, %d submeshes, %d joints, %d blendshapes\n",
                st.vertex_count, st.triangle_count, st.submesh_count, st.joint_count, st.blendshape_count);
    std::printf("  buffers: geometry %.1f MB, textures %.1f MB, peak field %.1f MB\n",
                st.geometry_bytes / 1048576.0, st.texture_bytes / 1048576.0, st.peak_field_bytes / 1048576.0);

    if (af_engine_export_glb(engine, opt.out.c_str()) != AF_OK) {
        std::fprintf(stderr, "afgen: export failed: %s\n", af_engine_last_error(engine));
        af_engine_destroy(engine);
        return 1;
    }
    std::printf("  wrote %s\n", opt.out.c_str());

    if (opt.stats) {
        std::printf("  timings: anatomy %.0f ms, geometry %.0f ms, uv %.0f ms, textures %.0f ms, "
                    "rig %.0f ms, export %.0f ms  (total %.1f s wall)\n",
                    st.fit_anatomy_ms, st.field_ms, st.uv_ms, st.texture_ms, st.rig_ms, st.export_ms, wall);
    }
    af_engine_destroy(engine);
    return 0;
}
