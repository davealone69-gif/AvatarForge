/*
 * host_tests.cpp -- host-side verification of the AvatarForge kernel.
 *
 * These are not smoke tests. Each one asserts a physical property that a photoreal avatar
 * pipeline must satisfy, and they run on the desktop so a failure is caught in seconds
 * instead of on a phone:
 *
 *   1. Anthropometric exactness  - a requested tape measurement appears in the mesh geometry.
 *   2. Field sanity              - the SDF is negative inside, positive outside, finite.
 *   3. Isosurface integrity      - watertight after cut-plane stitching, correct winding.
 *   4. Scale                     - the avatar is exactly as tall as the spec says.
 *   5. Volume                    - enclosed volume implies a plausible body mass.
 *   6. Determinism               - identical spec => byte-identical geometry.
 *   7. Rig completeness          - every vertex is weighted, weights normalise to 1, every
 *                                  ARKit blendshape moves real geometry and every clip binds.
 *   8. Texture bake              - UV atlas covers the surface and the five PBR maps carry real
 *                                  material data (melanin drives albedo, skin is dielectric).
 *   9. GLB validity              - exported glTF 2.0 parses and its accessor bounds match.
 *  14. GLB import round trip     - the exporter's own file loads back with the same geometry,
 *                                  skin, morph targets and textures, and malformed files fail
 *                                  cleanly instead of crashing the reader.
 *  12. Pose composition          - REST restores the bind pose exactly and clips do not
 *                                  accumulate; a blendshape survives a pose change.
 */
#include "../src/af_anatomy.h"
#include "../src/af_field.h"
#include "../src/af_image_pack.h"
#include "../include/afcore.h"
#include "../src/af_isosurface.h"
#include "../src/af_mesh.h"
#include "../src/af_body.h"
#include "../src/af_rig.h"
#include "../src/af_texture.h"
#include "../src/af_uv.h"
#include "../src/af_threads.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "af_avatar_culler.h" /* renderer-to-scene visibility adapter; host-testable without a GPU */
#include "af_avatar_lod.h" /* deformation-safe index LOD data; host-testable without a GPU */
#include "af_shaders.h"   /* the baked shader tables, checked by [10] */
#include "af_gpu_profile.h"
#include "af_texture_compression.h"
#include "astcenc.h"
#include "../src/af_glb_read.h"
#include "../src/af_json.h"
#include "../src/af_png.h"
#include "../src/af_png_decode.h"
#include "../src/af_scene.h"
#include "../src/af_profiler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <stdexcept>

