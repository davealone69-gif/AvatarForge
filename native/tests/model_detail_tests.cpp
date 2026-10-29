/*
 * Focused geometry regressions for the defects visible in the 2026-10-29 device captures:
 * blocky world-axis palms, skin breaking through garments, swapped/asymmetric eyes, the solid
 * metal-ball chest plate and hair that read as a helmet. These query the same analytic field and
 * extraction path the renderer uses; they are not a substitute for a device visual pass.
 */
#include "../src/af_anatomy.h"
#include "../src/af_field.h"
#include "../src/af_body.h"
#include "../src/af_mesh.h"
#include "../include/afcore.h"
#include <cstdio>
#include <cmath>

using namespace af;
static int checks = 0, failures = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

static BodyField makeField(float height, int outfit, int hair = AF_HAIR_LONG) {
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE;
    spec.build = AF_BUILD_AVERAGE;
    spec.height_m = height;
    spec.outfit = outfit;
    spec.footwear = AF_FOOTWEAR_SABATON;
    spec.hair_style = hair;
    BodyMetrics metrics = Anatomy::solve(spec);
    BodyField field(metrics);
    ApparelOptions apparel;
    apparel.outfit = outfit;
    apparel.footwear = spec.footwear;
    apparel.hair = hair;
    apparel.latticeCell = BodyBuildOptions{}.bodyCell;
    field.setApparel(apparel);
    return field;
}

static void palmUsesAnatomicFrame() {
    for (float height : {1.40f, 1.75f}) {
        const BodyField field = makeField(height, AF_OUTFIT_ARMOR_PLATE);
        const BodyMetrics& m = field.metrics();
        for (int side : {-1, 1}) {
            const Chain& c = field.chain(side);
            const Vec3 down = (c.wrist - c.elbow).normalizedOr({0, -1, 0});
            const Vec3 normal(-float(side), 0, 0);
            const Vec3 across = normal.cross(down).normalizedOr({0, 0, 1});
            // A relaxed palm is thin along its normal and wide across the knuckles. The old
            // world-axis box made it thick along X and narrow along Z, producing the blocks.
            const Vec3 throughThickness = c.palmCentre + normal * (m.palmWidth * 0.45f);
            const Vec3 acrossWidth = c.palmCentre + across * (m.palmWidth * 0.42f);
            check(field.hand(throughThickness, side) > 0.001f,
                  "palm does not extrude a world-axis box through its thin side");
            check(field.hand(acrossWidth, side) < -0.001f,
                  "palm remains solid across the knuckle width");
        }
    }
}

static void garmentLayerIsResolvable() {
    for (int outfit : {AF_OUTFIT_TSHIRT, AF_OUTFIT_SUIT, AF_OUTFIT_ARMOR_PLATE, AF_OUTFIT_SPACESUIT}) {
        const BodyField field = makeField(1.75f, outfit);
        const float cell = field.apparel().latticeCell;
        check(field.minLayer() >= 1.9f * cell,
              "garment offset is thick enough for the extraction lattice to resolve");
        // At the body surface under a covered torso patch, the garment must still be outside
        // (negative) so the union extracts cloth, not coincident skin.
        const BodyMetrics& m = field.metrics();
        int covered = 0, leaks = 0;
        for (int yi = 0; yi < 9; ++yi) {
            const float y = lerpf(m.y.waistY, m.y.shoulderY - 0.03f, float(yi) / 8.0f);
            for (int ai = 0; ai < 12; ++ai) {
                const float angle = float(ai) * 6.2831853f / 12.0f;
                Vec3 p(0, y, 0);
                // March outward from the torso axis to the skin surface.
                float r = 0.0f;
                for (int step = 0; step < 80; ++step) {
                    p = Vec3(std::cos(angle) * r, y, std::sin(angle) * r);
                    if (field.body(p) >= 0.0f) break;
                    r += 0.002f;
                }
                if (field.body(p) < -0.001f || field.body(p) > 0.004f) continue;
                ++covered;
                if (field.clothUpperPublic(p) > -0.001f) ++leaks;
            }
        }
        check(covered > 20 && leaks == 0,
              "upper garment covers the torso surface without skin poke-through");
    }
}

static void lowerGarmentCoversLegs() {
    for (int outfit : {AF_OUTFIT_SUIT, AF_OUTFIT_ARMOR_PLATE, AF_OUTFIT_SPACESUIT}) {
        const BodyField field = makeField(1.75f, outfit);
        const BodyMetrics& m = field.metrics();
        int covered = 0, leaks = 0;
        for (int side : {-1, 1}) {
            const Chain& c = field.chain(side);
            for (int yi = 0; yi < 7; ++yi) {
                const float y = lerpf(m.y.ankleY + 0.12f, m.y.crotchY - 0.04f, float(yi) / 6.0f);
                for (int ai = 0; ai < 10; ++ai) {
                    const float angle = float(ai) * 6.2831853f / 10.0f;
                    Vec3 p(c.hipJoint.x, y, 0);
                    float r = 0.0f;
                    for (int step = 0; step < 70; ++step) {
                        p = Vec3(c.hipJoint.x + std::cos(angle) * r, y, std::sin(angle) * r);
                        if (field.body(p) >= 0.0f) break;
                        r += 0.002f;
                    }
                    if (field.body(p) < -0.001f || field.body(p) > 0.004f) continue;
                    ++covered;
                    if (field.clothLowerPublic(p) > -0.001f) ++leaks;
                }
            }
        }
        check(covered > 40 && leaks == 0,
              "lower garment covers the leg surface without skin poke-through");
    }
}


