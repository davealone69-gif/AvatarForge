/*
 * af_anatomy.h -- anthropometric solver.
 *
 * Turns a user-facing af_spec (sex, build, fat, muscle + optional tape measurements) into a
 * complete set of metric landmarks and cross-section radii that the SDF body field consumes.
 *
 * Two stages:
 *   1. Regression: sex/build/height -> all landmark heights and default circumferences,
 *      from published anthropometric ratios (Drillis & Contini landmark fractions, ANSUR II
 *      circumferential means, ISO 7250 definitions for how each tape is taken).
 *   2. Constraint solve: any tape value supplied by the caller overrides the regression and
 *      is honoured *exactly* by inverting the ellipse-perimeter relation, so a "waist 78 cm"
 *      request produces a mesh whose waist perimeter measures 78 cm (asserted in tests to
 *      within 0.2%). Inseam and head circumference similarly move the landmark heights.
 *
 * Everything here is metres. Depths are expressed as depth:width ratios per segment, taken
 * from MRI-derived cross-section studies, which is what keeps torsos from looking like
 * cylinders.
 */
#ifndef AF_ANATOMY_H
#define AF_ANATOMY_H

#include "af_math.h"
#include "../include/afcore.h"
#include <array>

namespace af {

struct Landmarks {
    float soleY = 0.0f;
    float ankleY = 0.0f;
    float calfMaxY = 0.0f;
    float kneeY = 0.0f;
    float midThighY = 0.0f;
    float crotchY = 0.0f;
    float hipY = 0.0f;        /* iliac crest / widest hip                        */
    float waistY = 0.0f;      /* narrowest torso point                           */
    float underbustY = 0.0f;
    float bustY = 0.0f;       /* nipple line                                      */
    float armpitY = 0.0f;
    float shoulderY = 0.0f;   /* acromion                                         */
    float neckBaseY = 0.0f;
    float jawY = 0.0f;
    float chinY = 0.0f;
    float eyeY = 0.0f;
    float crownY = 0.0f;      /* scalp top                                        */
    float armFoldY = 0.0f;    /* where arm hangs, used to place hands at rest     */
    float wristRestY = 0.0f;
};

struct Segment {
    /* Semi-axes of the elliptical cross-section, metres. */
    float rx = 0.0f;
    float rz = 0.0f;
    float centreZ = 0.0f;     /* front/back offset of the cross-section centre */
};

struct BodyMetrics {
    /* --- global form --- */
    int   sex = AF_SEX_UNSPECIFIED;
    int   build = AF_BUILD_AVERAGE;
    float height = 1.75f;
    float muscle = 0.45f;
    float bodyFat = 0.22f;
    float bust = 0.4f;
    float age = 30.0f;
    uint32_t seed = 1337u;

    /* --- skeletal landmarks --- */
    Landmarks y;

    /* --- torso cross-sections --- */
    Segment neck, chest, bustSeg, underbust, waist, hip, head, jaw;

    /* --- widths & limb geometry --- */
    float biacromialHalf = 0.0f;   /* shoulder joint separation / 2              */
    float deltoidR = 0.0f;
    float armPitHalf = 0.0f;
    float handLength = 0.0f;
    float palmWidth = 0.0f;
    float palmThickness = 0.0f;
    float upperArmLen = 0.0f;
    float forearmLen = 0.0f;
    float upperArmR = 0.0f, elbowR = 0.0f, forearmR = 0.0f, wristR = 0.0f;
    float bicepBulge = 0.0f, tricepBulge = 0.0f, brachioradialisBulge = 0.0f;
    float thighLen = 0.0f, shankLen = 0.0f;
    float thighRx = 0.0f, thighRz = 0.0f, kneeR = 0.0f, calfRx = 0.0f, calfRz = 0.0f, ankleRx = 0.0f, ankleRz = 0.0f;
    float quadBulge = 0.0f, hamstringBulge = 0.0f, adductorBulge = 0.0f, gastrocBulge = 0.0f, tibialisBulge = 0.0f;
    float footLength = 0.0f, footWidth = 0.0f, footHeelZ = 0.0f, footHeight = 0.0f;
    float toeLength[5] = {0, 0, 0, 0, 0};
    float toeRadius[5] = {0, 0, 0, 0, 0};
    float fingerLength[5] = {0, 0, 0, 0, 0};   /* thumb, index, middle, ring, little */
    float fingerRadius[5] = {0, 0, 0, 0, 0};

    /* --- soft tissue shaping --- */
    float pecBulge = 0.0f, abDepth = 0.0f, obliqueBulge = 0.0f, gluteBulge = 0.0f, bellyBulge = 0.0f;
    float trapeziusBulge = 0.0f, latBulge = 0.0f;
    float hipHalfWidth = 0.0f;     /* centre of thigh to centre of thigh / 2      */
    float legSeparation = 0.0f;    /* hip joint x offset                          */

    /* --- facial proportions (derived from head metrics) --- */
    float faceWidth = 0.0f, faceHeight = 0.0f, noseLength = 0.0f, lipWidth = 0.0f, earLength = 0.0f;
    float eyeSeparation = 0.0f, eyeRadius = 0.0f, irisRadius = 0.0f, mouthY = 0.0f;
    float eyeZ = 0.0f;             /* -Z of the eyeball centres (forward)      */
    float mouthWidth = 0.0f;       /* corner-to-corner mouth width            */
    float bustSep = 0.0f;          /* half distance between the breast centres */

    /* Quality hints for downstream stages. */
    float roughnessSkin = 0.0f;    /* 0..1 micro-roughness baseline (age dependent)  */
    float sssRadius = 0.0f;        /* subsurface scattering radius in metres         */
};

class Anatomy {
public:
    /** Full solve. Reads every field of `spec`; guarantees internal consistency. */
    static BodyMetrics solve(const af_spec& spec);

    /** Recomputed perimeter (metres) of a torso segment - used by tests and the SDK report. */
    static float perimeter(const Segment& s);

    /** Convenience: estimated body mass index from the solved metrics. */
    static float bmi(const BodyMetrics& m);
};

} // namespace af
#endif /* AF_ANATOMY_H */