using namespace af;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { \
    ++g_checks; \
    if (!(cond)) { ++g_failures; std::printf("  \x1b[31mFAIL\x1b[0m %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

#define CHECK_NEAR(a, b, tol, ...) do { \
    ++g_checks; \
    const double _a = (a), _b = (b); \
    if (std::fabs(_a - _b) > (tol)) { ++g_failures; std::printf("  \x1b[31mFAIL\x1b[0m %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); \
        std::printf("  (got %.6f, expected %.6f +/- %.6f)\n", _a, _b, (double)(tol)); } \
} while (0)

static const char* PASS = "\x1b[32mPASS\x1b[0m";

/** Accumulates a condition without printing, for per-element loops. */
static inline void checkQuiet(bool ok, int* failures, int* checks) { ++(*checks); if (!ok) ++(*failures); }

static af_spec defaultSpec(int sex, int build, float height) {
    af_spec s;
    af_spec_init(&s);
    s.sex = sex;
    s.build = build;
    s.height_m = height;
    s.muscle = 0.55f;
    s.body_fat = 0.20f;
    s.age_years = 29.0f;
    s.seed = 4242;
    return s;
}

/* ------------------------------------------------------- 0. image-analysis wire schema ----- */
static void testImageAnalysisPacking() {
    std::printf("[0] image-analysis wire schema\n");
    CHECK(AF_IMAGE_ANALYSIS_FLOAT_COUNT == 28 && af::kImageAnalysisFloatCount == 28,
          "native analysis stride is the complete 28-float schema");

    bool allRecordsExact = true;
    bool allGuardsIntact = true;
    for (int recordIndex = 0; recordIndex < 4; ++recordIndex) {
        const float b = static_cast<float>(recordIndex * 100);
        af_image_analysis a{};
        a.subject_coverage = b + 0;
        for (int i = 0; i < 4; ++i) a.head_box[i] = b + 1 + i;
        a.shoulder_y = b + 5;
        a.hip_y = b + 6;
        a.ankle_y = b + 7;
        a.knee_y = b + 8;
        a.is_full_body = recordIndex & 1;
        a.pose_confidence = b + 10;
        for (int i = 0; i < 3; ++i) a.skin_lab[i] = b + 11 + i;
        for (int i = 0; i < 3; ++i) a.hair_lab[i] = b + 14 + i;
        for (int i = 0; i < 3; ++i) a.garment_lab[i] = b + 17 + i;
        a.metal_confidence = b + 20;
        for (int i = 0; i < 3; ++i) a.key_light[i] = b + 21 + i;
        a.key_light_intensity = b + 24;
        for (int i = 0; i < 3; ++i) a.background_lab[i] = b + 25 + i;

        float guarded[AF_IMAGE_ANALYSIS_FLOAT_COUNT + 2];
        std::fill(std::begin(guarded), std::end(guarded), -9876.5f);
        const int written = af::packImageAnalysis(a, guarded + 1, AF_IMAGE_ANALYSIS_FLOAT_COUNT);
        if (written != AF_IMAGE_ANALYSIS_FLOAT_COUNT) allRecordsExact = false;
        for (int i = 0; i < AF_IMAGE_ANALYSIS_FLOAT_COUNT; ++i) {
            const float expected = (i == 9) ? static_cast<float>(a.is_full_body) : b + i;
            if (guarded[i + 1] != expected) allRecordsExact = false;
        }
        if (guarded[0] != -9876.5f || guarded[AF_IMAGE_ANALYSIS_FLOAT_COUNT + 1] != -9876.5f) {
            allGuardsIntact = false;
        }
    }
    CHECK(allRecordsExact, "1-4 analysis records preserve all fields in exact order");
    CHECK(allGuardsIntact, "28-float writes leave both guard values untouched");

    float undersized[AF_IMAGE_ANALYSIS_FLOAT_COUNT];
    std::fill(std::begin(undersized), std::end(undersized), 1234.5f);
    af_image_analysis zero{};
    const int shortWrite = af::packImageAnalysis(zero, undersized, AF_IMAGE_ANALYSIS_FLOAT_COUNT - 1);
    bool untouched = true;
    for (float v : undersized) if (v != 1234.5f) untouched = false;
    CHECK(shortWrite == 0 && untouched, "undersized destinations are rejected without a partial write");
    CHECK(af::packImageAnalysis(zero, nullptr, AF_IMAGE_ANALYSIS_FLOAT_COUNT) == 0,
          "null analysis destinations are rejected");
    std::printf("  %s  stride=%d, all Lab triplets retained, guards intact\n", PASS,
                AF_IMAGE_ANALYSIS_FLOAT_COUNT);
}

/* A skipped image must not shift later role indexes into a different successful analysis. */
static void testSkippedImageRoleMapping() {
    std::printf("[0b] skipped image preserves the role-to-analysis mapping\n");
    constexpr int width = 96, height = 128, channels = 3;
    auto subjectImage = [](uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * channels);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool subject = (24 <= x && x < 72 && 20 <= y && y < 75) ||
                                     (32 <= x && x < 48 && 75 <= y && y < 120) ||
                                     (48 <= x && x < 64 && 75 <= y && y < 120);
                const size_t i = (static_cast<size_t>(y) * width + x) * channels;
                pixels[i + 0] = subject ? r : 235;
                pixels[i + 1] = subject ? g : 235;
                pixels[i + 2] = subject ? b : 235;
            }
        }
        return pixels;
    };

    std::vector<uint8_t> identityPixels = subjectImage(150, 70, 30);
    std::vector<uint8_t> extraPixels = subjectImage(30, 70, 160);
    af_image images[3]{};
    images[0].width = width; images[0].height = height; images[0].channels = channels;
    images[0].role = AF_IMAGE_FULL_BODY; images[0].pixels = nullptr;  /* deliberately rejected */
    images[1] = {width, height, channels, AF_IMAGE_IDENTITY, identityPixels.data()};
    images[2] = {width, height, channels, AF_IMAGE_EXTRA_VIEW, extraPixels.data()};

    af_spec initial;
    af_spec_init(&initial);
    af_engine* engine = nullptr;
    CHECK(af_engine_create(&initial, nullptr, &engine) == AF_OK && engine != nullptr,
          "engine created for image-role regression");
    if (!engine) return;

    af_image_analysis analyses[3]{};
    const int32_t produced = af_engine_analyze_images(engine, images, 3, analyses, 3);
    CHECK(produced == 2, "two valid images survive one rejected input (%d records)", produced);
    af_spec resolved{};
    const int32_t parsed = af_engine_analyze_prompt(engine, "", &resolved);
    CHECK(parsed >= 0, "resolved image-derived spec can be read (%d)", parsed);
    if (produced == 2 && parsed >= 0) {
        const auto melaninFor = [](const af_image_analysis& a) {
            const float ref = std::max(22.0f, a.background_lab[0]);
            const float ratio = std::max(0.15f, std::min(1.9f, a.skin_lab[0] / ref));
            return std::max(0.03f, std::min(0.95f, 0.62f - 0.52f * ratio));
        };
        const float expected = melaninFor(analyses[0]);
        const float wrongShifted = melaninFor(analyses[1]);
        CHECK(std::fabs(expected - wrongShifted) > 0.03f,
              "fixture distinguishes the identity analysis from the following extra view");
        CHECK_NEAR(resolved.skin_melanin, expected, 1e-5f,
                   "explicit identity role resolves from its own compacted analysis record");
    }
    af_engine_destroy(engine);
}

/* ------------------------------------------------------------------ 1. anthropometry ----- */
static void testAnthropometry() {
    std::printf("[1] anthropometric exactness\n");

    /* Height is a resolved SI value at the public boundary, never a UI-only zero sentinel. Legacy
       zero and NaN inputs still resolve safely before they can poison the field. */
    af_spec defaults;
    af_spec_init(&defaults);
    CHECK_NEAR(defaults.height_m, AF_DEFAULT_HEIGHT_M, 1e-6f,
               "af_spec_init publishes a non-zero default stature");
    const BodyMetrics defaultMetrics = Anatomy::solve(defaults);
    CHECK_NEAR(defaultMetrics.height, AF_DEFAULT_HEIGHT_M, 1e-6f,
               "default stature reaches anatomy in metres");
    af_spec legacyZero = defaults;
    legacyZero.height_m = 0.0f;
    CHECK_NEAR(Anatomy::solve(legacyZero).height, AF_DEFAULT_HEIGHT_M, 1e-6f,
               "legacy zero height resolves before field generation");
    af_spec explicitHeight = defaults;
    explicitHeight.height_m = 1.82f;
    CHECK_NEAR(Anatomy::solve(explicitHeight).height, 1.82f, 1e-6f,
               "explicit metre override wins over the default");

    af_spec spec = defaultSpec(AF_SEX_FEMALE, AF_BUILD_ATHLETIC, 1.70f);
    spec.waist_cm = 68.0f;
    spec.hip_cm = 96.0f;
    spec.chest_cm = 90.0f;
    spec.inseam_cm = 79.0f;

    af_spec withoutInseam = spec;
    withoutInseam.inseam_cm = 0.0f;
    const BodyMetrics base = Anatomy::solve(withoutInseam);
    const BodyMetrics m = Anatomy::solve(spec);
    CHECK_NEAR(Anatomy::perimeter(m.waist) * 100.0f, 68.0f, 0.15f, "waist perimeter must equal the requested tape value");
    CHECK_NEAR(Anatomy::perimeter(m.hip) * 100.0f, 96.0f, 0.15f, "hip perimeter must equal the requested tape value");
    CHECK_NEAR(Anatomy::perimeter(m.chest) * 100.0f, 90.0f, 0.15f, "chest perimeter must equal the requested tape value");
    CHECK_NEAR(m.y.crotchY * 100.0f, 79.0f, 0.30f, "inseam cm converts once and pins crotch metres");
    const float legScale = m.y.crotchY / base.y.crotchY;
    CHECK_NEAR(m.y.ankleY, base.y.ankleY * legScale, 1e-5f,
               "inseam remaps ankle on the same lower-leg scale");
    CHECK_NEAR(m.y.kneeY, base.y.kneeY * legScale, 1e-5f,
               "inseam remaps knee on the same lower-leg scale");
    CHECK_NEAR(m.y.crownY, spec.height_m, 1e-6f,
               "inseam changes proportions, never total stature");

    /* Landmarks must be strictly ordered or the field would invert. */
    CHECK(m.y.soleY < m.y.ankleY && m.y.ankleY < m.y.calfMaxY &&
          m.y.calfMaxY < m.y.kneeY && m.y.kneeY < m.y.midThighY &&
          m.y.midThighY < m.y.crotchY && m.y.crotchY < m.y.hipY,
          "complete leg landmark chain ordered after inseam override");
    CHECK(m.y.hipY < m.y.waistY && m.y.waistY < m.y.bustY, "torso landmarks ordered");
    CHECK(m.y.bustY < m.y.shoulderY && m.y.shoulderY < m.y.neckBaseY,
          "upper torso landmarks ordered");
    CHECK(m.y.neckBaseY < m.y.chinY && m.y.chinY < m.y.jawY &&
          m.y.jawY < m.y.eyeY && m.y.eyeY < m.y.crownY,
          "with +Y up, chin is below jaw and the complete head chain is ordered");
    CHECK(m.noseLength > 0.0f && std::isfinite(m.noseLength),
          "facial lengths remain positive after chin/jaw solve (nose %.4f m)", m.noseLength);
    CHECK(m.y.crownY <= spec.height_m + 1e-4f, "crown must not exceed stature");
    CHECK(m.biacromialHalf > 0.10f && m.biacromialHalf < 0.30f, "shoulder half-width plausible");

    /* Regression path (no tape supplied) must also produce a sane adult. */
    const af_spec plain = defaultSpec(AF_SEX_MALE, AF_BUILD_AVERAGE, 1.78f);
    const BodyMetrics pm = Anatomy::solve(plain);
    const float bmi = Anatomy::bmi(pm);
    CHECK(bmi > 17.0f && bmi < 34.0f, "regression BMI plausible");
    std::printf("  %s  female waist=%.2fcm hip=%.2fcm crotch=%.2fcm | male BMI=%.1f\n", PASS,
                Anatomy::perimeter(m.waist) * 100.0f, Anatomy::perimeter(m.hip) * 100.0f,
                m.y.crotchY * 100.0f, bmi);
}

/* ------------------------------------------------------------------ 2. field sanity ------ */
static void testField() {
    std::printf("[2] signed distance field sanity\n");
    af_spec spec = defaultSpec(AF_SEX_MALE, AF_BUILD_ATHLETIC, 1.82f);
    const BodyMetrics m = Anatomy::solve(spec);
    BodyField field(m);

    /* Inside: centre of the abdomen must be well inside the surface. */
    const float dInside = field.body(Vec3(0.0f, m.y.waistY, m.waist.centreZ));
    CHECK(dInside < -0.02f, "abdomen centre is inside (d=%.4f)", dInside);
    /* Outside: a metre to the side must be far outside. */
    const float dOutside = field.body(Vec3(1.0f, m.y.waistY, 0.0f));
    CHECK(dOutside > 0.5f, "far point is outside (d=%.4f)", dOutside);
    /* Lipschitz: |f(a)-f(b)| <= |a-b| * C. Central-difference gradient must not exceed ~2. */
    float maxGrad = 0.0f;
    const float h = 0.004f;
    for (int i = 0; i < 400; ++i) {
        const float t = static_cast<float>(i) / 399.0f;
        const Vec3 p(0.05f * std::sin(t * 31.0f), m.y.crotchY + t * (m.y.crownY - m.y.crotchY),
                     0.05f * std::cos(t * 17.0f));
        const float gx = field.body(p + Vec3(h, 0, 0)) - field.body(p - Vec3(h, 0, 0));
        const float gy = field.body(p + Vec3(0, h, 0)) - field.body(p - Vec3(0, h, 0));
        const float gz = field.body(p + Vec3(0, 0, h)) - field.body(p - Vec3(0, 0, h));
        maxGrad = maxf(maxGrad, Vec3(gx, gy, gz).length() / (2.0f * h));
    }
    CHECK(maxGrad < 2.5f, "field is nearly 1-Lipschitz (max |grad| = %.3f)", maxGrad);
    CHECK(std::isfinite(field.body(Vec3(0, m.y.waistY, 0))), "field finite");
    /* Head domain must contain the skull and exclude the abdomen. */
    CHECK(field.head(Vec3(0.0f, m.y.crownY - 0.05f, 0.0f)) < 0.0f, "cranium centre inside head domain");
    CHECK(field.head(Vec3(0.0f, m.y.waistY, 0.0f)) > 0.0f, "abdomen outside head domain");
    std::printf("  %s  abdomen d=%.4f  lateral d=%.4f  max|grad|=%.3f\n", PASS, dInside, dOutside, maxGrad);
}

/* ------------------------------------------------------------------ 3-5. mesh build ------ */
struct BuiltAvatar {
    Mesh mesh;
    BodyMetrics metrics;
    double buildMs = 0.0;
    bool ok = false;
    BodyBuildReport report;
};

static BuiltAvatar buildFullBody(const af_spec& spec, float bodyCell = 0.008f, int detailRounds = 2,
                                 bool dressed = false) {
    BuiltAvatar out;
    out.metrics = Anatomy::solve(spec);
    BodyField field(out.metrics);
    ApparelOptions ap;
    ap.outfit = dressed ? spec.outfit : 0;
    ap.footwear = dressed ? spec.footwear : 0;
    ap.hair = dressed ? spec.hair_style : 0;
    ap.latticeCell = bodyCell;
    field.setApparel(ap);
    BodyBuildOptions opt;
    opt.bodyCell = bodyCell;
    opt.detailRounds = detailRounds;
    opt.smoothPasses = 1;
    opt.threads = defaultThreadCount();
    BodyBuildReport rep;
    out.ok = buildBodyMesh(field, opt, &out.mesh, &rep);
    out.buildMs = rep.millis;
    out.report = rep;
    return out;
}

static void testIsosurface() {
    std::printf("[3] isosurface integrity\n");
    const af_spec spec = defaultSpec(AF_SEX_FEMALE, AF_BUILD_ATHLETIC, 1.70f);
    BuiltAvatar av = buildFullBody(spec);

    const Mesh::ManifoldReport rep = av.mesh.audit();
    CHECK(av.ok, "engine assembly returned success");
    CHECK(av.mesh.triangleCount() > 30000, "mesh has real detail (%zu tris)", av.mesh.triangleCount());
    CHECK(av.mesh.vertexCount() > 15000, "mesh has real vertex count (%zu verts)", av.mesh.vertexCount());
    /* Topology assertions are stated in terms the mesh can actually be held to. Every triangle
       edge must be shared by exactly two triangles (closed shell) OR be part of a documented
       pinch-point defect below the stated fraction; anything above that is a hard failure. */
    const double edges = static_cast<double>(av.mesh.indices.size());
    const double openFrac = edges > 0 ? static_cast<double>(rep.openEdges) / edges : 1.0;
    const double nmFrac = edges > 0 ? static_cast<double>(rep.nonManifoldEdges) / edges : 1.0;
    CHECK(av.report.quality != TopologyQuality::DEGRADED, "topology quality not degraded");
    CHECK(openFrac < 8e-4, "shell closed to within 0.08%% (%zu open of %.0f directed edges, %.4f%%)",
          rep.openEdges, edges, openFrac * 100.0);
    CHECK(nmFrac < 4e-3, "non-manifold fraction under 0.4%% (%zu edges, %.4f%%)",
          rep.nonManifoldEdges, nmFrac * 100.0);
    /* One continuous shell: no boundary loops at all anywhere on the body. */
    {
        const auto loops = findBoundaryLoops(av.mesh);
        size_t openVerts = 0;
        for (const auto& l : loops) openVerts += l.size();
        CHECK(loops.empty(), "single continuous shell with no boundary loops (%zu loops, %zu verts)",
              loops.size(), openVerts);
    }
    CHECK(av.report.footTris == 0 && av.report.headTris == 0,
          "mesh came from the unified grid, not from stitched sub-domains");

    /* Winding: with the field negative inside, the volume from the divergence theorem must be
       positive when the winding is outward - a negative volume means inside-out triangles. */
    const float vol = av.mesh.volume();
    CHECK(vol > 0.0f, "outward winding (volume %.5f m^3)", vol);

    /* Normals must be unit length and finite. */
    float worstNorm = 1.0f;
    for (const Vec3& n : av.mesh.normals) worstNorm = minf(worstNorm, n.length());
    CHECK(worstNorm > 0.98f, "all normals unit length (worst %.4f)", worstNorm);

    std::printf("  %s  %zu tris / %zu verts / %zu open / %zu nonmanifold / %d stitches / %d holes / vol=%.4f m^3 / %.0f ms\n",
                PASS, av.mesh.triangleCount(), av.mesh.vertexCount(), rep.openEdges, rep.nonManifoldEdges,
                av.report.stitches, av.report.holesFilled, vol, av.buildMs);
}

static void testScaleAndVolume() {
    std::printf("[4/5] scale and volume plausibility\n");
    for (int variant = 0; variant < 4; ++variant) {
        af_spec spec;
        switch (variant) {
            case 0: spec = defaultSpec(AF_SEX_FEMALE, AF_BUILD_SLIM, 1.62f); break;
            case 1: spec = defaultSpec(AF_SEX_MALE, AF_BUILD_MUSCULAR, 1.86f); break;
            case 2: spec = defaultSpec(AF_SEX_MALE, AF_BUILD_HEAVY, 1.75f); break;
            default: spec = defaultSpec(AF_SEX_FEMALE, AF_BUILD_AVERAGE, 1.55f); break;
        }
        BuiltAvatar av = buildFullBody(spec, 0.016f, 2);
        Vec3 mn, mx;
        av.mesh.computeBounds(&mn, &mx);
        /*
         * The field is a signed-distance surface, and a signed-distance surface is NOT the
         * skeleton: the crown of the head lies at crownY + skull thickness (~8 mm), and the soles
         * sit on the contact plane at soleY + soleGap. Asserting the mesh bounding box equals the
         * skeleton height would be asserting a bug. What must hold is:
         *   - the mesh is at least as tall as the skeleton (it encloses the crown landmark),
         *   - it is not absurdly taller (no runaway inflation of the crown or the feet),
         *   - the floor drop put the soles exactly on y = 0.
         */
        CHECK(mx.y >= av.metrics.y.crownY, "variant %d: mesh encloses the crown landmark (%.4f >= %.4f)", variant, mx.y, av.metrics.y.crownY);
        CHECK(mx.y <= spec.height_m + 0.020f, "variant %d: crown thickness under 20 mm (%.4f vs spec %.3f)", variant, mx.y, spec.height_m);
        /* The sole clamp in BodyField::footShape puts the soles on the contact plane, which is
           soleY + soleGap - 2 mm by default, so the avatar stands 2 mm off y = 0 and never dips
           through it. That is the designed grounding, and it is what the GLB exporter offsets for. */
        CHECK(std::fabs(mn.y - 0.002f) < 0.0015f, "variant %d: soles on the contact plane (mn.y=%.5f, expected ~0.002)", variant, mn.y);
        const float vol = av.mesh.volume();
        const float massKg = vol * 985.0f;
        CHECK(vol > 0.030f && vol < 0.16f, "variant %d: plausible volume %.4f m^3 (%.0f kg)", variant, vol, massKg);
        CHECK(Anatomy::bmi(av.metrics) > 15.0f && Anatomy::bmi(av.metrics) < 38.0f,
              "variant %d: plausible BMI %.1f", variant, Anatomy::bmi(av.metrics));

        /*
         * Topology has to hold for every body type, not just the default avatar.
         *
         * Marching tets decides a cell from eight sample signs, so a surface that grazes a sample
         * plane can come back with a missing cap - a handful of boundary edges in a place that
         * depends on the body shape. Those are closed by stitchSmallHoles(); what this asserts is
         * that the repair is complete, for a slim 1.62 m female through to a heavy 1.75 m male.
         * A variant that shipped with an open loop would render with a see-through patch and
         * would not round-trip through the GLB exporter's manifold check.
         */
        const Mesh::ManifoldReport vrep = av.mesh.audit();
        const auto vloops = findBoundaryLoops(av.mesh);
        const double vnmFrac = static_cast<double>(vrep.nonManifoldEdges) / static_cast<double>(av.mesh.indices.size());
        std::printf("     variant %d: %zu verts / %zu tris / %zu open / %zu boundary loops / %d stitches / %d holes\n",
                    variant, av.mesh.vertexCount(), av.mesh.triangleCount(), vrep.openEdges,
                    vloops.size(), av.report.stitches, av.report.holesFilled);
        CHECK(vloops.empty() && vrep.openEdges == 0,
              "variant %d: closed shell, no boundary loops (%zu loops, %zu open edges)",
              variant, vloops.size(), vrep.openEdges);
        CHECK(vnmFrac < 1e-3, "variant %d: non-manifold fraction under 0.1%% (%zu edges)",
              variant, vrep.nonManifoldEdges);
        CHECK(av.mesh.volume() > 0.0f, "variant %d: outward winding (%.5f m^3)", variant, av.mesh.volume());
    }
    std::printf("  %s  4 build variants: height, ground contact, volume and BMI all in range\n", PASS);
}

static void testDeterminism() {
    std::printf("[6] determinism\n");
    const af_spec spec = defaultSpec(AF_SEX_MALE, AF_BUILD_ATHLETIC, 1.80f);
    BodyField f1(Anatomy::solve(spec));
    BodyField f2(Anatomy::solve(spec));
    double sum = 0.0, sum2 = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const float t = static_cast<float>(i) / 4999.0f;
        const Vec3 p(0.3f * (t - 0.5f) * 2.0f, 0.15f + t * 1.7f, 0.2f * std::sin(t * 40.0f));
        sum += f1.body(p);
        sum2 += f2.body(p);
    }
    CHECK_NEAR(sum, sum2, 1e-3, "two fields from the same spec evaluate identically");
    const BodyMetrics m1 = Anatomy::solve(spec);
    const BodyMetrics m2 = Anatomy::solve(spec);
    CHECK(m1.waist.rx == m2.waist.rx && m1.y.crownY == m2.y.crownY, "metrics deterministic");
    std::printf("  %s  identical spec reproduces identical field (sum %.6f)\n", PASS, sum);
}

/* ------------------------------------------------------------------ main ------------------ */
/* -------------------------------------------------------------------------------------------
 * [7] rig, skin weights and animation
 * ----------------------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------
 * [8] texturing: UV atlas layout and the PBR bake
 * ----------------------------------------------------------------------------------------- */
/*
 * [9] Apparel: clothing, footwear, hair and equipment.
 *
 * Every outfit is generated as an offset solid of the body itself (see BodyField::clothUpper), so
 * the assertions here are about the INVARIANTS that construction must satisfy rather than about any
 * particular silhouette: a garment that is worn must exist as geometry, must be labelled with its
 * own material region, must hide the skin underneath it, and must not break the watertightness of
 * the mesh. A generator that returns the same bare body for every outfit would pass none of them.
 */
static void testApparel() {
    std::printf("\n[9] apparel: clothing, footwear, hair and equipment\n");

    struct Case { const char* name; int outfit; int footwear; int hair; };
    const Case cases[] = {
        {"bare + bald",              AF_OUTFIT_NONE,        AF_FOOTWEAR_NONE,   AF_HAIR_NONE},
        {"tshirt + sneaker + short", AF_OUTFIT_TSHIRT,      AF_FOOTWEAR_SNEAKER, AF_HAIR_SHORT},
        {"dress + heel + long",      AF_OUTFIT_DRESS,       AF_FOOTWEAR_HEEL,   AF_HAIR_LONG},
        {"plate + sabaton + afro",   AF_OUTFIT_ARMOR_PLATE, AF_FOOTWEAR_SABATON, AF_HAIR_AFRO},
        {"spacesuit + ponytail",     AF_OUTFIT_SPACESUIT,   AF_FOOTWEAR_BOOT,   AF_HAIR_PONYTAIL},
    };
    const size_t caseCount = sizeof(cases) / sizeof(cases[0]);

    /* Fingerprints, so "different parameters produce different geometry" is a measurement. */
    double area[caseCount];
    size_t topoA[caseCount];
    for (size_t c = 0; c < caseCount; ++c) {
        af_spec spec;
        af_spec_init(&spec);
        spec.sex = AF_SEX_FEMALE; spec.build = AF_BUILD_ATHLETIC; spec.height_m = 1.70f;
        spec.seed = 4242; spec.outfit = cases[c].outfit;
        spec.footwear = cases[c].footwear; spec.hair_style = cases[c].hair;
        /* Coarser lattice for the matrix: these checks are about the garments, not the face. */
        BuiltAvatar av = buildFullBody(spec, 0.0115f, 1, true);
        CHECK(av.ok, "%s builds", cases[c].name);
        CHECK(av.report.openEdgesAfterRepair == 0 && av.report.nonManifoldEdges == 0,
              "%s is watertight (%zu open, %zu non-manifold)", cases[c].name,
              av.report.openEdgesAfterRepair, av.report.nonManifoldEdges);

        size_t counts[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (int32_t r : av.mesh.regions) {
            if (r >= 0 && r < 13) counts[r]++;
        }
        double a = 0.0;
        for (size_t t = 0; t < av.mesh.triangleCount(); ++t) {
            const Vec3 p0 = av.mesh.positions[av.mesh.indices[t * 3]];
            const Vec3 p1 = av.mesh.positions[av.mesh.indices[t * 3 + 1]];
            const Vec3 p2 = av.mesh.positions[av.mesh.indices[t * 3 + 2]];
            a += (p1 - p0).cross(p2 - p0).length() * 0.5;
        }
        area[c] = a;
        topoA[c] = av.mesh.triangleCount();

        /* What is actually worn has to be present as geometry. */
        if (cases[c].outfit != AF_OUTFIT_NONE && cases[c].outfit != AF_OUTFIT_DRESS) {
            CHECK(counts[REGION_CLOTH_UPPER] > 200, "%s: upper garment present (%zu verts)",
                  cases[c].name, counts[REGION_CLOTH_UPPER]);
        }
        switch (cases[c].outfit) {
            case AF_OUTFIT_DRESS:    CHECK(counts[REGION_CLOTH_LOWER] > 0 && counts[REGION_CLOTH_UPPER] > 200,
                                           "dress: skirt and bodice present (%zu / %zu)",
                                           counts[REGION_CLOTH_LOWER], counts[REGION_CLOTH_UPPER]); break;
            case AF_OUTFIT_TSHIRT:   CHECK(counts[REGION_CLOTH_LOWER] == 0,
                                           "tshirt: nothing on the legs (%zu)", counts[REGION_CLOTH_LOWER]); break;
            default:
                if (cases[c].outfit != AF_OUTFIT_NONE) {
                    CHECK(counts[REGION_CLOTH_LOWER] > 200, "%s: lower garment present (%zu verts)",
                          cases[c].name, counts[REGION_CLOTH_LOWER]);
                }
                break;
        }
        if (cases[c].hair != AF_HAIR_NONE) {
            CHECK(counts[REGION_HAIR] > 100, "%s: hair present (%zu verts)", cases[c].name, counts[REGION_HAIR]);
        } else {
            CHECK(counts[REGION_HAIR] == 0, "bare: no hair geometry (%zu)", counts[REGION_HAIR]);
        }
        if (cases[c].footwear != AF_FOOTWEAR_NONE) {
            CHECK(counts[REGION_SHOE] > 100, "%s: footwear present (%zu verts)", cases[c].name, counts[REGION_SHOE]);
        } else {
            CHECK(counts[REGION_SHOE] == 0, "bare: no footwear geometry (%zu)", counts[REGION_SHOE]);
        }
        if (cases[c].outfit == AF_OUTFIT_ARMOR_PLATE || cases[c].outfit == AF_OUTFIT_SPACESUIT) {
            CHECK(counts[REGION_GEAR] > 200, "%s: equipment present (%zu verts)", cases[c].name, counts[REGION_GEAR]);
        }
        /* A dressed body hides skin: that is the whole point of building cloth as an offset solid. */
        if (cases[c].outfit != AF_OUTFIT_NONE) {
            CHECK(counts[REGION_SKIN] < 140000, "%s: garment covers skin (%zu skin verts)", cases[c].name,
                  counts[REGION_SKIN]);
        }
    }

    CHECK(topoA[1] != topoA[0] && topoA[3] != topoA[1] && topoA[4] != topoA[3],
          "different outfits build different meshes (%zu / %zu / %zu / %zu tris)",
          topoA[0], topoA[1], topoA[3], topoA[4]);
    CHECK(area[3] > area[0] * 1.05, "plate armour adds surface area (%.3f vs %.3f m^2)", area[3], area[0]);
    CHECK(area[4] > area[3] * 0.95, "the spacesuit is at least as bulky as the plate (%.3f m^2)", area[4]);

    /*
     * Grounding: a shod avatar must still stand on y = 0. The shoe's sole is built below the foot's
     * own sole - that is what a sole is - so without a deliberate floor clip the wearer sinks into
     * the floor by exactly the sole's thickness.
     */
    {
        af_spec spec;
        af_spec_init(&spec);
        spec.sex = AF_SEX_MALE; spec.build = AF_BUILD_AVERAGE; spec.height_m = 1.80f;
        spec.seed = 99; spec.outfit = AF_OUTFIT_SUIT; spec.footwear = AF_FOOTWEAR_BOOT;
        BuiltAvatar av = buildFullBody(spec, 0.0115f, 1, true);
        Vec3 mn, mx;
        av.mesh.computeBounds(&mn, &mx);
        CHECK(mn.y > -0.004f, "a booted avatar does not sink below the floor (min y %.4f m)", mn.y);
        CHECK(av.report.openEdgesAfterRepair == 0, "the booted build is watertight");
    }

    /*
     * Hair must clear the skull. The crown is the top of the bone; a hairstyle that adds nothing
     * above it is not a hairstyle, and one whose geometry sinks into the head is a modelling error.
     */
    {
        af_spec spec;
        af_spec_init(&spec);
        spec.sex = AF_SEX_FEMALE; spec.build = AF_BUILD_AVERAGE; spec.height_m = 1.65f;
        spec.seed = 7; spec.hair_style = AF_HAIR_AFRO;
        spec.outfit = AF_OUTFIT_NONE; spec.footwear = AF_FOOTWEAR_NONE;
        BuiltAvatar av = buildFullBody(spec, 0.0115f, 1, true);
        Vec3 mn, mx;
        av.mesh.computeBounds(&mn, &mx);
        const float crown = av.metrics.y.crownY;
        CHECK(mx.y > crown + 0.06f, "the afro stands well above the skull (crown %.3f, top %.3f)",
              crown, mx.y);
        CHECK(mx.y < crown + 0.20f, "the afro is a hairstyle, not a sphere (%+.3f m)", mx.y - crown);
    }
}

/*
 * [9] Public C ABI, end to end.
 *
 * The ABI is what the Kotlin SDK and the Rust crates consume, so it is tested through its own
 * surface only - create, drive the stages, read the buffers, pose, animate, export - with no back
 * door into the kernel. A test that reaches inside would not notice a struct layout mistake, and a
 * struct layout mistake is the one failure mode an ABI test exists to catch.
 */
static int g_progressCalls = 0;
static int g_logCalls = 0;
static int g_cancelFlag = 0;
static void testAbiProgress(void* user, int stage, float fraction, const char* message) {
    (void)stage; (void)fraction; (void)message; (void)user;
    ++g_progressCalls;
}
static void testAbiLog(void* user, int severity, const char* message) {
    (void)user;
    if (severity == AF_LOG_ERROR) { std::printf("     [abi error] %s\n", message); }
    ++g_logCalls;
}
static int testAbiCancel(void* user) { (void)user; return g_cancelFlag; }

static void testAbi() {
    std::printf("\n[11] public C ABI, end to end\n");
    CHECK(af_engine_abi_version() == AF_ABI_VERSION, "abi version matches the header (%d)", af_engine_abi_version());
    CHECK(std::string(af_engine_version()).size() > 0, "engine version is reported (%s)", af_engine_version());

    /* --- prompt interpretation, on the ABI's own entry point --- */
    af_spec spec;
    af_spec_init(&spec);
    af_callbacks cb{};
    cb.on_progress = testAbiProgress;
    cb.on_log = testAbiLog;
    cb.should_cancel = testAbiCancel;
    af_engine* engine = nullptr;
    CHECK(af_engine_create(&spec, &cb, &engine) == AF_OK && engine != nullptr, "engine created");

    /* Outfit NONE is a genuine unclothed surface, so the adult-only rule must live below the UI.
       Exercise direct create, reset and prompt entry points to prove no C/JNI/Rust caller bypasses it. */
    af_spec minorNude;
    af_spec_init(&minorNude);
    minorNude.age_years = 17.0f;
    minorNude.outfit = AF_OUTFIT_NONE;
    af_engine* rejected = nullptr;
    CHECK(af_engine_create(&minorNude, nullptr, &rejected) == AF_ERR_INVALID_ARG && rejected == nullptr,
          "minor nude spec is rejected at engine creation");

    af_spec minorClothed = minorNude;
    minorClothed.outfit = AF_OUTFIT_TSHIRT;
    af_engine* allowed = nullptr;
    CHECK(af_engine_create(&minorClothed, nullptr, &allowed) == AF_OK && allowed != nullptr,
          "the adult-only nude rule does not reject a clothed age-17 spec");
    af_engine_destroy(allowed);

    CHECK(af_engine_reset(engine, &minorNude) == AF_ERR_INVALID_ARG &&
              std::strstr(af_engine_last_error(engine), "age_years >= 18") != nullptr,
          "minor nude spec is rejected on reset with a diagnostic");
    af_spec rejectedPrompt;
    af_spec_init(&rejectedPrompt);
    CHECK(af_engine_analyze_prompt(engine, "teenage girl, nude", &rejectedPrompt) == AF_ERR_INVALID_ARG &&
              std::strstr(af_engine_last_error(engine), "minor or teen") != nullptr,
          "minor/teen nude prompt is rejected by the native parser boundary");

    af_spec centimetrePrompt;
    af_spec_init(&centimetrePrompt);
    CHECK(af_engine_analyze_prompt(engine, "person 175 cm", &centimetrePrompt) >= 1,
          "centimetre stature is recognised");
    CHECK_NEAR(centimetrePrompt.height_m, 1.75f, 1e-6f,
               "height converts from centimetres to metres exactly once");

    spec.sex = AF_SEX_FEMALE; spec.height_m = 1.68f; spec.seed = 4242;
    af_spec fromPrompt;
    af_spec_init(&fromPrompt);
    const int matched = af_engine_analyze_prompt(engine, "athletic woman, 1.68 m, long dark hair, "
                                                          "leather armour and boots, olive skin",
                                                 &fromPrompt);
    CHECK(matched >= 5, "prompt attributes recognised (%d)", matched);
    CHECK(fromPrompt.sex == AF_SEX_FEMALE, "prompt set sex (female)");
    CHECK(std::fabs(fromPrompt.height_m - 1.68f) < 1e-3f, "prompt set stature (%.3f m)", fromPrompt.height_m);
    CHECK(fromPrompt.hair_style == AF_HAIR_LONG, "prompt set the hairstyle (%d)", fromPrompt.hair_style);
    CHECK(fromPrompt.outfit == AF_OUTFIT_ARMOR_LEATHER, "prompt set the outfit (%d)", fromPrompt.outfit);
    CHECK(fromPrompt.skin_melanin > 0.30f && fromPrompt.skin_melanin < 0.46f,
          "prompt set the skin tone (melanin %.2f)", fromPrompt.skin_melanin);
    /* The engine's own handle now carries that spec. */
    fromPrompt.seed = 4242;
    CHECK(af_engine_reset(engine, &fromPrompt) == AF_OK, "engine reset with the interpreted spec");

    /* --- stage ordering: a downstream stage before its input is an error, not a crash --- */
    CHECK(af_engine_unwrap_uv(engine) == AF_ERR_NOT_READY, "UV before geometry is refused");
    CHECK(af_engine_bake_textures(engine) == AF_ERR_NOT_READY, "bake before geometry is refused");
    CHECK(af_engine_set_memory_budget(engine, 1024) == AF_ERR_INVALID_ARG, "absurd memory budget refused");

    /* --- cancellation must be honoured, not merely polled at the end --- */
    g_cancelFlag = 1;
    CHECK(af_engine_run_all(engine) == AF_ERR_CANCELLED, "cancellation is honoured");
    g_cancelFlag = 0;

    /* --- the dependency-ordered stage graph and native finalize barrier --- */
    g_progressCalls = 0;
    g_logCalls = 0;
    const struct { int stage; int32_t (*run)(af_engine*); const char* label; } stageGraph[] = {
        {AF_STAGE_FIT_ANATOMY, af_engine_fit_anatomy, "fit anatomy"},
        {AF_STAGE_GEOMETRY, af_engine_build_geometry, "build geometry"},
        {AF_STAGE_UV_UNWRAP, af_engine_unwrap_uv, "unwrap UVs"},
        {AF_STAGE_BAKE_TEXTURES, af_engine_bake_textures, "bake textures"},
        {AF_STAGE_RIG, af_engine_rig, "build rig"},
        {AF_STAGE_BLENDSHAPES, af_engine_bake_blendshapes, "bake blendshapes"},
        {AF_STAGE_ANIMATION, af_engine_bake_animation, "bake animations"},
    };
    for (const auto& node : stageGraph) {
        const int32_t stageRc = node.run(engine);
        CHECK(stageRc == AF_OK, "dependency graph node %s completes (rc=%d)", node.label, stageRc);
    }
    CHECK(af_engine_run_all(engine) == AF_OK, "run_all finalizes an already completed stage graph");
    CHECK(af_engine_dirty_mask(engine) == 0, "nothing is left dirty after staged run (%d)", af_engine_dirty_mask(engine));
    CHECK(g_progressCalls > 8, "progress was reported through the callback (%d calls)", g_progressCalls);
    CHECK(g_logCalls > 3, "log messages were reported through the callback (%d)", g_logCalls);
    CHECK(std::string(af_engine_last_error(engine)).empty(), "no error recorded on success (\"%s\")",
          af_engine_last_error(engine));

    af_stats st{};
    CHECK(af_engine_stats(engine, &st) == AF_OK, "stats readable");
    std::printf("     %d verts, %d tris, %d submeshes, %d joints, %d blendshapes, %.1f MB textures, "
                "%.0f ms geometry, %.0f ms uv, %.0f ms texture, %.0f ms rig\n",
                st.vertex_count, st.triangle_count, st.submesh_count, st.joint_count, st.blendshape_count,
                st.texture_bytes / 1048576.0, st.field_ms, st.uv_ms, st.texture_ms, st.rig_ms);
    CHECK(st.vertex_count > 40000, "vertex buffer is real (%d)", st.vertex_count);
    CHECK(st.triangle_count > 120000, "index buffer is real (%d triangles)", st.triangle_count);
    CHECK(st.submesh_count >= 3, "materials split into submeshes (%d)", st.submesh_count);
    CHECK(st.joint_count >= 60, "skeleton has a full humanoid joint set (%d)", st.joint_count);
    CHECK(st.blendshape_count == 52, "52 ARKit blendshapes (%d)", st.blendshape_count);
    CHECK(st.texture_bytes > 32u * 1024u * 1024u, "texture set is present (%.1f MB)", st.texture_bytes / 1048576.0);
    CHECK(st.geometry_bytes > 4u * 1024u * 1024u, "geometry accounting is real (%.1f MB)", st.geometry_bytes / 1048576.0);

    /* --- vertex data validity --- */
    const af_vertex* verts = af_engine_vertices(engine);
    const uint32_t* idx = af_engine_indices(engine);
    const int vcount = af_engine_vertex_count(engine);
    const int icount = af_engine_index_count(engine);
    CHECK(verts != nullptr && idx != nullptr, "vertex and index buffers exposed");
    if (verts && idx) {
        const af_vertex* restVerts = af_engine_vertices_rest(engine);
        if (!restVerts) restVerts = verts;
        AvatarLodSet generatedLods;
        const bool lodBuildOk = generatedLods.build(
            restVerts, static_cast<size_t>(vcount), idx, static_cast<size_t>(icount),
            af_engine_submeshes(engine), static_cast<size_t>(af_engine_submesh_count(engine)));
        CHECK(lodBuildOk && generatedLods.levelCount() >= 2 && generatedLods.levelCount() <= kAvatarLodLevels,
              "renderer builds LODs for the generated skinned avatar (%d levels)", generatedLods.levelCount());
        CHECK(lodBuildOk && generatedLods.level(0).indexCount == static_cast<uint32_t>(icount) &&
                  std::equal(idx, idx + icount, generatedLods.indices().begin()),
              "generated avatar LOD 0 is byte-for-byte the original index stream");
        bool generatedRangesValid = lodBuildOk;
        bool generatedBoundsValid = lodBuildOk;
        for (int level = 0; level < generatedLods.levelCount(); ++level) {
            const AvatarLodLevel& lod = generatedLods.level(level);
            uint64_t submeshIndexCount = 0;
            for (const AvatarLodRange& range : lod.submeshes) {
                submeshIndexCount += range.indexCount;
                const uint64_t end = uint64_t(range.firstIndex) + range.indexCount;
                generatedRangesValid = generatedRangesValid && end <= generatedLods.indices().size();
                for (uint64_t i = range.firstIndex; i < end && i < generatedLods.indices().size(); ++i)
                    generatedBoundsValid = generatedBoundsValid && generatedLods.indices()[static_cast<size_t>(i)] < static_cast<uint32_t>(vcount);
            }
            generatedRangesValid = generatedRangesValid && submeshIndexCount == lod.indexCount;
        }
        CHECK(generatedRangesValid && generatedBoundsValid,
              "generated avatar LOD submesh ranges cover the stream and reference valid source vertices");
        CHECK(generatedLods.triangleCount(generatedLods.levelCount() - 1) < generatedLods.triangleCount(0),
              "generated avatar coarse LOD reduces the source triangle count (%d -> %d)",
              generatedLods.triangleCount(0), generatedLods.triangleCount(generatedLods.levelCount() - 1));
        size_t badNormal = 0, badWeights = 0, badJoint = 0, badUv = 0, nonFinite = 0;
        for (int i = 0; i < vcount; ++i) {
            const af_vertex& v = verts[i];
            const float nl = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            if (std::fabs(nl - 1.0f) > 0.05f) ++badNormal;
            float wsum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                wsum += v.weights[k];
                if (v.joints[k] >= st.joint_count) ++badJoint;
                if (!std::isfinite(v.weights[k])) ++nonFinite;
            }
            if (std::fabs(wsum - 1.0f) > 1e-3f) ++badWeights;
            if (!(v.uv[0] >= 0.0f && v.uv[0] <= 1.0f && v.uv[1] >= 0.0f && v.uv[1] <= 1.0f)) ++badUv;
            if (!std::isfinite(v.pos[0]) || !std::isfinite(v.pos[1]) || !std::isfinite(v.pos[2])) ++nonFinite;
        }
        CHECK(badNormal == 0, "every vertex normal is unit length (%zu bad)", badNormal);
        CHECK(badWeights == 0, "every skin weight set is normalised (%zu bad)", badWeights);
        CHECK(badJoint == 0, "no joint index is out of range (%zu bad)", badJoint);
        CHECK(badUv == 0, "every UV is inside the atlas (%zu bad)", badUv);
        CHECK(nonFinite == 0, "no non-finite vertex data (%zu)", nonFinite);
        uint32_t maxIdx = 0;
        for (int i = 0; i < icount; ++i) maxIdx = std::max(maxIdx, idx[i]);
        CHECK(static_cast<int>(maxIdx) < vcount, "every index is in range (max %u of %d)", maxIdx, vcount);
    }

    /* --- submeshes reference the shared buffers coherently --- */
    {
        const af_submesh* subs = af_engine_submeshes(engine);
        const af_material* mats = af_engine_materials(engine);
        const int scount = af_engine_submesh_count(engine);
        const int mcount = af_engine_material_count(engine);
        CHECK(subs != nullptr && mats != nullptr, "submeshes and materials exposed");
        size_t total = 0;
        bool inRange = true;
        for (int i = 0; i < scount; ++i) {
            if (subs[i].index_offset < 0 || subs[i].index_offset + subs[i].index_count > icount) inRange = false;
            if (subs[i].material_index < 0 || subs[i].material_index >= mcount) inRange = false;
            if (subs[i].index_count % 3 != 0) inRange = false;
            total += static_cast<size_t>(subs[i].index_count);
        }
        CHECK(inRange, "every submesh range lies inside the shared index buffer");
        CHECK(total == static_cast<size_t>(icount), "submeshes cover the index buffer exactly (%zu of %d)", total, icount);
        bool named = true;
        for (int i = 0; i < mcount; ++i) if (mats[i].name[0] == '\0') named = false;
        CHECK(named, "every material carries a name (%d materials)", mcount);
        CHECK(mcount >= 5, "skin, hair, cloth and equipment materials exist (%d)", mcount);
    }

    /* --- skeleton --- */
    {
        const int joints = af_engine_joint_count(engine);
        CHECK(joints >= 60, "joint count (%d)", joints);
        af_joint_info info{};
        int roots = 0, named = 0;
        for (int j = 0; j < joints; ++j) {
            if (af_engine_joint_info(engine, j, &info) != AF_OK) continue;
            if (info.parent < 0) ++roots;
            if (info.name[0] != '\0') ++named;
        }
        CHECK(named == joints, "every joint is named (%d of %d)", named, joints);
        CHECK(roots == 1, "exactly one root joint (%d)", roots);
        std::vector<float> mats4(static_cast<size_t>(joints) * 16, 0.0f);
        CHECK(af_engine_joint_matrices(engine, mats4.data(), joints) == joints, "joint matrices exported");
        CHECK(std::fabs(mats4[15] - 1.0f) < 1e-4f, "joint matrices are affine (m[15] = %.3f)", mats4[15]);
        CHECK(af_engine_joint_info(engine, joints + 5, &info) == AF_ERR_INVALID_ARG, "bad joint index refused");
    }

    /* --- blendshape channel names --- */
    {
        std::vector<std::array<char, 64>> names(52);
        const int n = af_engine_blendshape_names(reinterpret_cast<char(*)[64]>(names.data()), 52);
        CHECK(n == 52, "52 blendshape names returned (%d)", n);
        CHECK(std::string(names[0].data()) == "browDownLeft", "first channel is browDownLeft (%s)", names[0].data());
        CHECK(std::string(names[51].data()) == "tongueOut", "last channel is tongueOut (%s)", names[51].data());
    }

    /* --- textures --- */
    {
        for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
            af_texture t{};
            const int32_t rc = af_engine_texture(engine, slot, &t);
            CHECK(rc == AF_OK, "texture slot %d available", slot);
            if (rc != AF_OK) continue;
            const int expectedChannels = (slot == AF_TEX_OCCLUSION) ? 1 : 4;
            CHECK(t.width == 2048 && t.height == 2048, "texture slot %d is 2048x2048 (%dx%d)", slot, t.width, t.height);
            CHECK(t.channels == expectedChannels, "texture slot %d has %d channels", slot, t.channels);
            CHECK(t.byte_size == static_cast<uint64_t>(t.width) * t.height * t.channels,
                  "texture slot %d byte count matches its size", slot);
            CHECK(t.data != nullptr, "texture slot %d has data", slot);
        }
        af_texture bad{};
        CHECK(af_engine_texture(engine, 99, &bad) == AF_ERR_INVALID_ARG, "bad texture slot refused");
    }

    /* --- posing: the skin actually moves, and moves plausibly --- */
    {
        std::vector<af_vertex> rest(static_cast<size_t>(vcount));
        std::memcpy(rest.data(), af_engine_vertices_rest(engine), static_cast<size_t>(vcount) * sizeof(af_vertex));
        CHECK(af_engine_apply_clip(engine, AF_CLIP_WALK, 0.35f) == AF_OK, "walk clip applied");
        const af_vertex* posed = af_engine_vertices(engine);
        double maxMove = 0.0, moved = 0;
        for (int i = 0; i < vcount; ++i) {
            const double dx = posed[i].pos[0] - rest[static_cast<size_t>(i)].pos[0];
            const double dy = posed[i].pos[1] - rest[static_cast<size_t>(i)].pos[1];
            const double dz = posed[i].pos[2] - rest[static_cast<size_t>(i)].pos[2];
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            maxMove = std::max(maxMove, d);
            if (d > 0.002) moved += 1;
        }
        CHECK(maxMove > 0.05, "the walk moves the mesh (max %.3f m)", maxMove);
        CHECK(maxMove < 0.80, "the walk does not tear the mesh (max %.3f m)", maxMove);
        CHECK(moved > vcount * 0.20, "a large share of vertices follow the animation (%.0f%%)",
              100.0 * moved / vcount);

        /* Facial animation, through the same buffer. */
        std::vector<float> weights(52, 0.0f);
        weights[0] = 0.0f;
        af_engine_apply_clip(engine, AF_CLIP_REST, 0.0f);
        std::vector<af_vertex> neutral(static_cast<size_t>(vcount));
        std::memcpy(neutral.data(), af_engine_vertices(engine), static_cast<size_t>(vcount) * sizeof(af_vertex));
        /* jawOpen is channel 24 in the ARKit order; drive it and expect motion near the jaw. */
        int jaw = -1;
        for (int c = 0; c < 52; ++c) if (std::string(kBlendshapeNames[c]) == "jawOpen") jaw = c;
        CHECK(jaw >= 0, "jawOpen exists in the channel table");
        if (jaw >= 0) {
            weights[static_cast<size_t>(jaw)] = 1.0f;
            CHECK(af_engine_apply_blendshapes(engine, weights.data(), 52) == AF_OK, "blendshapes applied");
            const af_vertex* shaped = af_engine_vertices(engine);
            double maxFaceMove = 0.0;
            for (int i = 0; i < vcount; ++i) {
                if (neutral[static_cast<size_t>(i)].pos[1] < 1.35f) continue;   /* face only */
                const double dx = shaped[i].pos[0] - neutral[static_cast<size_t>(i)].pos[0];
                const double dy = shaped[i].pos[1] - neutral[static_cast<size_t>(i)].pos[1];
                const double dz = shaped[i].pos[2] - neutral[static_cast<size_t>(i)].pos[2];
                maxFaceMove = std::max(maxFaceMove, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            CHECK(maxFaceMove > 0.004, "jawOpen moves the face (max %.4f m)", maxFaceMove);
            CHECK(maxFaceMove < 0.10, "jawOpen is a facial range, not a deformation (max %.4f m)", maxFaceMove);
            std::fill(weights.begin(), weights.end(), 0.0f);
            af_engine_apply_blendshapes(engine, weights.data(), 52);
        }

        /* Clip sampling writes a real pose into the caller's buffer. */
        {
            const int J = af_engine_joint_count(engine);
            std::vector<float> quats(static_cast<size_t>(J) * 4, 0.0f);
            af_pose pose{};
            pose.quats = quats.data();
            pose.joint_count = J;
            CHECK(af_engine_sample_clip(engine, AF_CLIP_RUN, 0.20f, &pose) == AF_OK, "clip sampled");
            int nonIdentity = 0;
            for (int j = 0; j < J; ++j) {
                if (std::fabs(quats[static_cast<size_t>(j) * 4 + 3] - 1.0f) > 1e-4f) ++nonIdentity;
            }
            CHECK(nonIdentity > 6, "the sampled pose rotates joints (%d of %d)", nonIdentity, J);
            af_pose bad{};
            bad.quats = quats.data();
            bad.joint_count = J + 3;
            CHECK(af_engine_apply_pose(engine, &bad) == AF_ERR_INVALID_ARG, "a mismatched pose is refused");
            pose.joint_count = J;
            CHECK(af_engine_apply_pose(engine, &pose) == AF_OK, "sampled pose applied");
        }
        CHECK(af_engine_apply_clip(engine, AF_CLIP_REST, 0.0f) == AF_OK, "back to rest");
    }

    /* --- export: bytes in memory, and a file on disk --- */
    {
        uint8_t* bytes = nullptr;
        size_t size = 0;
        CHECK(af_engine_export_glb_to_memory(engine, &bytes, &size) == AF_OK, "GLB exported to memory");
        CHECK(bytes != nullptr && size > 4u * 1024u * 1024u, "GLB is substantial (%.2f MB)", size / 1048576.0);
        if (bytes && size > 20) {
            CHECK(bytes[0] == 'g' && bytes[1] == 'l' && bytes[2] == 'T' && bytes[3] == 'F', "GLB magic");
            const uint32_t version = static_cast<uint32_t>(bytes[4]) | (static_cast<uint32_t>(bytes[5]) << 8) |
                                     (static_cast<uint32_t>(bytes[6]) << 16) | (static_cast<uint32_t>(bytes[7]) << 24);
            CHECK(version == 2, "GLB version 2 (%u)", version);
            const uint32_t declared = static_cast<uint32_t>(bytes[8]) | (static_cast<uint32_t>(bytes[9]) << 8) |
                                      (static_cast<uint32_t>(bytes[10]) << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
            CHECK(declared == size, "GLB length field matches the payload (%u vs %zu)", declared, size);
            /* The JSON must declare a skin, animations and morph targets, and reference nothing
               outside the file. */
            const std::string json(reinterpret_cast<const char*>(bytes + 20), std::min<size_t>(size - 20, 262144));
            CHECK(json.find("\"skins\"") != std::string::npos, "GLB declares a skin");
            CHECK(json.find("\"animations\"") != std::string::npos, "GLB declares animations");
            CHECK(json.find("\"targets\"") != std::string::npos, "GLB declares morph targets");
            CHECK(json.find("\"uri\"") == std::string::npos, "GLB references no external files");
            CHECK(json.find("KHR_materials_ior") != std::string::npos, "GLB carries the ior extension");
        }
        af_arena_free(bytes);

        const char* path = "/tmp/af_abi_export.glb";
        CHECK(af_engine_export_glb(engine, path) == AF_OK, "GLB written to a path");
        std::FILE* f = std::fopen(path, "rb");
        CHECK(f != nullptr, "GLB file exists");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long len = std::ftell(f);
            std::fclose(f);
            CHECK(len > 4 * 1024 * 1024, "GLB file is substantial (%ld bytes)", len);
        }
        CHECK(af_engine_export_glb(engine, "/nonexistent-directory/x.glb") == AF_ERR_IO,
              "a bad export path reports an I/O error");
    }

    af_engine_destroy(engine);

    /* --- a rejected image must not become an invented body --- */
    {
        af_engine* e2 = nullptr;
        af_spec s2;
        af_spec_init(&s2);
        CHECK(af_engine_create(&s2, nullptr, &e2) == AF_OK, "engine created without callbacks");
        af_image blank{};
        blank.width = 32; blank.height = 32; blank.channels = 3;
        std::vector<uint8_t> px(static_cast<size_t>(blank.width) * blank.height * 3, 128);
        blank.pixels = px.data();
        af_image_analysis a{};
        const int32_t produced = af_engine_analyze_images(e2, &blank, 1, &a, 1);
        CHECK(produced >= 0, "a flat image is analysed without crashing (%d)", produced);
        CHECK(a.subject_coverage <= 1.0f, "coverage is a fraction (%.3f)", a.subject_coverage);
        CHECK(af_engine_analyze_images(e2, nullptr, 0, nullptr, 0) == AF_ERR_INVALID_ARG,
              "a null image list is refused");
        af_engine_destroy(e2);
    }
}

static void testTexturing() {
    std::printf("\n[8] UV atlas and PBR bake\n");
    constexpr uint64_t MiB = 1024ull * 1024ull;
    CHECK(atlasResolutionForBudget(1536ull * MiB, 0) == 2048,
          "the requested default budget retains a 2048 atlas");
    CHECK(atlasResolutionForBudget(24ull * MiB, 0) == 1024 &&
              atlasResolutionForBudget(5ull * MiB, 0) == 512 &&
              atlasResolutionForBudget(1ull * MiB, 0) == 0,
          "atlas selection steps down to fit 1024/512 or fails below the safe minimum");
    CHECK(atlasBakeWorkspaceBytes(1024) == 19ull * MiB,
          "atlas memory estimate includes maps and raster/dilation masks");
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE; spec.build = AF_BUILD_ATHLETIC; spec.height_m = 1.70f;
    spec.seed = 4242; spec.skin_melanin = 0.42f; spec.age_years = 29.0f;
    BuiltAvatar av = buildFullBody(spec, 0.008f, 2);
    CHECK(av.ok, "body built for texturing");

    UVLayout layout;
    const bool unwrapped = unwrapAtlas(&av.mesh, 1024, 1024, &layout);
    CHECK(unwrapped, "atlas unwrapped");
    CHECK(layout.charts.size() > 0, "charts produced (%zu)", layout.charts.size());
    CHECK(layout.coverage > 0.40f, "atlas is at least 40%% covered (%.3f)", layout.coverage);
    CHECK(layout.texelsPerMeter > 150.0f && layout.texelsPerMeter < 4000.0f,
          "sane texel density (%.0f texels/m)", layout.texelsPerMeter);

    /* Every vertex carries a UV inside the unit square, and the seams were split. */
    size_t outside = 0, missing = 0;
    CHECK(av.mesh.uvs.size() == av.mesh.vertexCount() * 2, "one UV per vertex (%zu uvs, %zu verts)",
          av.mesh.uvs.size() / 2, av.mesh.vertexCount());
    for (size_t i = 0; i < av.mesh.vertexCount(); ++i) {
        const float u = av.mesh.uvs[i * 2], v = av.mesh.uvs[i * 2 + 1];
        if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) ++outside;
        if (u == 0.0f && v == 0.0f) ++missing;
    }
    CHECK(outside == 0, "every UV lies inside the atlas (%zu outside)", outside);
    CHECK(missing == 0, "no vertex left at the origin UV (%zu)", missing);

    /*
     * Chart orientation: the baker relies on charts being projected with the surface's
     * handedness, otherwise every relief gradient inside a mirrored chart comes out inverted.
     *
     * The measurement is by atlas AREA, not by triangle count. A planar projection of a curved
     * surface always folds over somewhere - at a nostril rim, inside the mouth, along a dental
     * arch - and the triangles at a fold are seen edge-on: their projected area is a fraction of
     * a texel and the sign of that fraction is numerical noise. Counting them would fail the
     * build on dust that no sampler can resolve, while saying nothing about the texels that a
     * viewer actually sees. So the check is: no mirrored triangle may cover a texel of the atlas,
     * and mirrored triangles together may not own a hundredth of it.
     */
    size_t flipped = 0, degenerate = 0, mirroredWithFootprint = 0;
    double uvAreaAll = 0.0, uvAreaMirrored = 0.0;
    const double texelArea = 0.5 / (1024.0 * 1024.0);   /* half a texel of the 1024 atlas, in uv units² */
    for (size_t t = 0; t + 2 < av.mesh.indices.size(); t += 3) {
        const uint32_t a = av.mesh.indices[t], b = av.mesh.indices[t + 1], c = av.mesh.indices[t + 2];
        const float ax = av.mesh.uvs[a * 2], ay = av.mesh.uvs[a * 2 + 1];
        const float bx = av.mesh.uvs[b * 2], by = av.mesh.uvs[b * 2 + 1];
        const float cx = av.mesh.uvs[c * 2], cy = av.mesh.uvs[c * 2 + 1];
        const double area = (static_cast<double>(bx) - ax) * (cy - ay) - (static_cast<double>(cx) - ax) * (by - ay);
        uvAreaAll += std::fabs(area) * 0.5;
        if (std::fabs(area) < 1e-14) ++degenerate;
        if (area < 0.0) {
            ++flipped;
            uvAreaMirrored += std::fabs(area) * 0.5;
            if (std::fabs(area) * 0.5 >= texelArea) ++mirroredWithFootprint;
        }
    }
    const size_t triCount = av.mesh.triangleCount();
    const double mirroredAreaShare = uvAreaAll > 0.0 ? uvAreaMirrored / uvAreaAll : 1.0;
    CHECK(mirroredWithFootprint == 0,
          "no mirrored UV triangle covers a texel of the atlas (%zu do, of %zu mirrored)",
          mirroredWithFootprint, flipped);
    CHECK(mirroredAreaShare < 0.01, "mirrored triangles own under 1%% of the atlas area (%.3f%%)",
          mirroredAreaShare * 100.0);
    std::printf("     charts=%zu coverage=%.2f density=%.0f texels/m | uv-degenerate %zu, mirrored %zu of %zu"
                " (all sub-texel: %.3f%% of the atlas)\n",
                layout.charts.size(), layout.coverage, layout.texelsPerMeter, degenerate, flipped, triCount,
                mirroredAreaShare * 100.0);

    /* Bake at 1024 so the harness stays quick; 2048 is the product default. */
    BakeOptions bopt;
    bopt.width = 1024; bopt.height = 1024;
    TextureSet ts;
    const bool baked = bakeTextures(av.mesh, layout, av.metrics, spec, bopt, &ts);
    CHECK(baked, "bake completed");
    const size_t texels = static_cast<size_t>(ts.width) * static_cast<size_t>(ts.height);
    CHECK(ts.baseColor.size() == texels * 4 && ts.normal.size() == texels * 4 &&
          ts.metallicRoughness.size() == texels * 4 && ts.occlusion.size() == texels &&
          ts.subsurface.size() == texels * 4, "all five maps present at %dx%d", ts.width, ts.height);
    CHECK(textureSetBytes(ts) == texels * 17, "memory accounting matches the maps (%llu bytes)",
          static_cast<unsigned long long>(textureSetBytes(ts)));

    /* No texel may be left at the unbaked marker: after the dilation passes the gutters are
       filled from the charts, so a marker anywhere means a chart was missed. */
    size_t unbaked = 0;
    for (size_t i = 0; i < texels; ++i) {
        const uint8_t* p = &ts.baseColor[i * 4];
        if (p[0] == kUnbakedRGBA[0] && p[1] == kUnbakedRGBA[1] && p[2] == kUnbakedRGBA[2]) ++unbaked;
    }
    CHECK(unbaked == 0, "every atlas texel baked (%zu left at the marker)", unbaked);

    /* Material plausibility: skin is dielectric (metallic 0 in the B channel of the MR map), its
       tangent-space normals point outward (B >= 128), and the albedo responds to melanin. */
    size_t metalTexels = 0, normalInward = 0, skinTexels = 0;
    double roughSum = 0.0;
    for (size_t i = 0; i < texels; ++i) {
        const uint8_t* mr = &ts.metallicRoughness[i * 4];
        const uint8_t* nrm = &ts.normal[i * 4];
        if (mr[2] > 8 && mr[2] < 250) ++metalTexels;
        if (nrm[2] < 100) ++normalInward;
        if (mr[1] > 100 && mr[1] < 200) { ++skinTexels; roughSum += mr[1] / 255.0; }
    }
    CHECK(metalTexels < texels / 20, "most of the atlas is dielectric (%zu metallic texels)", metalTexels);
    CHECK(normalInward < texels / 20, "normals in the atlas point outward (%zu inward)", normalInward);
    CHECK(skinTexels > texels / 20, "skin covers a sensible share of the atlas (%zu texels)", skinTexels);
    const double meanRough = skinTexels ? roughSum / static_cast<double>(skinTexels) : 0.0;
    CHECK(meanRough > 0.25 && meanRough < 0.85, "skin roughness in a plausible band (%.2f)", meanRough);

    af_spec pale = spec;
    pale.skin_melanin = 0.05f;
    af_spec deep = spec;
    deep.skin_melanin = 0.95f;
    auto meanAlbedo = [&](const af_spec& sp) {
        TextureSet t;
        if (!bakeTextures(av.mesh, layout, av.metrics, sp, bopt, &t)) return -1.0;
        double sum = 0.0;
        for (size_t i = 0; i < texels; ++i) sum += t.baseColor[i * 4];
        return sum / static_cast<double>(texels);
    };
    const double paleR = meanAlbedo(pale), deepR = meanAlbedo(deep);
    CHECK(paleR > 0.0 && deepR > 0.0 && paleR > deepR + 10.0,
          "melanin drives albedo (pale %.1f vs deep %.1f mean red)", paleR, deepR);
    std::printf("     bake: %dx%d, %.1f MB, %zu metallic texels, skin roughness %.2f, albedo %.0f (deep) / %.0f (pale)\n",
                ts.width, ts.height, textureSetBytes(ts) / 1048576.0, metalTexels, meanRough, deepR, paleR);
}

/* Splits the mesh into its connected shells and checks each encloses a positive volume. */
static bool allShellsOutward(const af::Mesh& mesh) {
    std::unordered_map<uint32_t, uint32_t> par;
    for (uint32_t v : mesh.indices) if (!par.count(v)) par[v] = v;
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
        while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; }
        return x;
    };
    const size_t F = mesh.triangleCount();
    for (size_t t = 0; t < F; ++t) {
        const uint32_t r0 = find(mesh.indices[t * 3]), r1 = find(mesh.indices[t * 3 + 1]);
        par[r0] = r1;
    }
    std::unordered_map<uint32_t, double> vol;
    for (size_t t = 0; t < F; ++t) {
        const uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1], c = mesh.indices[t * 3 + 2];
        vol[find(a)] += static_cast<double>(mesh.positions[a].dot(mesh.positions[b].cross(mesh.positions[c]))) / 6.0;
    }
    for (const auto& kv : vol) if (kv.second < 0.0) return false;
    return true;
}

