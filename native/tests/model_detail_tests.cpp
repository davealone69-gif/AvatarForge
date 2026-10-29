/*
 * Focused geometry regressions for the defects visible in the 2026-10-29 device captures:
 * blocky world-axis palms and skin breaking through garments. These query the same analytic
 * field the mesh extractor uses; they are not a substitute for a device visual pass.
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

static BodyField makeField(float height, int outfit) {
    af_spec spec;
    af_spec_init(&spec);
    spec.sex = AF_SEX_FEMALE;
    spec.build = AF_BUILD_AVERAGE;
    spec.height_m = height;
    spec.outfit = outfit;
    spec.footwear = AF_FOOTWEAR_SABATON;
    spec.hair_style = AF_HAIR_LONG;
    BodyMetrics metrics = Anatomy::solve(spec);
    BodyField field(metrics);
    ApparelOptions apparel;
    apparel.outfit = outfit;
    apparel.footwear = spec.footwear;
    apparel.hair = spec.hair_style;
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

int main() {
    palmUsesAnatomicFrame();
    garmentLayerIsResolvable();
    lowerGarmentCoversLegs();
    std::printf("Model detail regressions: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