static void eyesAreSymmetricAndExposed() {
    // The 2026-10-29 device GLB had 1637 left-eye triangles but only 156 right-eye
    // triangles. The analytic orbits are mirrored, so the exposed sclera must be too.
    for (int hair : {AF_HAIR_NONE, AF_HAIR_PONYTAIL}) {
        const BodyField field = makeField(1.75f, AF_OUTFIT_ARMOR_PLATE, hair);
        const BodyMetrics& m = field.metrics();
        int exposedBySide[2] = {0, 0};
        int samples = 0;
        for (int si = 0; si < 2; ++si) {
            const int side = si == 0 ? -1 : 1;
            const Vec3 e = field.eyeCentre(side);
            const float r = m.eyeRadius;
            for (int yi = -2; yi <= 2; ++yi) {
                for (int xi = -2; xi <= 2; ++xi) {
                    const float x = e.x + xi * r * 0.22f;
                    const float y = e.y + yi * r * 0.22f;
                    const float z = e.z - std::sqrt(std::max(0.0f, r * r - (x - e.x) * (x - e.x) - (y - e.y) * (y - e.y)));
                    const Vec3 p(x, y, z - 0.0005f);
                    ++samples;
                    if (field.classifyRegion(p) == (side > 0 ? REGION_EYE_L : REGION_EYE_R)) ++exposedBySide[si];
                }
            }
            check(exposedBySide[si] >= samples / 6, "eyeball has a usable exposed sclera patch");
        }
        const int lo = std::min(exposedBySide[0], exposedBySide[1]);
        const int hi = std::max(exposedBySide[0], exposedBySide[1]);
        check(lo > 0 && hi <= lo * 2, "exposed sclera is symmetric between the two eyes");
    }
}

static void extractedEyePatchesAreSymmetric() {
    // Field symmetry alone is not enough: the old red-green refinement inherited endpoint A's
    // region at every new midpoint, so the extracted sclera patch depended on lattice phase.
    const BodyField field = makeField(1.75f, AF_OUTFIT_ARMOR_PLATE, AF_HAIR_PONYTAIL);
    BodyBuildOptions opt;
    opt.threads = 2;
    Mesh mesh;
    BodyBuildReport report;
    check(buildBodyMesh(field, opt, &mesh, &report), "detail-test avatar extracts");
    const BodyMetrics& m = field.metrics();
    int tris[2] = {0, 0};
    for (size_t t = 0; t < mesh.triangleCount(); ++t) {
        const int reg = mesh.regions[mesh.indices[t * 3]];
        if (reg != REGION_EYE_L && reg != REGION_EYE_R) continue;
        const Vec3 c = (mesh.positions[mesh.indices[t * 3]] + mesh.positions[mesh.indices[t * 3 + 1]] +
                        mesh.positions[mesh.indices[t * 3 + 2]]) / 3.0f;
        const int side = c.x > 0.0f ? 1 : -1;
        if ((c - field.eyeCentre(side)).length() < m.eyeRadius * 2.5f) ++tris[side > 0 ? 1 : 0];
    }
    const int lo = std::min(tris[0], tris[1]);
    const int hi = std::max(tris[0], tris[1]);
    check(lo >= 500, "both eyes survive extraction with a real sclera patch");
    check(hi <= lo * 2, "extracted eye patches are within 2x of each other");
}

static void chestPlateIsAShellNotABall() {
    const BodyField field = makeField(1.75f, AF_OUTFIT_ARMOR_PLATE);
    const BodyMetrics& m = field.metrics();
    // A cuirass follows the sternum. The old solid ellipsoid protruded ~8 cm in front of
    // the cloth and read as a metal sphere stuck to the chest in the device capture.
    const float y = m.y.bustY + 0.02f * m.height;
    float skinZ = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const float z = -0.40f + i * 0.001f;
        if (field.body(Vec3(0, y, z)) < 0.0f) { skinZ = z; break; }
    }
    float gearFront = 1e9f;
    for (int i = 0; i < 500; ++i) {
        const float z = -0.45f + i * 0.001f;
        if (field.gearPublic(Vec3(0, y, z)) < 0.0f) { gearFront = z; break; }
    }
    check(gearFront < 0.0f, "cuirass exists in front of the sternum");
    check(gearFront > skinZ - 0.045f, "cuirass stays within 45 mm of the torso, not a sphere");
}

static void hairDoesNotCoverTheEyes() {
    for (int hair : {AF_HAIR_SHORT, AF_HAIR_PONYTAIL, AF_HAIR_LONG}) {
        const BodyField field = makeField(1.75f, AF_OUTFIT_ARMOR_PLATE, hair);
        const BodyMetrics& m = field.metrics();
        int covered = 0;
        for (int side : {-1, 1}) {
            const Vec3 e = field.eyeCentre(side);
            for (int i = 0; i < 9; ++i) {
                const Vec3 p(e.x + (i - 4) * m.eyeRadius * 0.25f, e.y, e.z - m.eyeRadius * 1.4f);
                if (field.hairPublic(p) < 0.0f) ++covered;
            }
        }
        check(covered == 0, "hair mass stays clear of the eye line");
    }
}

int main() {
    palmUsesAnatomicFrame();
    eyesAreSymmetricAndExposed();
    extractedEyePatchesAreSymmetric();
    chestPlateIsAShellNotABall();
    hairDoesNotCoverTheEyes();
    garmentLayerIsResolvable();
    lowerGarmentCoversLegs();
    std::printf("Model detail regressions: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