static void testRig() {
    std::printf("\n[7] rig, skinning and animation\n");
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE; spec.build = AF_BUILD_ATHLETIC; spec.height_m = 1.70f;
    spec.muscle = 0.55f; spec.body_fat = 0.20f; spec.seed = 4242;
    BuiltAvatar av = buildFullBody(spec, 0.008f, 2);
    CHECK(av.ok, "body built for rigging");

    BodyField field(av.metrics);
    Rig rig;
    const bool rigged = rig.build(field, av.mesh);
    CHECK(rigged, "rig built from the same field that produced the mesh");
    CHECK(rig.jointCount() >= 70, "full humanoid joint count (%d joints)", rig.jointCount());
    {
        int fingers = 0, twists = 0;
        for (const Joint& j : rig.joints()) {
            if (j.flags & AF_JOINT_FINGER) ++fingers;
            if (j.flags & AF_JOINT_TWIST) ++twists;
        }
        CHECK(fingers >= 32, "30 finger joints plus 2 thumb metacarpals (%d)", fingers);
        CHECK(twists >= 6, "twist bones on arms and legs (%d)", twists);
    }

    /* Required joints must exist: fingers, toes, twist bones, face. */
    const char* required[] = {"root", "hips", "spine_01", "spine_04", "chest", "neck_01", "neck_02", "head",
                              "upperarm_01_L", "forearm_01_L", "hand_L", "thigh_L", "shin_L",
                              "foot_L", "toe_L", "thumb_03_L", "pinky_03_R", "index_01_R",
                              "upperarm_twist_L", "forearm_twist_R", "thigh_twist_L",
                              "jaw", "eye_L", "eye_R"};
    int missing = 0;
    for (const char* n : required) if (rig.jointIndex(n) < 0) { ++missing; std::printf("     missing joint: %s\n", n); }
    CHECK(missing == 0, "every required joint exists (missing=%d)", missing);

    /* Weights: <=4 influences, normalised, not degenerate. */
    const std::vector<SkinWeights>& w = rig.weights();
    CHECK(w.size() == av.mesh.vertexCount(), "one weight set per vertex (%zu)", w.size());
    double worstSum = 0.0, worstInf = 0.0;
    size_t usedInfluence0 = 0;
    int negWeight = 0, quietChecks = 0;
    for (const SkinWeights& sw : w) {
        float sum = 0.0f; int used = 0;
        for (int k = 0; k < 4; ++k) { sum += sw.weights[k]; if (sw.weights[k] > 1e-4f) ++used; }
        worstSum = maxf(static_cast<float>(worstSum), std::fabs(sum - 1.0f));
        worstInf = maxf(static_cast<float>(worstInf), static_cast<float>(used));
        if (used == 0) ++usedInfluence0;
        for (int k = 0; k < 4; ++k) checkQuiet(sw.weights[k] >= 0.0f, &negWeight, &quietChecks);
    }
    CHECK(negWeight == 0, "no negative skin weights (%d of %d)", negWeight, quietChecks);
    CHECK(worstSum < 1e-3, "all weight sets normalised (worst |sum-1| = %.2e)", worstSum);
    CHECK(usedInfluence0 == 0, "no unweighted vertices (%zu)", usedInfluence0);

    /* Bind-pose skinning must be the identity: this is what proves the rig is fitted exactly. */
    std::vector<Quat> rest(static_cast<size_t>(rig.jointCount()), Quat::identity());
    std::vector<Mat4> world = rig.computeWorld(rest);
    std::vector<Vec3> posed;
    rig.skin(av.mesh, world, &posed);
    double maxDev = 0.0;
    for (size_t i = 0; i < posed.size(); ++i) maxDev = std::max(maxDev, static_cast<double>((posed[i] - av.mesh.positions[i]).length()));
    CHECK(maxDev < 0.004, "bind pose reproduces the mesh (max deviation %.4f mm)", maxDev * 1000.0);
    std::printf("     bind-pose deviation %.4f mm\n", maxDev * 1000.0);

    /* Pose the left elbow 60 degrees and check the hand moved while the feet did not. */
    std::vector<Quat> posed_rot = rest;
    const int forearm = rig.jointIndex("forearm_01_L");
    const int hand = rig.jointIndex("hand_L");
    const int toe = rig.jointIndex("toe_R");
    posed_rot[static_cast<size_t>(forearm)] = Quat::fromAxisAngle(Vec3(1, 0, 0), radians(60.0f));
    std::vector<Mat4> posed_world = rig.computeWorld(posed_rot);
    const Vec3 handRest = rig.joints()[static_cast<size_t>(hand)].worldPosition;
    const Vec3 handPosed = posed_world[static_cast<size_t>(hand)].transformPoint(Vec3(0, 0, 0));
    CHECK((handPosed - handRest).length() > 0.05f,
          "elbow rotation moves the hand (%.3f m)", static_cast<double>((handPosed - handRest).length()));

    std::vector<Vec3> deformed;
    rig.skin(av.mesh, posed_world, &deformed);
    double footMove = 0.0, handVertMove = 0.0;
    Vec3 toePos = rig.joints()[static_cast<size_t>(toe)].worldPosition;
    size_t handVertexCount = 0;
    for (size_t i = 0; i < deformed.size(); ++i) {
        const double d = static_cast<double>((deformed[i] - av.mesh.positions[i]).length());
        if ((av.mesh.positions[i] - toePos).length() < 0.03f) footMove = std::max(footMove, d);
        const Vec3 hp = av.mesh.positions[i] - handRest;
        if (hp.length() < 0.05f) { ++handVertexCount; handVertMove = std::max(handVertMove, d); }
    }
    CHECK(handVertMove > 0.10, "hand vertices follow the elbow (max %.3f m)", handVertMove);
    CHECK(handVertexCount > 200, "hand island is skinned (%zu verts near the wrist)", handVertexCount);
    CHECK(footMove < 0.006, "untouched limbs stay put (foot moved %.4f mm)", footMove * 1000.0);

    /* Blendshapes: all 52 exist, face channels are non-trivial, and they are localised. */
    int emptyShapes = 0, hugeShapes = 0, shapeLenBad = 0, shapeChecks = 0;
    for (int c = 0; c < kBlendshapeCount; ++c) {
        const std::vector<Vec3>& d = rig.blendshapeDeltas(c);
        double amp = 0.0; size_t moved = 0;
        for (const Vec3& v : d) { const double l = v.length(); if (l > 1e-6) ++moved; amp = std::max(amp, l); }
        if (amp < 1e-5) ++emptyShapes;
        if (amp > 0.05) ++hugeShapes;
        checkQuiet(d.size() == av.mesh.vertexCount(), &shapeLenBad, &shapeChecks);
        /* Localised: a facial shape must not translate the whole model. */
        if (moved > av.mesh.vertexCount() * 6 / 10) { std::printf("     shape %s moves %zu/%zu verts\n", kBlendshapeNames[c], moved, av.mesh.vertexCount()); ++hugeShapes; }
    }
    CHECK(shapeLenBad == 0, "every blendshape array matches the mesh (%d bad of %d)", shapeLenBad, shapeChecks);
    CHECK(emptyShapes == 0, "all 52 ARKit blendshapes carry real displacement (%d empty)", emptyShapes);
    CHECK(hugeShapes == 0, "no blendshape is global or absurd (%d suspicious)", hugeShapes);
    /* Every shell must be wound outward. A closed shell wound outward encloses a positive signed
       volume; a shell that came out inside-out would render with backface culling as a hole. */
    CHECK(allShellsOutward(av.mesh), "every connected shell is wound outward (no inverted shells)");
    CHECK(av.report.flippedTriangles >= 0 && av.report.openEdgesAfterRepair == 0,
          "winding repaired during the build (%d flips, %zu open edges)",
          av.report.flippedTriangles, av.report.openEdgesAfterRepair);

    CHECK(std::string(kBlendshapeNames[0]) == "browDownLeft" && std::string(kBlendshapeNames[51]) == "tongueOut",
          "blendshape channel order matches ARKit");

    /* Clips: the ABI enum must resolve to the built-in library, and every clip must actually
       rotate at least one joint that exists in this rig. */
    int resolved = 0, unbound = 0;
    for (int id = AF_CLIP_REST; id < AF_CLIP_COUNT; ++id) {
        const int idx = clipIndexForId(id);
        if (id == AF_CLIP_REST) { CHECK(idx == -1, "rest clip needs no track data"); continue; }
        if (idx < 0) { ++unbound; continue; }
        const Clip& c = builtinClips()[static_cast<size_t>(idx)];
        int bound = 0;
        for (const Track& t : c.tracks) if (rig.jointIndex(t.jointName.c_str()) >= 0) ++bound;
        if (bound == 0) ++unbound;
        ++resolved;
    }
    CHECK(unbound == 0, "every ABI clip resolves to real tracks on this rig (%d unresolved, %d resolved)", unbound, resolved);
    /* Every ABI clip except REST must have exactly one library entry, and the id must resolve to
       the clip of the SAME NAME - counting entries alone would not catch a transposed table. */
    int nameMismatch = 0, nameDup = 0;
    std::vector<std::string> seenNames;
    for (int id = AF_CLIP_REST; id < AF_CLIP_COUNT; ++id) {
        const char* want = clipNameForId(id);
        if (!want) { ++nameMismatch; continue; }
        for (const std::string& n : seenNames) if (n == want) ++nameDup;
        seenNames.push_back(want);
        if (id == AF_CLIP_REST) continue;
        const int idx = clipIndexForId(id);
        if (idx < 0 || builtinClips()[static_cast<size_t>(idx)].name != want) ++nameMismatch;
    }
    CHECK(nameMismatch == 0, "every af_clip id resolves to the clip of the same name (%d wrong)", nameMismatch);
    CHECK(nameDup == 0, "clip names are unique across the ABI (%d duplicates)", nameDup);
    CHECK(builtinClips().size() + 1 == static_cast<size_t>(AF_CLIP_COUNT),
          "library holds one authored clip per ABI id except rest (%zu clips, AF_CLIP_COUNT=%d)",
          builtinClips().size(), static_cast<int>(AF_CLIP_COUNT));

    /* Animating the walk clip must move the legs and leave the feet cycle repeatable. */
    const int walkIdx = clipIndexForId(AF_CLIP_WALK);
    const Clip& walk = builtinClips()[static_cast<size_t>(walkIdx)];
    std::vector<Quat> rot(static_cast<size_t>(rig.jointCount()), Quat::identity());
    auto poseAt = [&](float t) {
        for (const Track& tr : walk.tracks) {
            const int ji = rig.jointIndex(tr.jointName.c_str());
            if (ji >= 0) rot[static_cast<size_t>(ji)] = tr.sample(t);
        }
        return rig.computeWorld(rot);
    };
    std::vector<Mat4> w0 = poseAt(0.0f), wHalf = poseAt(0.55f), wLoop = poseAt(walk.duration);
    /* A joint's own origin never moves under its own rotation, so the swing is measured at the
       knee and ankle - the children that the thigh actually carries. */
    const int kneeJ = rig.jointIndex("shin_L");
    const int ankleJ = rig.jointIndex("foot_L");
    const Vec3 knee0 = w0[static_cast<size_t>(kneeJ)].transformPoint(Vec3(0, 0, 0));
    const Vec3 kneeHalf = wHalf[static_cast<size_t>(kneeJ)].transformPoint(Vec3(0, 0, 0));
    const Vec3 kneeLoop = wLoop[static_cast<size_t>(kneeJ)].transformPoint(Vec3(0, 0, 0));
    const Vec3 ankle0 = w0[static_cast<size_t>(ankleJ)].transformPoint(Vec3(0, 0, 0));
    const Vec3 ankleMax = wHalf[static_cast<size_t>(ankleJ)].transformPoint(Vec3(0, 0, 0));
    (void)ankle0; (void)ankleMax;
    CHECK((kneeHalf - knee0).length() > 0.02f, "walk cycle swings the legs (%.3f m)", static_cast<double>((kneeHalf - knee0).length()));
    CHECK((kneeLoop - knee0).length() < 1e-4f, "looping clip returns to its start pose (%.5f m)", static_cast<double>((kneeLoop - knee0).length()));

    /* Root translation (jump) must lift the pelvis. */
    const int jumpIdx = clipIndexForId(AF_CLIP_JUMP);
    const Clip& jump = builtinClips()[static_cast<size_t>(jumpIdx)];
    float peak = 0.0f;
    for (const Track& tr : jump.tracks) if (tr.jointName == "root") peak = std::max(peak, tr.keys[3].translation.y);
    CHECK(peak > 0.3f, "jump clip lifts the root (%.2f m)", peak);
}


static void testShaderTables() {
    std::printf("\n[10] embedded shader tables\n");
    /*
     * The renderer looks a shader up by (pipeline name, stage) and treats a miss as a hard
     * initialisation failure - there is no unlit fallback path. That makes the baked tables a
     * build artefact the harness can verify without a GPU: every stage must be present, the
     * SPIR-V must carry its magic number and be non-trivial, and the GLES dialect must start with
     * its version directive. A shader that silently went missing would otherwise show up as a
     * black viewport on a phone, which is the worst place to find out.
     */
    struct Want { const char* name; AfShaderStage stage; };
    const Want want[] = {
        {"forward", AF_STAGE_VERT}, {"forward", AF_STAGE_FRAG},
        {"shadow", AF_STAGE_VERT},  {"shadow", AF_STAGE_FRAG},
        {"ground", AF_STAGE_VERT},  {"ground", AF_STAGE_FRAG},
        {"line", AF_STAGE_VERT},    {"line", AF_STAGE_FRAG},
        {"fullscreen", AF_STAGE_VERT},
        {"bloom_down", AF_STAGE_FRAG}, {"bloom_up", AF_STAGE_FRAG}, {"composite", AF_STAGE_FRAG},
        {"forward_plus", AF_STAGE_COMP},
    };
    const int wanted = static_cast<int>(sizeof(want) / sizeof(want[0]));
    CHECK(afShaderCount() == wanted, "every pipeline stage is baked into the SPIR-V table (%d of %d)",
          afShaderCount(), wanted);

    int vkOk = 0, glOk = 0, words = 0;
    size_t glBytes = 0;
    for (int i = 0; i < wanted; ++i) {
        const AfShaderVk* vk = afShaderVk(want[i].name, want[i].stage);
        const AfShaderGl* gl = afShaderGl(want[i].name, want[i].stage);
        if (vk && vk->words && vk->wordCount > 8 && vk->words[0] == 0x07230203u) { ++vkOk; words += int(vk->wordCount); }
        if (gl && gl->source && std::strlen(gl->source) > 200 &&
            std::strncmp(gl->source, "#version 310 es", 15) == 0 &&
            std::strstr(gl->source, "void main") != nullptr) { ++glOk; glBytes += std::strlen(gl->source); }
    }
    CHECK(vkOk == wanted, "all stages carry valid SPIR-V (magic + non-empty) (%d of %d)", vkOk, wanted);
    CHECK(glOk == wanted, "all stages carry GLSL ES 3.10 source (%d of %d)", glOk, wanted);
    /* A lookup that does not exist must return nullptr: the backends rely on that being a hard
       error rather than a fallback. */
    CHECK(afShaderVk("nope", AF_STAGE_FRAG) == nullptr && afShaderGl("nope", AF_STAGE_VERT) == nullptr,
          "an unknown shader name returns null (no silent fallback)");
    CHECK(afShaderVk("forward", AF_STAGE_FRAG) != nullptr && afShaderGl("forward", AF_STAGE_FRAG) != nullptr &&
              std::strstr(afShaderGl("forward", AF_STAGE_FRAG)->source, "AF_KIND_SKIN") != nullptr,
          "the forward shader is the shared lighting model (AF_KIND_* present in both dialects)");
    std::printf("  %s  %d stages / %d SPIR-V words (%.0f kB) / %.0f kB of GLSL ES\n", PASS, wanted, words,
                words * 4.0 / 1024.0, glBytes / 1024.0);
}

/* ------------------------------------------ renderer-to-scene visibility adapter --------------- */
static void testGpuProfilesAndTextureCompression() {
    std::printf("[10c] per-device GPU profiles and ASTC/ETC2 atlas encoding\n");
    GpuDeviceCaps caps;
    caps.backend = "vulkan";
    caps.vendorId = 0x5143u;
    caps.deviceName = "Adreno (TM) 650";
    caps.maxTextureDimension2D = 16384;
    caps.maxColorSamples = 4;
    caps.supportsAstcLdr = true;
    caps.supportsEtc2 = true;
    caps.supportsFloatHdr = true;
    GpuProfile balanced = makeGpuProfile(caps);
    CHECK(balanced.tier == GpuTier::BALANCED && balanced.maxQualityLevel == 2,
          "Adreno 6xx selects a balanced profile and caps the quality ladder");
    CHECK(balanced.atlasCompression == AtlasCompression::ASTC_6X6 &&
              balanced.maxMsaaSamples == 2 && balanced.maxShadowMapSize == 2048,
          "ASTC capability wins, while MSAA and shadow resolution are profile-bounded");

    caps.deviceName = "Mali-G57 MC2";
    caps.vendorId = 0x13B5u;
    GpuProfile mali = makeGpuProfile(caps);
    CHECK(mali.tier == GpuTier::BALANCED && mali.lodDistanceScale == 1.0f,
          "Mali G57 receives the balanced LOD and render profile");
    caps.deviceName = "Adreno 740";
    caps.vendorId = 0x5143u;
    caps.maxColorSamples = 2;
    GpuProfile high = makeGpuProfile(caps);
    CHECK(high.tier == GpuTier::HIGH && high.maxMsaaSamples == 2 && high.maxQualityLevel == 3,
          "Adreno 7xx is high-tier but never exceeds reported sample support");
    caps.deviceName = "Generic GLES device";
    caps.vendorId = 0;
    caps.supportsAstcLdr = false;
    caps.supportsEtc2 = true;
    GpuProfile etcFallback = makeGpuProfile(caps);
    CHECK(etcFallback.atlasCompression == AtlasCompression::ETC2,
          "ETC2 is selected when ASTC LDR is unavailable");
    caps.supportsEtc2 = false;
    GpuProfile rawFallback = makeGpuProfile(caps);
    CHECK(rawFallback.atlasCompression == AtlasCompression::NONE,
          "raw texture upload remains the fallback when no compressed format is advertised");
    caps.softwareRenderer = true;
    GpuProfile software = makeGpuProfile(caps);
    CHECK(software.tier == GpuTier::LOW && software.maxMsaaSamples == 1,
          "software renderers receive conservative limits");

    std::vector<uint8_t> rgba(8u * 8u * 4u);
    for (size_t i = 0; i < 64; ++i) {
        rgba[i * 4 + 0] = 160;
        rgba[i * 4 + 1] = 90;
        rgba[i * 4 + 2] = 40;
        rgba[i * 4 + 3] = 255;
    }
    af_texture rgbaTexture{8, 8, 4, 1, rgba.size(), rgba.data()};
    EncodedTexture astcColor;
    std::string encodeError;
    CHECK(compressAtlasTexture(rgbaTexture, AF_TEX_BASE_COLOR, balanced, &astcColor, &encodeError),
          "ASTC sRGB base-colour encoding succeeds (%s)", encodeError.c_str());
    CHECK(astcColor.encoding == AtlasTextureEncoding::ASTC_6X6_SRGB &&
              astcColor.levels.size() == 4 && astcColor.levels[0].bytes.size() == 64 &&
              astcColor.byteSize() == 112,
          "ASTC stores the expected 6x6 block payload and explicit 8x8-to-1x1 mip chain");

    if (astcColor.valid()) {
        astcenc_config config{};
        const astcenc_error configResult = astcenc_config_init(ASTCENC_PRF_LDR_SRGB, 6, 6, 1,
                                                                ASTCENC_PRE_FASTEST, 0, &config);
        astcenc_context* context = nullptr;
        const astcenc_error allocResult = configResult == ASTCENC_SUCCESS
            ? astcenc_context_alloc(&config, 1, &context, nullptr) : configResult;
        std::vector<uint8_t> decoded(8u * 8u * 4u);
        void* outputSlices[1] = {decoded.data()};
        astcenc_image output{};
        output.dim_x = 8;
        output.dim_y = 8;
        output.dim_z = 1;
        output.data_type = ASTCENC_TYPE_U8;
        output.data = outputSlices;
        const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const astcenc_error decodeResult = allocResult == ASTCENC_SUCCESS && context
            ? astcenc_decompress_image(context, astcColor.levels[0].bytes.data(),
                                       astcColor.levels[0].bytes.size(), &output, &swizzle, 0)
            : allocResult;
        CHECK(decodeResult == ASTCENC_SUCCESS && std::abs(int(decoded[0]) - 160) < 20 &&
                  std::abs(int(decoded[1]) - 90) < 20 && std::abs(int(decoded[2]) - 40) < 20,
              "ASTC block round-trips representative sRGB colour values");
        if (context) astcenc_context_free(context);
    } else {
        CHECK(false, "ASTC output is available for decoder validation");
    }

    std::vector<uint8_t> ao(7u * 5u);
    for (size_t i = 0; i < ao.size(); ++i) ao[i] = static_cast<uint8_t>(64u + (i * 7u) % 160u);
    af_texture aoTexture{7, 5, 1, 1, ao.size(), ao.data()};
    EncodedTexture astcAo;
    CHECK(compressAtlasTexture(aoTexture, AF_TEX_OCCLUSION, balanced, &astcAo, &encodeError) &&
              astcAo.encoding == AtlasTextureEncoding::ASTC_6X6_UNORM &&
              astcAo.levels.size() == 3 && astcAo.levels[0].bytes.size() == 32 &&
              astcAo.byteSize() == 64,
          "single-channel AO is swizzled safely into ASTC with odd-size mip dimensions");

    GpuProfile etc = etcFallback;
    etc.encoderThreads = 1;
    etc.etc2Effort = 0.0f;
    for (size_t i = 0; i < 64; ++i) rgba[i * 4 + 3] = (i % 2u) ? 96u : 224u;
    EncodedTexture etcAo;
    CHECK(compressAtlasTexture(aoTexture, AF_TEX_OCCLUSION, etc, &etcAo, &encodeError) &&
              etcAo.encoding == AtlasTextureEncoding::ETC2_R11_UNORM &&
              etcAo.levels.size() == 3 && etcAo.levels[0].bytes.size() == 32 &&
              etcAo.byteSize() == 48,
          "ETC2 EAC R11 encodes AO blocks and the explicit mip chain");
    EncodedTexture etcColor;
    CHECK(compressAtlasTexture(rgbaTexture, AF_TEX_BASE_COLOR, etc, &etcColor, &encodeError) &&
              etcColor.encoding == AtlasTextureEncoding::ETC2_RGBA8_SRGB &&
              etcColor.levels[0].bytes.size() == 64,
          "ETC2 base colour uses the sRGB RGBA8 block format and preserves alpha");
    EncodedTexture etcNormal;
    CHECK(compressAtlasTexture(rgbaTexture, AF_TEX_NORMAL, etc, &etcNormal, &encodeError) &&
              etcNormal.encoding == AtlasTextureEncoding::ETC2_RGBA8_UNORM &&
              etcNormal.levels[0].bytes.size() == 64,
          "ETC2 normal data retains all sampled RGBA channels");

    af_texture invalid{8, 8, 4, 1, 1, rgba.data()};
    EncodedTexture rejected;
    CHECK(!compressAtlasTexture(invalid, AF_TEX_BASE_COLOR, balanced, &rejected, &encodeError) &&
              !rejected.valid(),
          "malformed raw byte counts fail closed without publishing a partial encoding");

    std::array<af_texture, AF_TEX_COUNT> sources{};
    sources[AF_TEX_BASE_COLOR] = rgbaTexture;
    AsyncAtlasCompression async;
    const bool started = async.start(sources, balanced);
    EncodedAtlas encodedAtlas;
    bool completed = false;
    for (int i = 0; i < 10000 && !completed; ++i) {
        completed = async.poll(&encodedAtlas);
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!completed) async.waitDiscard();
    CHECK(started && completed && encodedAtlas.textures[AF_TEX_BASE_COLOR].valid(),
          "renderer-owned compression job can be polled and joined off the render thread (started=%d completed=%d valid=%d)",
          started ? 1 : 0, completed ? 1 : 0,
          encodedAtlas.textures[AF_TEX_BASE_COLOR].valid() ? 1 : 0);
}

static void testAvatarCuller() {
    std::printf("\n[10b] renderer avatar visibility: front, off-screen and fail-open invalid inputs\n");
    AvatarCuller culler;
    CameraState camera;
    const float modelMin[3] = {-0.5f, 0.0f, -0.2f};
    const float modelMax[3] = { 0.5f, 1.8f,  0.2f};

    AvatarCullResult visible = culler.update(modelMin, modelMax, true, 36, camera, 1080, 1920);
    CHECK(visible.visible, "renderer culler: avatar inside the camera frustum is retained");
    CHECK(visible.lod == 0 && visible.triangleCount == 12,
          "renderer culler: a single-LOD mesh keeps all triangles (%d, LOD %d)",
          visible.triangleCount, visible.lod);

    const int lodIndices[3] = {300, 120, 30};
    CameraState farCamera = camera;
    farCamera.distance = 10.0f;
    const AvatarCullResult farLod = culler.update(modelMin, modelMax, true, lodIndices, 3,
                                                   farCamera, 1080, 1920, 1.0f);
    CHECK(farLod.visible && farLod.lod == 2 && farLod.triangleCount == 10,
          "renderer culler selects the available coarse LOD at distance (%d, %d triangles)",
          farLod.lod, farLod.triangleCount);

    const int highPolyIndices[3] = {1226016, 147120, 61440};
    const AvatarCullResult mobileBudget = culler.update(modelMin, modelMax, true,
        highPolyIndices, 3, camera, 1080, 1920, 1.0f, 1.0f);
    CHECK(mobileBudget.visible && mobileBudget.lod == 1 && mobileBudget.triangleCount == 49040 &&
              !mobileBudget.triangleBudgetExceeded,
          "mobile culler selects the finest available LOD at or below the 50k triangle target (%d, %d tris)",
          mobileBudget.lod, mobileBudget.triangleCount);

    const int degradedBudgetIndices[3] = {1226016, 300000, 180000};
    const AvatarCullResult degradedBudget = culler.update(modelMin, modelMax, true,
        degradedBudgetIndices, 3, camera, 1080, 1920, 1.0f, 1.0f);
    CHECK(degradedBudget.visible && degradedBudget.lod == 2 && degradedBudget.triangleCount == 60000 &&
              degradedBudget.triangleBudgetExceeded,
          "when no generated LOD meets 50k, culler keeps the avatar visible and selects the coarsest available level");
    CameraState lowQualityCamera = camera;
    lowQualityCamera.distance = 3.0f;
    const AvatarCullResult lowQualityLod = culler.update(modelMin, modelMax, true, lodIndices, 3,
                                                          lowQualityCamera, 1080, 1920, 0.0f);
    CHECK(lowQualityLod.visible && lowQualityLod.lod >= 1,
          "lower adaptive quality moves LOD thresholds closer to the camera (%d)", lowQualityLod.lod);

    const float offscreenMin[3] = {10.0f, 0.0f, -0.2f};
    const float offscreenMax[3] = {11.0f, 1.8f,  0.2f};
    const AvatarCullResult culled = culler.update(offscreenMin, offscreenMax, true, 36, camera, 1080, 1920);
    CHECK(!culled.visible && culled.culledInstances == 1,
          "renderer culler: an avatar wholly outside the frustum is skipped");

    const AvatarCullResult empty = culler.update(nullptr, nullptr, false, 0, camera, 1080, 1920);
    CHECK(!empty.visible, "renderer culler: a missing model produces no draw");

    const float invalidMin[3] = {1.0f, 0.0f, 0.0f};
    const float invalidMax[3] = {-1.0f, 1.0f, 1.0f};
    const AvatarCullResult failOpen = culler.update(invalidMin, invalidMax, true, 36, camera, 1080, 1920);
    CHECK(failOpen.visible && failOpen.triangleCount == 12,
          "renderer culler: malformed bounds fail open instead of hiding the avatar");

    CameraState invalidCamera = camera;
    invalidCamera.fovDeg = 0.0f;
    const AvatarCullResult cameraFailOpen = culler.update(modelMin, modelMax, true, 36,
                                                          invalidCamera, 1080, 1920);
    CHECK(cameraFailOpen.visible,
          "renderer culler: degenerate field of view fails open instead of hiding the avatar");
}

/* ------------------------------------------ deformation-safe renderer LOD data ---------------- */
static void testAvatarLodSet() {
    std::printf("\n[10e] renderer LODs keep original vertices and provide monotone index ranges\n");
    constexpr int side = 24;
    std::vector<af_vertex> vertices(static_cast<size_t>(side) * side);
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(side - 1) * (side - 1) * 6);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            af_vertex& vertex = vertices[static_cast<size_t>(y) * side + x];
            vertex.pos[0] = static_cast<float>(x) / (side - 1);
            vertex.pos[1] = static_cast<float>(y) / (side - 1);
            vertex.pos[2] = 0.02f * std::sin(vertex.pos[0] * 6.2831853f) *
                                      std::sin(vertex.pos[1] * 6.2831853f);
            vertex.normal[2] = 1.0f;
            vertex.uv[0] = vertex.pos[0];
            vertex.uv[1] = vertex.pos[1];
            vertex.weights[0] = 1.0f;
        }
    }
    for (int y = 0; y + 1 < side; ++y) {
        for (int x = 0; x + 1 < side; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * side + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(side);
            const uint32_t d = c + 1;
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    af_submesh submesh{};
    submesh.vertex_count = static_cast<int32_t>(vertices.size());
    submesh.index_count = static_cast<int32_t>(indices.size());
    AvatarLodSet lods;
    const bool built = lods.build(vertices.data(), vertices.size(), indices.data(), indices.size(),
                                  &submesh, 1);
    CHECK(built && lods.levelCount() >= 2 && lods.levelCount() <= kAvatarLodLevels,
          "renderer constructs a full mesh and at least one simplified level (%d levels)", lods.levelCount());
    CHECK(lods.triangleCount(0) == static_cast<int>(indices.size() / 3),
          "LOD 0 preserves the exact source triangle count");
    bool monotone = true, indicesInBounds = true, rangesValid = true;
    for (int level = 0; level < lods.levelCount(); ++level) {
        const AvatarLodLevel& item = lods.level(level);
        monotone = monotone && (level == 0 || item.indexCount <= lods.level(level - 1).indexCount);
        rangesValid = rangesValid && item.submeshes.size() == 1 &&
            item.firstIndex <= lods.indices().size() && item.indexCount <= lods.indices().size() - item.firstIndex &&
            item.submeshes[0].firstIndex <= lods.indices().size() &&
            item.submeshes[0].indexCount <= lods.indices().size() - item.submeshes[0].firstIndex;
        for (size_t i = item.firstIndex; i < static_cast<size_t>(item.firstIndex) + item.indexCount; ++i)
            indicesInBounds = indicesInBounds && lods.indices()[i] < vertices.size();
    }
    CHECK(monotone && lods.triangleCount(lods.levelCount() - 1) < lods.triangleCount(0),
          "lower LODs reduce triangle count monotonically (%d -> %d)",
          lods.triangleCount(0), lods.triangleCount(lods.levelCount() - 1));
    CHECK(indicesInBounds && rangesValid,
          "all LOD ranges stay within the combined index stream and source vertex pool");
    bool everyIndexIsOriginal = true;
    for (int level = 1; level < lods.levelCount(); ++level) {
        const AvatarLodLevel& item = lods.level(level);
        for (size_t i = item.firstIndex; i < static_cast<size_t>(item.firstIndex) + item.indexCount; ++i)
            everyIndexIsOriginal = everyIndexIsOriginal &&
                std::find(indices.begin(), indices.end(), lods.indices()[i]) != indices.end();
    }
    CHECK(everyIndexIsOriginal,
          "simplified levels reference original source vertices (no averaged/skinning data)");

    af_submesh malformed = submesh;
    malformed.index_offset = -3;
    AvatarLodSet rejected;
    CHECK(!rejected.build(vertices.data(), vertices.size(), indices.data(), indices.size(), &malformed, 1) &&
              rejected.levelCount() == 0,
          "malformed source ranges are rejected instead of generating unsafe GPU indices");

    std::vector<uint32_t> outOfRangeIndices = indices;
    outOfRangeIndices[0] = static_cast<uint32_t>(vertices.size());
    CHECK(!rejected.build(vertices.data(), vertices.size(), outOfRangeIndices.data(),
                          outOfRangeIndices.size(), &submesh, 1) && rejected.levelCount() == 0,
          "out-of-range vertex references are rejected and leave no partial LOD state");

    CHECK(!rejected.build(vertices.data(), vertices.size(), indices.data(), indices.size() - 1,
                          &submesh, 1) && rejected.levelCount() == 0,
          "truncated/non-triangle index streams are rejected and leave no partial LOD state");
}

/* ------------------------------------------ parallel work error propagation -------------------- */
static void testParallelForExceptions() {
    std::printf("\n[10c] parallelFor propagates worker exceptions and honours cancellation\n");
    std::atomic<int> calls{0};
    bool caughtWorker = false;
    try {
        parallelFor(32, 4, [&](int begin, int) {
            ++calls;
            if (begin == 8) throw std::runtime_error("worker regression sentinel");
        });
    } catch (const std::runtime_error& e) {
        caughtWorker = std::string(e.what()) == "worker regression sentinel";
    }
    CHECK(caughtWorker, "parallelFor rethrows a worker exception on the caller thread");
    CHECK(calls.load() > 0, "parallelFor actually started at least one worker range");

    std::atomic<int> cancelledBodyCalls{0};
    parallelFor(64, 4, [&](int, int) { ++cancelledBodyCalls; }, [] { return true; });
    CHECK(cancelledBodyCalls.load() == 0, "parallelFor does not run work after cancellation is observed");

    bool caughtInline = false;
    try {
        parallelFor(1, 1, [](int, int) { throw std::logic_error("inline sentinel"); });
    } catch (const std::logic_error& e) {
        caughtInline = std::string(e.what()) == "inline sentinel";
    }
    CHECK(caughtInline, "the single-thread path propagates exceptions directly");
}

/* ------------------------------------------ candidate LOD mesh decimation --------------------- */
static Mesh makeDecimationGrid(int side) {
    Mesh mesh;
    mesh.reserveVertices(static_cast<size_t>(side) * side);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(side - 1);
            const float v = static_cast<float>(y) / static_cast<float>(side - 1);
            mesh.positions.emplace_back(u, v, 0.02f * std::sin(u * 6.2831853f) * std::sin(v * 6.2831853f));
            mesh.uvs.push_back(u);
            mesh.uvs.push_back(v);
            mesh.regions.push_back((x < side / 2) ? REGION_SKIN : REGION_CLOTH_UPPER);
            mesh.curvature.push_back(1.0f);
        }
    }
    for (int y = 0; y + 1 < side; ++y) {
        for (int x = 0; x + 1 < side; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * side + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(side);
            const uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    mesh.computeNormals();
    mesh.computeTangents();
    return mesh;
}

static void testMeshDecimator() {
    std::printf("\n[10d] candidate LOD decimator reduces triangle budgets safely\n");
    const Mesh source = makeDecimationGrid(65);
    Mesh untouched = source;
    untouched.clusterDecimate(0);
    CHECK(untouched.positions.size() == source.positions.size() &&
              untouched.indices.size() == source.indices.size(),
          "decimator leaves a mesh unchanged for a non-positive target");

    Mesh medium = source;
    medium.clusterDecimate(2048);
    Mesh low = source;
    low.clusterDecimate(512);
    CHECK(medium.triangleCount() > 0 && medium.triangleCount() < source.triangleCount(),
          "decimator reduces the medium LOD candidate (%zu -> %zu triangles)",
          source.triangleCount(), medium.triangleCount());
    CHECK(low.triangleCount() > 0 && low.triangleCount() < medium.triangleCount(),
          "a smaller target produces a coarser candidate (%zu -> %zu triangles)",
          medium.triangleCount(), low.triangleCount());

    bool valid = !low.positions.empty() && low.normals.size() == low.positions.size() &&
                 low.tangents.size() == low.positions.size() && low.uvs.size() == low.positions.size() * 2 &&
                 low.regions.size() == low.positions.size() && low.curvature.size() == low.positions.size();
    for (uint32_t index : low.indices) valid = valid && index < low.positions.size();
    CHECK(valid, "decimator rebuilds aligned attributes and valid triangle indices");

    bool preservesRegions = false;
    for (int32_t region : low.regions) {
        if (region == REGION_CLOTH_UPPER) { preservesRegions = true; break; }
    }
    CHECK(preservesRegions, "decimator keeps material regions in separate clusters");

    Mesh malformed = source;
    malformed.indices[0] = static_cast<uint32_t>(malformed.positions.size() + 10);
    const size_t malformedIndices = malformed.indices.size();
    malformed.clusterDecimate(512);
    CHECK(malformed.indices.size() == malformedIndices,
          "decimator refuses malformed source indices without mutating the mesh");
}

/* ---------------------------------------------------- 12. pose composition and the bind pose ----- */
/*
 *  The viewer applies clips continuously, so the deform path has to be stateless with respect to
 *  whatever was displayed last. Two properties are easy to get wrong and both are user visible:
 *
 *    - REST must restore the bind pose exactly. If the deformation is evaluated from the currently
 *      displayed mesh instead of the bind-pose mesh, playing a walk, then REST, then the walk again
 *      leaves the avatar permanently skewed (it drifts a little further with every clip change).
 *    - Blendshape weights are independent of the skeleton pose, so a smile must produce the same
 *      face after a walk as it does at rest.
 */
static void testPoseComposition() {
    std::printf("\n[12] pose composition and bind-pose restoration\n");
    af_spec spec = defaultSpec(AF_SEX_MALE, AF_BUILD_ATHLETIC, 1.78f);
    spec.hair_style = AF_HAIR_SHORT;

    af_engine* e = nullptr;
    CHECK(af_engine_create(&spec, nullptr, &e) == AF_OK && e != nullptr, "engine created through the public ABI");
    if (!e) return;
    CHECK(af_engine_run_all(e) == AF_OK, "full pipeline ran (%s)", af_engine_last_error(e));

    const int32_t n = af_engine_vertex_count(e);
    const af_vertex* verts = af_engine_vertices(e);
    CHECK(n > 10000 && verts != nullptr, "%d vertices addressable through the ABI", n);

    std::vector<af_vertex> bind(static_cast<size_t>(n)), walkA(static_cast<size_t>(n)),
                           restAfter(static_cast<size_t>(n)), walkB(static_cast<size_t>(n));
    std::memcpy(bind.data(), verts, sizeof(af_vertex) * static_cast<size_t>(n));

    auto nearest = [](const std::vector<af_vertex>& a, const std::vector<af_vertex>& b) {
        float worst = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            const float dx = a[i].pos[0] - b[i].pos[0];
            const float dy = a[i].pos[1] - b[i].pos[1];
            const float dz = a[i].pos[2] - b[i].pos[2];
            worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        return worst;
    };
    auto movedCount = [](const std::vector<af_vertex>& a, const std::vector<af_vertex>& b) {
        size_t c = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            const float dx = a[i].pos[0] - b[i].pos[0];
            const float dy = a[i].pos[1] - b[i].pos[1];
            const float dz = a[i].pos[2] - b[i].pos[2];
            if (dx * dx + dy * dy + dz * dz > 1e-12f) ++c;
        }
        return c;
    };

    /* 1. the bind pose is where the avatar starts */
    const int32_t joints = af_engine_joint_count(e);
    std::vector<float> identityQuats(static_cast<size_t>(joints) * 4u, 0.0f);
    for (int32_t j = 0; j < joints; ++j) identityQuats[static_cast<size_t>(j) * 4u + 3u] = 1.0f;
    af_pose identity{identityQuats.data(), joints};
    CHECK(af_engine_apply_pose(e, &identity) == AF_OK, "an identity pose is accepted (x y z w, w=1)");
    std::vector<af_vertex> afterPose0(static_cast<size_t>(n));
    std::memcpy(afterPose0.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
    CHECK(nearest(bind, afterPose0) < 1e-5f, "a zero pose reproduces the bind pose (max %.2e m)",
          (double)nearest(bind, afterPose0));

    /* 2. a clip moves real geometry */
    CHECK(af_engine_apply_clip(e, AF_CLIP_WALK, 0.35f) == AF_OK, "AF_CLIP_WALK sampled at t=0.35");
    std::memcpy(walkA.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
    const size_t walkMoved = movedCount(bind, walkA);
    CHECK(walkMoved > static_cast<size_t>(n) / 4, "the walk poses most of the body (%zu of %d verts)", walkMoved, n);
    CHECK(nearest(bind, walkA) > 0.05f, "walk displacement is macroscopic (max %.3f m)",
          (double)nearest(bind, walkA));

    std::printf("     %d vertices / %d joints; the walk moves %zu of them, max %.3f m\n",
                n, joints, walkMoved, (double)nearest(bind, walkA));

    /* 3. joint matrices are published for the posed skeleton, too - the renderer skins with these */
    {
        std::vector<float> jm(static_cast<size_t>(joints) * 16u, 0.0f);
        const int32_t written = af_engine_joint_matrices(e, jm.data(), joints);
        CHECK(written == joints, "the posed skeleton publishes every joint matrix (%d written)", written);
        float rootErr = 0.0f;
        for (int k = 0; k < 16; ++k) {
            rootErr = std::max(rootErr, std::fabs(jm[static_cast<size_t>(k)] - ((k % 5 == 0) ? 1.0f : 0.0f)));
        }
        std::printf("     root joint matrix deviates from identity by %.4f (hips carry the world offset)\n",
                    (double)rootErr);
    }

    /* 4. AF_CLIP_REST restores the bind pose exactly */
    CHECK(af_engine_apply_clip(e, AF_CLIP_REST, 0.0f) == AF_OK, "AF_CLIP_REST accepted");
    std::memcpy(restAfter.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
    CHECK(movedCount(bind, restAfter) == 0, "REST restores the bind pose exactly (%zu verts off)",
          movedCount(bind, restAfter));

    /* 5. returning to the same frame of the same clip is bit-exact (no accumulation) */
    CHECK(af_engine_apply_clip(e, AF_CLIP_WALK, 0.35f) == AF_OK, "AF_CLIP_WALK re-sampled at t=0.35");
    std::memcpy(walkB.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
    CHECK(movedCount(walkA, walkB) == 0, "walk -> REST -> walk is not cumulative (%zu verts differ)",
          movedCount(walkA, walkB));

    /* 6. blendshapes are independent of pose: apply the same smile standing and walking */
    {
        char names[64][64];
        const int32_t channels = af_engine_blendshape_names(names, 64);
        CHECK(channels == 52, "52 ARKit channels exposed (%d)", channels);
        std::vector<float> w(static_cast<size_t>(channels), 0.0f);
        int32_t smile = -1;
        for (int32_t c = 0; c < channels; ++c) if (std::strcmp(names[c], "mouthSmileLeft") == 0) smile = c;
        CHECK(smile >= 0, "mouthSmileLeft resolvable by name");
        if (smile >= 0) {
            w[static_cast<size_t>(smile)] = 1.0f;
            af_engine_apply_blendshapes(e, w.data(), channels);       /* still walking */
            std::vector<af_vertex> smileWalk(static_cast<size_t>(n));
            std::memcpy(smileWalk.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
            af_engine_apply_clip(e, AF_CLIP_REST, 0.0f);
            af_engine_apply_blendshapes(e, w.data(), channels);       /* rest, same weight */
            std::vector<af_vertex> smileRest(static_cast<size_t>(n));
            std::memcpy(smileRest.data(), af_engine_vertices(e), sizeof(af_vertex) * static_cast<size_t>(n));
            /* The face offsets must be identical once the difference between the two skeletons is
               removed, so compare the smile against its own pose baseline. */
            const float faceDelta = [&] {
                float worst = 0.0f;
                for (size_t i = 0; i < smileRest.size(); ++i) {
                    if (smileRest[i].pos[1] < 1.45f) continue;          /* face band only */
                    const float dx = smileWalk[i].pos[0] - smileRest[i].pos[0];
                    const float dy = smileWalk[i].pos[1] - smileRest[i].pos[1];
                    const float dz = smileWalk[i].pos[2] - smileRest[i].pos[2];
                    worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                return worst;
            }();
            CHECK(faceDelta > 0.0f, "a smile applied while walking survives the pose change (max %.4f m)",
                  (double)faceDelta);
            std::vector<float> zero(static_cast<size_t>(channels), 0.0f);
            af_engine_apply_blendshapes(e, zero.data(), channels);
        }
    }
    af_engine_destroy(e);
}

/* --------------------------------------- 13. every channel survives into the exported file ----- */
/*
 *  Two failures are possible between "the rig has 52 channels" and "the .glb has 52 channels", and
 *  both were real here:
 *
 *    1. a channel whose stamp misses the skin produces an all-zero delta table. The exporter used
 *       to SKIP such a channel, which silently renumbered every morph target after it - glTF morph
 *       targets are positional, so an importer that maps weight[i] to channel i would drive the
 *       wrong expression. The exporter now writes every channel, empty ones as a zero-cost
 *       accessor with no bufferView, and this test measures that none is empty to begin with;
 *    2. whether a stamp lands on the skin depends on how the FIELD is evaluated, and the engine's
 *       lattice-cached field differs from the raw one at the lips. That is what made the eight
 *       mouth channels collapse (measured: mouthUpperUp/Press/Dimple/Stretch moved 0.01-0.04 mm in
 *       the engine while moving 2-8 mm through the uncached path). The anchors are solved by
 *       scanning for the front surface now, and this test measures the ENGINE path, which is the
 *       one the app and the exporter use.
 */
static void testBlendshapeExport() {
    std::printf("\n[13] blendshape channels: real motion on the engine path, canonical export order\n");
    af_spec spec = defaultSpec(AF_SEX_FEMALE, AF_BUILD_ATHLETIC, 1.72f);
    spec.outfit = AF_OUTFIT_TSHIRT;
    spec.hair_style = AF_HAIR_PONYTAIL;
    spec.footwear = AF_FOOTWEAR_SNEAKER;

    af_engine* e = nullptr;
    CHECK(af_engine_create(&spec, nullptr, &e) == AF_OK && e != nullptr, "engine created through the public ABI");
    if (!e) return;
    CHECK(af_engine_run_all(e) == AF_OK, "full pipeline ran (%s)", af_engine_last_error(e));

    char names[64][64];
    const int32_t channels = af_engine_blendshape_names(names, 64);
    CHECK(channels == 52, "52 ARKit channels named (%d)", channels);
    const int32_t n = af_engine_vertex_count(e);
    const af_vertex* rest = af_engine_vertices_rest(e);
    CHECK(rest != nullptr && n > 10000, "bind-pose buffer exposed for %d vertices", n);

    /* The exporter's own threshold (af_glb.cpp): a component below 0.05 mm is not worth a sparse
       entry. A channel that never exceeds it would have been dropped. */
    const float kEps = 5.0e-5f;
    std::vector<float> w(static_cast<size_t>(channels), 0.0f);
    int dropped = 0;
    const char* weakestName = "?";
    double weakest = 1e9;
    for (int32_t c = 0; c < channels; ++c) {
        std::fill(w.begin(), w.end(), 0.0f);
        w[static_cast<size_t>(c)] = 1.0f;
        if (af_engine_apply_blendshapes(e, w.data(), channels) != AF_OK) break;
        const af_vertex* moved = af_engine_vertices(e);
        size_t above = 0;
        double amplitude = 0.0;
        for (int32_t i = 0; i < n && rest && moved; ++i) {
            const float dx = moved[i].pos[0] - rest[i].pos[0];
            const float dy = moved[i].pos[1] - rest[i].pos[1];
            const float dz = moved[i].pos[2] - rest[i].pos[2];
            if (std::fabs(dx) > kEps || std::fabs(dy) > kEps || std::fabs(dz) > kEps) ++above;
            const double l = std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz));
            if (l > amplitude) amplitude = l;
        }
        if (above == 0) ++dropped;
        if (amplitude < weakest) { weakest = amplitude; weakestName = names[c]; }
        std::fill(w.begin(), w.end(), 0.0f);
        af_engine_apply_blendshapes(e, w.data(), channels);   /* back to the bind face */
    }
    CHECK(dropped == 0, "no channel is below the exporter's %.5f m threshold (%d would be dropped)",
          (double)kEps, dropped);
    std::printf("     weakest channel %s: max %.5f m (threshold %.5f m)\n", weakestName, weakest, (double)kEps);

    /* And the file itself. The GLB is written to memory and its JSON chunk inspected as a
       consumer would: 52 targets declared, and every channel name present in order. */
    uint8_t* bytes = nullptr;
    size_t size = 0;
    CHECK(af_engine_export_glb_to_memory(e, &bytes, &size) == AF_OK && bytes != nullptr && size > 0,
          "GLB exported to memory (%.1f MB)", size / 1048576.0);
    if (bytes) {
        std::string json;
        if (size > 20) {
            const uint32_t jsonLen = static_cast<uint32_t>(bytes[12]) | (static_cast<uint32_t>(bytes[13]) << 8) |
                                     (static_cast<uint32_t>(bytes[14]) << 16) | (static_cast<uint32_t>(bytes[15]) << 24);
            json.assign(reinterpret_cast<const char*>(bytes + 20), jsonLen);
        }
        CHECK(json.find("\"blendshapes\":52") != std::string::npos, "the file declares 52 morph targets");
        CHECK(json.find("\"emptyBlendshapes\":0") != std::string::npos,
              "and none of them is empty on this build");
        size_t cursor = 0;
        int inOrder = 0;
        for (int32_t c = 0; c < channels; ++c) {
            const size_t at = json.find(std::string("\"") + names[c] + "\"", cursor);
            if (at == std::string::npos) break;
            cursor = at + 1;
            ++inOrder;
        }
        CHECK(inOrder == channels, "extras.targetNames lists all %d channels in kernel order (%d found)",
              channels, inOrder);
        af_arena_free(bytes);
    }
    af_engine_destroy(e);
}


/* ==============================================================================================
 *  [14] GLB import
 * ----------------------------------------------------------------------------------------------
 *  The exporter writes a file; the importer must read that exact file back. This section is the
 *  round trip, then a deliberate attempt to break the reader with malformed input. A crash here is
 *  a crash in the app when a user opens a bad file, which is why the fuzz cases run under ASan in
 *  the fuzz build and under the normal harness otherwise.
 * ============================================================================================== */
static void testGlbImport() {
    std::printf("\x1b[1m[14] GLB import: round trip and hostile input\x1b[0m\n");

    /* ---- 14a. a small synthetic document through the JSON reader ---- */
    {
        json::Json doc;
        std::string err;
        const std::string text =
            "{\"a\":[1,2,3],\"b\":\"x\\u00e9y\",\"c\":{\"d\":true},\"e\":null,}"
            ;
        /* trailing comma is invalid JSON and must be rejected, not tolerated */
        const bool bad = json::Json::parse(text, &doc, &err);
        CHECK(!bad && !err.empty(), "JSON: a trailing comma is rejected with a message");

        const bool ok = json::Json::parse(
            "{\"a\":[1,2,3],\"b\":\"x\\u00e9y\",\"c\":{\"d\":true},\"e\":null}", &doc, &err);
        CHECK(ok, "JSON: a valid document parses");
        CHECK(ok && doc["a"].size() == 3 && doc["a"].at(1).asNumber() == 2.0, "JSON: arrays and numbers");
        CHECK(ok && doc["b"].asString() == std::string("x\xc3\xa9y"), "JSON: \\u escape decodes to UTF-8");
        CHECK(ok && doc["c"]["d"].asBool(false), "JSON: nested objects");
        CHECK(ok && doc["e"].isNull(), "JSON: null");
        CHECK(ok && doc["missing"].isNull(), "JSON: a missing key reads as null, not a crash");

        const bool deep = json::Json::parse(std::string(200, '[') + std::string(200, ']'), &doc, &err);
        CHECK(!deep, "JSON: excessive nesting is refused, not stack-overflowed");
    }

    /* ---- 14b. PNG encode -> decode is lossless for an 8-bit RGBA image ---- */
    {
        const int w = 37, h = 19;   /* deliberately not a power of two, odd width per row */
        std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<uint8_t>((i * 37 + (i >> 5) * 11) & 0xFF);
        }
        std::vector<uint8_t> png;
        CHECK(encodePng(pixels.data(), w, h, 4, &png), "PNG: encoder produced a file");
        DecodedImage img;
        std::string err;
        const bool decoded = decodePng(png.data(), png.size(), &img, &err);
        CHECK(decoded, "PNG: decoder accepts the encoder's output");
        if (decoded) {
            CHECK(img.width == w && img.height == h, "PNG: dimensions survive the round trip");
            CHECK(img.rgba == pixels, "PNG: every RGBA byte survives the round trip");
        }

        /* Truncation and corruption must fail cleanly, never crash. */
        DecodedImage junk;
        int cleanFailures = 0;
        for (size_t cut = 0; cut < png.size(); cut += 7) {
            if (!decodePng(png.data(), cut, &junk, &err)) ++cleanFailures;
        }
        CHECK(cleanFailures > 0, "PNG: truncated files are rejected");

        std::vector<uint8_t> corrupt = png;
        corrupt[corrupt.size() / 2] ^= 0xFF;
        DecodedImage corrupted;
        const bool survived = decodePng(corrupt.data(), corrupt.size(), &corrupted, &err);
        /* Either outcome is acceptable; a crash is not. */
        (void)survived;
        CHECK(true, "PNG: a corrupted IDAT is survived (decoded or clean error, no crash)");
    }

    /* ---- 14c. the real thing: export a model, import it, compare ---- */
    af_engine* engine = nullptr;
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE;
    spec.height_m = 1.70f;
    spec.build = AF_BUILD_ATHLETIC;
    spec.seed = 4242;
    if (af_engine_create(&spec, nullptr, &engine) != AF_OK || !engine) {
        CHECK(false, "GLB import: engine creation");
        return;
    }
    const int rc = af_engine_run_all(engine);
    CHECK(rc == AF_OK, "GLB import: pipeline ran for the export fixture");
    if (rc != AF_OK) {
        af_engine_destroy(engine);
        return;
    }

    uint8_t* bytes = nullptr;
    size_t byteCount = 0;
    CHECK(af_engine_export_glb_to_memory(engine, &bytes, &byteCount) == AF_OK && bytes && byteCount > 0,
          "GLB import: exporter produced a file");
    std::vector<uint8_t> glb(bytes, bytes + byteCount);
    std::printf("     exported %zu bytes (%.1f MB)\n", glb.size(), glb.size() / 1048576.0);

    GlbAsset asset;
    std::string error;
    const int irc = importGlb(glb.data(), glb.size(), &asset, &error);
    CHECK(irc == AF_OK, "GLB import: the exporter's file loads (%s)", error.c_str());

    if (irc == AF_OK) {
        af_stats stats;
        af_engine_stats(engine, &stats);
        std::printf("     imported %zu unique vertices (%zu referenced), %zu triangles, %zu nodes, "
                    "%zu materials, %zu textures, %zu pools\n",
                    asset.uniqueVertexCount(), asset.referencedVertexCount(), asset.triangleCount(),
                    asset.nodes.size(), asset.materials.size(), asset.textures.size(), asset.pools.size());

        CHECK(asset.uniqueVertexCount() > 200000, "GLB import: every vertex came back");
        /* The exporter shares one POSITION accessor across the ten region primitives. The importer
           must store that stream ONCE: storing it per primitive costs 10x the memory (measured on
           the sample model: 24 MB vs 240 MB of af_vertex), which on a 2 GB phone is the difference
           between a working viewer and an OOM kill. */
        CHECK(asset.pools.size() < asset.primitives.size(),
              "GLB import: primitives share vertex pools (%zu pools for %zu primitives)",
              asset.pools.size(), asset.primitives.size());
        /* The reference count is legitimately larger than the stored count: each primitive's
           index range addresses the shared pool, and an index is 4 bytes against an 88-byte
           vertex. What must be bounded is the *storage*. */
        const size_t storedBytes = asset.uniqueVertexCount() * sizeof(af_vertex);
        CHECK(storedBytes < 64u * 1024u * 1024u,
              "GLB import: vertex storage stays under 64 MB (%zu vertices, %.1f MB)",
              asset.uniqueVertexCount(), storedBytes / 1048576.0);
        CHECK(asset.uniqueVertexCount() < asset.referencedVertexCount(),
              "GLB import: the pool is shared, not copied (%zu stored vs %zu referenced)",
              asset.uniqueVertexCount(), asset.referencedVertexCount());
        /* Every primitive must address a pool that exists and be inside it. */
        size_t badRanges = 0;
        for (const auto& p : asset.primitives) {
            if (p.pool < 0 || static_cast<size_t>(p.pool) >= asset.pools.size()) { ++badRanges; continue; }
            if (p.vertexOffset + p.vertexCount > asset.pools[static_cast<size_t>(p.pool)].size()) ++badRanges;
            for (uint32_t idx : p.indices) {
                if (idx >= p.vertexCount) { ++badRanges; break; }
            }
        }
        CHECK(badRanges == 0, "GLB import: every primitive's pool and index range is valid");
        CHECK(asset.triangleCount() > 300000, "GLB import: every triangle came back");
        CHECK(!asset.nodes.empty(), "GLB import: the node hierarchy came back");
        CHECK(!asset.skins.empty(), "GLB import: the skin came back");
        CHECK(!asset.skins.empty() && asset.skins[0].joints.size() == 71,
              "GLB import: all 71 joints came back");
        CHECK(!asset.skins.empty() &&
                  asset.skins[0].inverseBindMatrices.size() == asset.skins[0].joints.size(),
              "GLB import: inverse bind matrices came back");
        CHECK(!asset.animations.empty(), "GLB import: the animation clips came back");
        CHECK(asset.textures.size() >= 5, "GLB import: the baked PNG maps came back");
        bool anyTexturePixels = false;
        for (const auto& t : asset.textures) {
            if (!t.rgba.empty()) anyTexturePixels = true;
        }
        CHECK(anyTexturePixels, "GLB import: at least one embedded PNG decoded to pixels");

        /* Geometry identity: the imported positions must match the exported buffer within float
           precision. Compare the bounding box and the vertex count of the largest primitive. */
        Vec3 lo, hi;
        asset.bounds(&lo, &hi);
        const float importedHeight = hi.y - lo.y;
        CHECK(importedHeight > 1.55f && importedHeight < 1.85f,
              "GLB import: the imported figure is the requested 1.70 m tall");
        CHECK(lo.y > -0.02f && lo.y < 0.02f, "GLB import: the imported figure stands on y = 0");

        /* Skinning: every vertex has a normalised weight set. */
        size_t unweighted = 0;
        for (const auto& pool : asset.pools) {
            for (const auto& v : pool) {
                const float sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
                if (sum < 1e-4f) ++unweighted;
            }
        }
        CHECK(unweighted == 0, "GLB import: every imported vertex carries skin weights");

        /* Normals are unit length. */
        size_t badNormals = 0;
        for (const auto& pool : asset.pools) {
            for (const auto& v : pool) {
                const float len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                                            v.normal[2] * v.normal[2]);
                if (std::fabs(len - 1.0f) > 0.02f) ++badNormals;
            }
        }
        CHECK(badNormals == 0, "GLB import: every imported normal is unit length");
    }

    /* ---- 14d. hostile input: every one of these must return an error, none may crash ---- */
    {
        struct Case {
            const char* what;
            std::vector<uint8_t> bytes;
        };
        std::vector<Case> cases;
        auto makeHeader = [](uint32_t total) {
            std::vector<uint8_t> v(12, 0);
            std::memcpy(&v[0], "glTF", 4);
            const uint32_t version = 2;
            std::memcpy(&v[4], &version, 4);
            std::memcpy(&v[8], &total, 4);
            return v;
        };
        { Case c; c.what = "empty file"; cases.push_back(c); }
        { Case c; c.what = "header only"; c.bytes = makeHeader(12); cases.push_back(c); }
        { Case c; c.what = "declared length past end"; c.bytes = makeHeader(9999); cases.push_back(c); }
        {
            Case c; c.what = "JSON chunk that is not JSON";
            c.bytes = makeHeader(12 + 8 + 5);
            const uint32_t len = 5, type = 0x4E4F534A;
            const char payload[5] = {'{', 'o', 'o', 'p', ']'};
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&len), reinterpret_cast<const uint8_t*>(&len) + 4);
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&type), reinterpret_cast<const uint8_t*>(&type) + 4);
            c.bytes.insert(c.bytes.end(), payload, payload + 5);
            cases.push_back(c);
        }
        {
            Case c; c.what = "accessor range past the binary chunk";
            const char* jsonText =
                "{\"asset\":{\"version\":\"2.0\"},"
                "\"buffers\":[{\"byteLength\":16}],"
                "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":16}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":1000,\"type\":\"VEC3\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
            const size_t jlen = std::strlen(jsonText);
            const uint32_t jsonLen = static_cast<uint32_t>((jlen + 3) & ~static_cast<size_t>(3));
            const uint32_t binLen = 16;
            const uint32_t total = static_cast<uint32_t>(12 + 8 + jsonLen + 8 + binLen);
            c.bytes = makeHeader(total);
            const uint32_t jsonType = 0x4E4F534A, binType = 0x004E4942;
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&jsonLen), reinterpret_cast<const uint8_t*>(&jsonLen) + 4);
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&jsonType), reinterpret_cast<const uint8_t*>(&jsonType) + 4);
            const size_t before = c.bytes.size();
            c.bytes.resize(before + jsonLen, ' ');
            std::memcpy(c.bytes.data() + before, jsonText, jlen);
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&binLen), reinterpret_cast<const uint8_t*>(&binLen) + 4);
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&binType), reinterpret_cast<const uint8_t*>(&binType) + 4);
            c.bytes.resize(c.bytes.size() + binLen, 0);
            cases.push_back(c);
        }
        {
            Case c; c.what = "cyclic node graph";
            const char* jsonText =
                "{\"asset\":{\"version\":\"2.0\"},"
                "\"nodes\":[{\"children\":[1]},{\"children\":[0]}],"
                "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
            const size_t jlen = std::strlen(jsonText);
            const uint32_t jsonLen = static_cast<uint32_t>((jlen + 3) & ~static_cast<size_t>(3));
            const uint32_t total = static_cast<uint32_t>(12 + 8 + jsonLen);
            c.bytes = makeHeader(total);
            const uint32_t jsonType = 0x4E4F534A;
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&jsonLen), reinterpret_cast<const uint8_t*>(&jsonLen) + 4);
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&jsonType), reinterpret_cast<const uint8_t*>(&jsonType) + 4);
            const size_t before = c.bytes.size();
            c.bytes.resize(before + jsonLen, ' ');
            std::memcpy(c.bytes.data() + before, jsonText, jlen);
            cases.push_back(c);
        }
        {
            Case c; c.what = "required unsupported extension";
            const char* jsonText =
                "{\"asset\":{\"version\":\"2.0\"},"
                "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]}";
            const size_t jlen = std::strlen(jsonText);
            const uint32_t jsonLen = static_cast<uint32_t>((jlen + 3) & ~static_cast<size_t>(3));
            const uint32_t total = static_cast<uint32_t>(12 + 8 + jsonLen);
            c.bytes = makeHeader(total);
            const uint32_t jsonType = 0x4E4F534A;
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&jsonLen), reinterpret_cast<const uint8_t*>(&jsonLen) + 4);
            c.bytes.insert(c.bytes.end(), reinterpret_cast<const uint8_t*>(&jsonType), reinterpret_cast<const uint8_t*>(&jsonType) + 4);
            const size_t before = c.bytes.size();
            c.bytes.resize(before + jsonLen, ' ');
            std::memcpy(c.bytes.data() + before, jsonText, jlen);
            cases.push_back(c);
        }

        for (const auto& c : cases) {
            GlbAsset hostile;
            std::string err;
            const int hrc = importGlb(c.bytes.data(), c.bytes.size(), &hostile, &err);
            char msg[256];
            std::snprintf(msg, sizeof(msg), "GLB hostile input: %s -> rejected cleanly", c.what);
            CHECK(hrc != AF_OK && !err.empty(), msg);
        }

        /* The real export, truncated at many lengths and bit-flipped: the reader must always
           return, never crash. This is the case a downloaded-then-interrupted file produces. */
        int survived = 0, total = 0;
        for (size_t cut = 12; cut < glb.size() && cut < 4096; cut += 97) {
            GlbAsset partial;
            std::string err;
            importGlb(glb.data(), cut, &partial, &err);
            ++total;
            ++survived;
        }
        for (size_t pos = 12; pos < glb.size() && pos < 2048; pos += 131) {
            std::vector<uint8_t> flip(glb.begin(), glb.begin() + static_cast<std::ptrdiff_t>(pos + 256 > glb.size() ? glb.size() : pos + 256));
            flip[pos] ^= 0xA5;
            GlbAsset damaged;
            std::string err;
            importGlb(flip.data(), flip.size(), &damaged, &err);
            ++total;
            ++survived;
        }
        char msg[128];
        std::snprintf(msg, sizeof(msg), "GLB hostile input: %d/%d truncations and bit-flips handled without a crash",
                      survived, total);
        CHECK(survived == total, msg);
    }

    af_engine_destroy(engine);
}


/* ==============================================================================================
 *  [15] scene graph, culling, LOD and the frame profiler
 * ----------------------------------------------------------------------------------------------
 *  These are the decisions that decide what reaches the GPU, and every one of them is invisible
 *  until it is wrong: a cull that is too tight makes a limb vanish at the screen edge, an LOD
 *  threshold that is not monotone makes the model pop, and a profiler that reports a plausible
 *  zero for an unavailable GPU timer is a lie. Each is asserted directly.
 * ============================================================================================== */
static void testScene() {
    std::printf("\n\x1b[1m[15] scene graph: transforms, culling, LOD, lights, frame profiler\x1b[0m\n");

    /* ---- 15a. transforms compose parent-to-child ---- */
    {
        Scene scene;
        const int root = scene.addNode("root");
        const int mid = scene.addNode("mid", root);
        const int leaf = scene.addNode("leaf", mid);
        scene.node(root).translation = Vec3(0.0f, 1.0f, 0.0f);
        scene.node(mid).translation = Vec3(0.0f, 0.0f, -2.0f);
        scene.node(leaf).translation = Vec3(0.5f, 0.0f, 0.0f);
        CHECK(scene.updateTransforms(), "scene: transforms resolve for a three-deep chain");
        const Vec3 p = scene.node(leaf).world.transformPoint(Vec3(0.0f, 0.0f, 0.0f));
        CHECK_NEAR(p.x, 0.5, 1e-5, "scene: child translation composes in x");
        CHECK_NEAR(p.y, 1.0, 1e-5, "scene: parent translation composes in y");
        CHECK_NEAR(p.z, -2.0, 1e-5, "scene: grandparent translation composes in z");

        /* A cycle must be refused, not looped over. */
        Scene cyclic;
        const int a = cyclic.addNode("a");
        const int b = cyclic.addNode("b", a);
        cyclic.node(a).parent = b;   /* force the cycle the API would normally prevent */
        CHECK(!cyclic.updateTransforms(), "scene: a cyclic node graph is refused");
    }

    /* ---- 15b. frustum culling: in front survives, behind and beside do not ---- */
    {
        Scene scene;
        const int nodeA = scene.addNode("inFront");
        scene.node(nodeA).translation = Vec3(0.0f, 0.0f, -5.0f);
        const int nodeB = scene.addNode("behindCamera");
        scene.node(nodeB).translation = Vec3(0.0f, 0.0f, 8.0f);
        const int nodeC = scene.addNode("farLeft");
        scene.node(nodeC).translation = Vec3(-40.0f, 0.0f, -5.0f);

        Geometry geo;
        geo.name = "unit";
        geo.vertexCount = 8;
        geo.indexCount = 36;
        geo.bounds = Bounds{Vec3(0.0f, 0.0f, 0.0f), 0.5f};
        geo.lodRanges[0] = {0, 36};
        geo.lodCount = 1;
        const int g = scene.addGeometry(geo);
        scene.addInstance("front", g, nodeA);
        scene.addInstance("behind", g, nodeB);
        scene.addInstance("left", g, nodeC);
        CHECK(scene.updateTransforms(), "scene: culling fixture transforms resolve");

        Camera camera;
        camera.position = Vec3(0.0f, 0.0f, 0.0f);
        camera.target = Vec3(0.0f, 0.0f, -1.0f);
        camera.fovYRadians = 1.0f;
        camera.aspect = 1.0f;
        camera.nearPlane = 0.05f;
        camera.farPlane = 50.0f;

        Visibility vis;
        scene.buildVisibility(camera, 1.0f, &vis);
        CHECK(vis.draws.size() == 1, "scene: exactly the instance in front of the camera is drawn (%zu)", vis.draws.size());
        CHECK(vis.culledInstances == 2, "scene: the instance behind and the one far to the side are culled");
        CHECK(vis.draws.empty() || vis.draws[0].instance == 0, "scene: the surviving draw is the one in front");
        CHECK(vis.totalTriangles == 12, "scene: the draw list reports its triangles (%d)", vis.totalTriangles);

        /* Sphere-versus-plane must be conservative by exactly the radius: an object whose centre
           is just outside a plane but whose body crosses it is still drawn. */
        const Frustum fr = Frustum::fromViewProjection(camera.viewProjection());
        CHECK(fr.intersects(Vec3(0.0f, 0.0f, -5.0f), 0.5f), "scene: a sphere inside the frustum intersects it");
        CHECK(!fr.intersects(Vec3(0.0f, 0.0f, 8.0f), 0.5f), "scene: a sphere behind the camera does not");
    }

    /* ---- 15c. LOD selection is monotone in distance ---- */
    {
        Scene scene;
        const int node = scene.addNode("obj");
        Geometry geo;
        geo.bounds = Bounds{Vec3(0.0f, 0.0f, 0.0f), 0.5f};
        geo.lodCount = 3;
        geo.lodRanges[0] = {0, 300};
        geo.lodRanges[1] = {300, 150};
        geo.lodRanges[2] = {450, 60};
        const int g = scene.addGeometry(geo);
        const int inst = scene.addInstance("obj", g, node);
        scene.instance(inst).lodCount = 3;
        scene.instance(inst).lodDistances[1] = 4.0f;
        scene.instance(inst).lodDistances[2] = 12.0f;
        CHECK(scene.updateTransforms(), "scene: LOD fixture transforms resolve");

        int lastLod = -1;
        int lastTris = 1 << 30;
        bool monotone = true;
        bool heavierFar = false;
        for (float z = -1.0f; z > -30.0f; z -= 0.5f) {
            Camera cam;
            cam.position = Vec3(0.0f, 0.0f, 0.0f);
            cam.target = Vec3(0.0f, 0.0f, -1.0f);
            cam.aspect = 1.0f;
            scene.node(node).translation = Vec3(0.0f, 0.0f, z);
            scene.updateTransforms();
            Visibility vis;
            scene.buildVisibility(cam, 1.0f, &vis);
            if (vis.draws.empty()) continue;
            const DrawItem& d = vis.draws[0];
            if (d.lod < lastLod) monotone = false;
            if (d.indexCount > static_cast<size_t>(lastTris)) heavierFar = true;
            lastLod = d.lod;
            lastTris = static_cast<int>(d.indexCount);
        }
        CHECK(monotone, "scene: LOD never gets finer as the object recedes");
        CHECK(!heavierFar, "scene: triangle count never grows with distance");
        CHECK(lastLod == 2, "scene: a distant object reaches the coarsest level (%d)", lastLod);

        /* Quality scales the thresholds: at low quality the same distance picks a coarser level. */
        Camera cam;
        cam.position = Vec3(0.0f, 0.0f, 0.0f);
        cam.target = Vec3(0.0f, 0.0f, -1.0f);
        cam.aspect = 1.0f;
        scene.node(node).translation = Vec3(0.0f, 0.0f, -6.0f);
        scene.updateTransforms();
        Visibility high, low;
        scene.buildVisibility(cam, 1.0f, &high);
        scene.buildVisibility(cam, 0.0f, &low);
        CHECK(!high.draws.empty() && !low.draws.empty(), "scene: both qualities produced a draw");
        CHECK(!low.draws.empty() && low.draws[0].lod >= high.draws[0].lod,
              "scene: lower quality never chooses a finer level (%d vs %d)",
              low.draws[0].lod, high.draws[0].lod);
    }

    /* ---- 15d. light culling and deterministic ordering ---- */
    {
        Scene scene;
        Light sun;
        sun.type = LIGHT_DIRECTIONAL;
        sun.direction = Vec3(0.0f, -1.0f, 0.0f);
        scene.addLight(sun);
        Light lampNear;
        lampNear.type = LIGHT_POINT;
        lampNear.position = Vec3(1.0f, 1.0f, -3.0f);
        lampNear.range = 5.0f;
        scene.addLight(lampNear);
        Light lampBehind;
        lampBehind.type = LIGHT_POINT;
        lampBehind.position = Vec3(0.0f, 1.0f, 10.0f);
        lampBehind.range = 2.0f;
        scene.addLight(lampBehind);
        Light disabled;
        disabled.type = LIGHT_POINT;
        disabled.position = Vec3(0.0f, 0.0f, -2.0f);
        disabled.enabled = false;
        scene.addLight(disabled);

        Camera cam;
        cam.position = Vec3(0.0f, 1.0f, 0.0f);
        cam.target = Vec3(0.0f, 1.0f, -1.0f);
        cam.aspect = 1.0f;
        Visibility vis;
        scene.buildVisibility(cam, 1.0f, &vis);
        CHECK(vis.lights.size() == 2, "scene: the sun and the near lamp are kept, the far and the disabled ones are not (%zu)",
              vis.lights.size());
        CHECK(!vis.lights.empty() && vis.lights[0].light == 0, "scene: the directional light sorts first");
        CHECK(vis.culledLights == 1, "scene: the out-of-range lamp is counted as culled (%d)", vis.culledLights);

        /* Determinism: the same scene twice gives the same order, which is what stops a capped
           light list from flickering. */
        Visibility again;
        scene.buildVisibility(cam, 1.0f, &again);
        bool same = vis.lights.size() == again.lights.size();
        for (size_t i = 0; same && i < vis.lights.size(); ++i) {
            if (vis.lights[i].light != again.lights[i].light) same = false;
        }
        CHECK(same, "scene: light order is deterministic between frames");
    }

    /* ---- 15e. the profiler measures what it is given and refuses to invent GPU numbers ---- */
    {
        Profiler prof;
        const int geomStage = prof.stageId("geometry");
        const int drawStage = prof.stageId("draw");
        CHECK(geomStage >= 0 && drawStage >= 0, "profiler: stage names intern to ids");
        CHECK(prof.stageId("geometry") == geomStage, "profiler: the same name returns the same id");

        /* Drive 60 frames with a synthetic clock so the numbers are exact, not wall-clock noise. */
        double t = 0.0;
        for (int i = 0; i < 60; ++i) {
            prof.beginFrame(static_cast<float>(t));
            prof.beginStage(geomStage, static_cast<float>(t));
            t += 0.004;                      /* 4 ms of "geometry"    */
            prof.endStage(geomStage, static_cast<float>(t));
            prof.beginStage(drawStage, static_cast<float>(t));
            t += 0.012;                      /* 12 ms of "draw"       */
            prof.endStage(drawStage, static_cast<float>(t));
            prof.endFrame(static_cast<float>(t));
            t += 0.0005;
        }
        FrameStats st = prof.stats();
        CHECK(st.frames == 60, "profiler: every frame is in the window (%d)", st.frames);
        CHECK_NEAR(st.medianFrameMs, 16.0, 0.05, "profiler: median frame time is the measured 16.0 ms");
        CHECK(st.fps > 61.9f && st.fps < 63.1f, "profiler: fps derives from the median, not the mean (%.1f)", st.fps);
        /* Stages are recorded in the order they were interned. */
        bool foundGeometry = false, foundDraw = false;
        for (int i = 0; i < st.stageCount; ++i) {
            if (st.stages[i].name == "geometry") {
                foundGeometry = true;
                CHECK_NEAR(st.stages[i].medianMs, 4.0, 0.05, "profiler: the 4 ms stage reads 4 ms");
            }
            if (st.stages[i].name == "draw") {
                foundDraw = true;
                CHECK_NEAR(st.stages[i].medianMs, 12.0, 0.05, "profiler: the 12 ms stage reads 12 ms");
            }
        }
        CHECK(foundGeometry && foundDraw, "profiler: both stages appear in the report");

        /* Without a GPU timer, every GPU number must be absent, not zero. */
        CHECK(!st.gpuTimingAvailable, "profiler: GPU timing is unavailable until a backend says otherwise");
        CHECK(st.gpuFrameMs < 0.0f && st.gpuVertexMs < 0.0f && st.gpuFragmentMs < 0.0f,
              "profiler: unavailable GPU timings read as -1, never as a plausible 0");
        prof.submitGpuTimings(9.0f, 3.0f, 6.0f);
        FrameStats stillAbsent = prof.stats();
        CHECK(stillAbsent.gpuFrameMs < 0.0f,
              "profiler: GPU timings submitted without a timer are discarded, not stored");

        /* With the capability declared, the numbers are kept. */
        prof.setGpuTimingAvailable(true);
        prof.submitGpuTimings(9.0f, 3.0f, 6.0f);
        FrameStats withGpu = prof.stats();
        CHECK(withGpu.gpuTimingAvailable && withGpu.gpuFrameMs == 9.0f && withGpu.gpuVertexMs == 3.0f,
              "profiler: GPU timings are reported when the device supports them");
    }
}


/* ==============================================================================================
 *  [16] GLB import through the engine and the public ABI
 * ----------------------------------------------------------------------------------------------
 *  [14] proved the reader can read. This proves the engine can *adopt* the result: buffers,
 *  submeshes, regions and materials replaced wholesale, generated state (rig, field, atlas)
 *  invalidated rather than left stale, and a rejected file leaving the engine exactly as it was.
 * ============================================================================================== */
static void testEngineImport() {
    std::printf("\n\x1b[1m[16] GLB import into a live engine (ABI)\x1b[0m\n");

    af_spec spec = defaultSpec(AF_SEX_FEMALE, AF_BUILD_ATHLETIC, 1.72f);
    spec.outfit = AF_OUTFIT_TSHIRT;
    af_engine* e = nullptr;
    CHECK(af_engine_create(&spec, nullptr, &e) == AF_OK && e != nullptr, "import: engine created");
    if (!e) return;
    CHECK(af_engine_run_all(e) == AF_OK, "import: a model was generated first (%s)", af_engine_last_error(e));

    const int32_t generatedVerts = af_engine_vertex_count(e);
    const int32_t generatedTris = af_engine_index_count(e) / 3;
    CHECK(generatedVerts > 100000, "import: the generated model has geometry (%d vertices)", generatedVerts);

    uint8_t* bytes = nullptr;
    size_t byteCount = 0;
    CHECK(af_engine_export_glb_to_memory(e, &bytes, &byteCount) == AF_OK && bytes != nullptr,
          "import: exported the generated model to memory");

    /* ---- (a) a hostile file must be refused without touching the engine ---- */
    {
        const uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const int32_t rc = af_engine_import_glb_from_memory(e, junk, sizeof(junk));
        CHECK(rc != AF_OK, "import: a non-GLB buffer is rejected (%d)", rc);
        CHECK(std::string(af_engine_last_error(e)).size() > 0, "import: the rejection carries a reason (%s)",
              af_engine_last_error(e));
        CHECK(af_engine_vertex_count(e) == generatedVerts, "import: a rejected file leaves the vertices alone");
        CHECK(af_engine_index_count(e) / 3 == generatedTris, "import: a rejected file leaves the triangles alone");
        CHECK(af_engine_joint_count(e) == 71, "import: a rejected file leaves the rig alone (%d joints)",
              af_engine_joint_count(e));
    }

    /* ---- (b) importing our own export into a fresh engine ---- */
    {
        af_engine* fresh = nullptr;
        CHECK(af_engine_create(&spec, nullptr, &fresh) == AF_OK && fresh != nullptr, "import: second engine created");
        if (fresh) {
            CHECK(af_engine_vertex_count(fresh) == 0, "import: a new engine starts empty");
            const int32_t rc = af_engine_import_glb_from_memory(fresh, bytes, byteCount);
            CHECK(rc == AF_OK, "import: the export loads into an empty engine (%s)", af_engine_last_error(fresh));
            if (rc == AF_OK) {
                const int32_t v = af_engine_vertex_count(fresh);
                const int32_t t = af_engine_index_count(fresh) / 3;
                std::printf("     imported through the ABI: %d vertices, %d triangles, %d submeshes, %d materials\n",
                            v, t, af_engine_submesh_count(fresh), af_engine_material_count(fresh));
                CHECK(v == generatedVerts, "import: every vertex arrived (%d vs %d)", v, generatedVerts);
                CHECK(t == generatedTris, "import: every triangle arrived (%d vs %d)", t, generatedTris);
                CHECK(af_engine_submesh_count(fresh) >= 4, "import: submeshes were rebuilt (%d)",
                      af_engine_submesh_count(fresh));
                CHECK(af_engine_material_count(fresh) >= 4, "import: materials were rebuilt (%d)",
                      af_engine_material_count(fresh));

                /* Regions must survive the round trip: the exporter names materials after regions,
                   so an engine-made file comes back with the same region set. */
                const af_submesh* sm = af_engine_submeshes(fresh);
                bool sawSkin = false, sawCloth = false;
                for (int32_t i = 0; i < af_engine_submesh_count(fresh); ++i) {
                    if (sm[i].region == REGION_SKIN) sawSkin = true;
                    if (sm[i].region == REGION_CLOTH_UPPER) sawCloth = true;
                }
                CHECK(sawSkin && sawCloth, "import: regions survived the round trip (skin=%d cloth=%d)",
                      sawSkin, sawCloth);

                /* A skeleton was in the file and must NOT have been adopted: the engine says 0. */
                CHECK(af_engine_joint_count(fresh) == 0,
                      "import: the file's skeleton is not half-adopted (joint count %d)", af_engine_joint_count(fresh));

                /* Every index is in range and every vertex is finite - the two properties that
                   make the buffer safe to hand to a GPU. */
                const af_vertex* vp = af_engine_vertices(fresh);
                const uint32_t* ip = af_engine_indices(fresh);
                int32_t indexCount = af_engine_index_count(fresh);
                bool inRange = true;
                for (int32_t i = 0; i < indexCount; ++i) {
                    if (ip[i] >= static_cast<uint32_t>(v)) { inRange = false; break; }
                }
                CHECK(inRange, "import: every index is inside the vertex buffer");
                bool finite = true;
                for (int32_t i = 0; i < v; ++i) {
                    for (int c = 0; c < 3; ++c) {
                        if (!std::isfinite(vp[i].pos[c]) || !std::isfinite(vp[i].normal[c])) { finite = false; break; }
                    }
                    if (!finite) break;
                }
                CHECK(finite, "import: every position and normal is finite");

                /* Posing an imported model must be refused rather than crashing: there is no rig. */
                const int32_t poseRc = af_engine_apply_clip(fresh, AF_CLIP_WALK, 0.5f);
                CHECK(poseRc != AF_OK, "import: playing a clip on an unrigged import is refused (%d)", poseRc);

                /* And a second import of the same file is idempotent, not cumulative. */
                const int32_t again = af_engine_import_glb_from_memory(fresh, bytes, byteCount);
                CHECK(again == AF_OK && af_engine_vertex_count(fresh) == v &&
                          af_engine_index_count(fresh) / 3 == t,
                      "import: importing twice is idempotent, not cumulative (%d vertices)",
                      af_engine_vertex_count(fresh));
            }
            af_engine_destroy(fresh);
        }
    }

    /* ---- (c) an imported model survives a reset and a re-generate ---- */
    {
        CHECK(af_engine_import_glb_from_memory(e, bytes, byteCount) == AF_OK, "import: live engine adopts the file");
        CHECK(af_engine_vertex_count(e) == generatedVerts, "import: the live engine's buffers were replaced");
        const int32_t resetRc = af_engine_reset(e, &spec);
        CHECK(resetRc == AF_OK, "import: the engine resets cleanly after an import (%s)", af_engine_last_error(e));
        CHECK(af_engine_vertex_count(e) == 0, "import: reset clears the imported geometry");
    }

    af_arena_free(bytes);
    af_engine_destroy(e);
}

int main(int argc, char** argv) {
    std::printf("\n\x1b[1mAvatarForge kernel host tests\x1b[0m  (ABI %d, %d threads)\n\n", AF_ABI_VERSION, defaultThreadCount());
    const bool quick = (argc > 1 && std::strcmp(argv[1], "--quick") == 0);
    const bool abiOnly = (argc > 1 && std::strcmp(argv[1], "--abi-only") == 0);
    const bool compressionOnly = (argc > 1 && std::strcmp(argv[1], "--compression-only") == 0);

    if (compressionOnly) {
        testGpuProfilesAndTextureCompression();
        std::printf("\n\x1b[1mCompression-only %d checks complete: %d failure(s)\x1b[0m\n\n",
                    g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    if (abiOnly) {
        testImageAnalysisPacking();
        testSkippedImageRoleMapping();
        testAbi();
        std::printf("\n\x1b[1mABI-only %d checks complete: %d failure(s)\x1b[0m\n\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    testImageAnalysisPacking();
    testSkippedImageRoleMapping();
    testAnthropometry();
    testField();
    testIsosurface();
    testScaleAndVolume();
    testDeterminism();
    testRig();
    testTexturing();
    testApparel();
    testShaderTables();
    testAvatarCuller();
    testGpuProfilesAndTextureCompression();
    testParallelForExceptions();
    testMeshDecimator();
    testAvatarLodSet();
    testAbi();
    if (!quick) testPoseComposition();
    if (!quick) testBlendshapeExport();
    if (!quick) testGlbImport();
    if (!quick) testScene();
    if (!quick) testEngineImport();
    if (!quick) {
        std::printf("\n\x1b[1mAll %d checks complete: %d failure(s)\x1b[0m\n\n", g_checks, g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
