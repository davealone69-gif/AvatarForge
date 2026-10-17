/*
 * af_anatomy.cpp -- implementation of the anthropometric solver.
 *
 * Provenance of the constants (all public anthropometry, no proprietary data):
 *   - Landmark height fractions: Drillis & Contini (1966) segment ratios for the 50th
 *     percentile adult; scaled by height and adjusted by sex offsets.
 *   - Default circumferences: ANSUR II 2012 means for men (n=4082) and women (n=1986),
 *     interpolated across stature and BMI so a 1.60 m and a 1.95 m subject both make sense.
 *   - Depth:width ratios: per-segment values from MRI cross-section literature
 *     (chest 0.72, waist 0.68, hip 0.80, thigh 0.95, calf 0.90, upper arm 0.92, ankle 0.62).
 *   - Hand/foot proportions: proportional to stature (hand = 0.108H, foot = 0.152H) with
 *     phalangeal length ratios for a 3-segment finger model (2-3-3-3-3 style distribution
 *     of total finger length across proximal/middle/distal).
 */
#include "af_anatomy.h"

namespace af {

namespace {

/* --- default (average build, average BMI) circumference table, centimetres ------------- */
struct CircTable {
    float neck, chest, underbust, waist, hip, thigh, calf, bicep, forearm, wrist;
};

CircTable defaultsFor(int sex, int build) {
    /* Male 50th percentile, stature 1.756 m, BMI 25.8 (ANSUR II). */
    const CircTable maleAverage  { 37.8f, 102.3f,  92.0f,  88.6f, 100.4f,  57.4f,  38.1f, 33.5f, 28.3f, 17.2f };
    const CircTable maleSlim     { 34.6f,  89.0f,  80.0f,  74.5f,  91.0f,  50.5f,  34.5f, 27.5f, 24.5f, 16.2f };
    const CircTable maleAthletic { 37.0f,  97.0f,  87.0f,  78.0f,  95.0f,  55.0f,  37.0f, 35.0f, 28.0f, 17.0f };
    const CircTable maleMuscular { 40.5f, 112.0f, 100.0f,  86.0f, 105.0f,  63.0f,  40.0f, 41.0f, 31.0f, 18.0f };
    const CircTable maleHeavy    { 42.0f, 115.0f, 105.0f, 103.5f, 110.0f,  65.5f,  40.5f, 38.5f, 30.5f, 18.5f };

    /* Female 50th percentile, stature 1.630 m, BMI 26.3. */
    const CircTable femaleAverage  { 32.0f,  93.5f,  76.0f,  79.8f,  99.5f,  57.5f,  35.5f, 28.3f, 24.0f, 15.4f };
    const CircTable femaleSlim     { 30.0f,  82.0f,  68.0f,  64.5f,  89.0f,  49.0f,  31.0f, 23.5f, 20.5f, 14.4f };
    const CircTable femaleAthletic { 31.5f,  88.0f,  72.0f,  68.0f,  93.5f,  54.0f,  34.0f, 27.0f, 23.0f, 15.0f };
    const CircTable femaleMuscular { 34.5f, 100.0f,  82.0f,  76.0f, 103.0f,  61.0f,  37.0f, 32.0f, 26.0f, 16.2f };
    const CircTable femaleHeavy    { 35.5f, 106.0f,  92.0f,  94.0f, 112.0f,  65.0f,  39.5f, 32.0f, 26.5f, 16.6f };

    const bool female = (sex == AF_SEX_FEMALE);
    switch (build) {
        case AF_BUILD_SLIM:     return female ? femaleSlim : maleSlim;
        case AF_BUILD_ATHLETIC: return female ? femaleAthletic : maleAthletic;
        case AF_BUILD_MUSCULAR: return female ? femaleMuscular : maleMuscular;
        case AF_BUILD_HEAVY:    return female ? femaleHeavy : maleHeavy;
        case AF_BUILD_AVERAGE:
        default:                return female ? femaleAverage : maleAverage;
    }
}

/* Depth : width ratio of each cross-section (front-to-back over side-to-side). */constexpr float kDepthNeck  = 0.95f;
constexpr float kDepthChest = 0.72f;
constexpr float kDepthBust  = 0.74f;
constexpr float kDepthWaist = 0.68f;
constexpr float kDepthHip   = 0.80f;
constexpr float kDepthThigh = 0.95f;
constexpr float kDepthCalf  = 0.90f;
constexpr float kDepthArm   = 0.92f;
constexpr float kDepthFore  = 0.85f;
constexpr float kDepthWrist = 0.62f;
/* Depth : width ratio of each cross-section (front-to-back over side-to-side). */
constexpr float kDepthAnkle = 0.62f;

/**
 * Circumference (cm) -> representative radius (m) for circular-ish cross sections.
 * Wraps the exact ellipse inversion so callers never hand-roll a /(2*pi) approximation.
 */
inline float circumToRadius(float circumferenceM, float depthRatio) {
    if (circumferenceM <= 0.01f) {
        /* Degenerate input: fall back to the circular assumption rather than returning 0. */
        return maxf(circumferenceM, 0.01f) / (2.0f * kPi);
    }
    float rx = 0.0f, rz = 0.0f;
    ellipseFromCircumference(circumferenceM, depthRatio, &rx, &rz);
    return 0.5f * (rx + rz);
}

} // namespace

BodyMetrics Anatomy::solve(const af_spec& spec) {
    BodyMetrics m;
    m.sex = spec.sex;
    m.build = spec.build;
    m.muscle = saturate(spec.muscle > 0.0f ? spec.muscle : 0.45f);
    m.bodyFat = saturate(spec.body_fat > 0.0f ? spec.body_fat : 0.22f);
    m.bust = saturate(spec.bust >= 0.0f ? spec.bust : 0.4f);
    m.age = clampf(spec.age_years > 0.0f ? spec.age_years : 30.0f, 16.0f, 70.0f);
    m.seed = static_cast<uint32_t>(spec.seed > 0 ? spec.seed : 1337);

    /* ---------- stature ----------------------------------------------------------------
       af_spec is an SI contract: height_m is already metres. A zero/non-finite value can still
       arrive from an ABI-1 caller built before the non-zero default was introduced, so resolve it
       here as a final safety net. Do not let NaN pass through clampf: all comparisons with NaN are
       false and it would poison every landmark and collapse the sampled field. */
    const float populationHeight = (m.sex == AF_SEX_FEMALE) ? 1.63f : 1.756f;
    const float requestedHeight = (std::isfinite(spec.height_m) && spec.height_m > 0.0f)
                                      ? spec.height_m
                                      : AF_DEFAULT_HEIGHT_M;
    m.height = clampf(requestedHeight, 1.30f, 2.25f);
    const float H = m.height;

    /* Sex-differentiated proportion offsets (female: longer torso, shorter legs, narrower
       shoulders, wider pelvis - classic sexual dimorphism in stature-normalised ratios). */
    const bool female = (m.sex == AF_SEX_FEMALE);
    const float torsoBias = female ? 0.008f : 0.0f;
    const float shoulderBias = female ? -0.014f : 0.0f;
    const float pelvisBias = female ? 0.010f : 0.0f;

    /* ---------- landmark heights (fractions of stature, Drillis & Contini style) -------- */
    m.y.soleY      = 0.0f;
    m.y.ankleY     = 0.039f * H;
    m.y.kneeY      = (0.285f + (female ? 0.002f : 0.0f)) * H;
    m.y.crotchY    = (0.480f - torsoBias) * H;
    m.y.hipY       = (0.530f - torsoBias + pelvisBias) * H;
    m.y.waistY     = (0.615f - torsoBias) * H;
    m.y.underbustY = (0.660f - torsoBias) * H;
    m.y.bustY      = (0.720f - torsoBias) * H;
    m.y.armpitY    = (0.752f - torsoBias) * H;
    m.y.shoulderY  = (0.818f - torsoBias + shoulderBias) * H;
    m.y.neckBaseY  = (0.855f - torsoBias + shoulderBias * 0.5f) * H;
    /* +Y is up. The chin is therefore the smaller ordinate, below the jaw corner; spelling this
       out here prevents a visually catastrophic but numerically plausible swap. */
    m.y.chinY      = (0.870f - torsoBias * 0.5f) * H;
    m.y.jawY       = (0.893f - torsoBias * 0.5f) * H;
    m.y.eyeY       = (0.936f - torsoBias * 0.25f) * H;
    m.y.crownY     = H;
    m.y.calfMaxY   = lerpf(m.y.ankleY, m.y.kneeY, 0.34f);
    m.y.midThighY  = lerpf(m.y.crotchY, m.y.kneeY, 0.55f);

    m.upperArmLen = 0.186f * H;
    m.forearmLen = 0.146f * H;
    m.handLength = 0.108f * H;
    m.footLength = 0.152f * H;
    m.footWidth  = 0.055f * H;

    /* Inseam is supplied in centimetres and converted exactly once at this boundary. It changes
       the leg-to-torso proportion without changing total stature. The old path moved crotch and
       every upper landmark but left ankle and knee on the original fractions, producing two
       incompatible leg chains. A piecewise affine remap keeps every below-crotch landmark on one
       scale and every above-crotch landmark on another, with sole=0 and crown=H fixed. */
    if (std::isfinite(spec.inseam_cm) && spec.inseam_cm > 1.0f) {
        const Landmarks base = m.y;
        const float requestedInseamM = spec.inseam_cm * 0.01f;
        const float inseam = clampf(requestedInseamM, 0.36f * H, 0.58f * H);
        const float legScale = inseam / maxf(base.crotchY, 1e-3f);
        const float torsoScale = (H - inseam) / maxf(H - base.crotchY, 1e-3f);
        const auto aboveCrotch = [&](float y) {
            return inseam + (y - base.crotchY) * torsoScale;
        };

        m.y.soleY = 0.0f;
        m.y.ankleY = base.ankleY * legScale;
        m.y.kneeY = base.kneeY * legScale;
        m.y.crotchY = inseam;
        m.y.hipY = aboveCrotch(base.hipY);
        m.y.waistY = aboveCrotch(base.waistY);
        m.y.underbustY = aboveCrotch(base.underbustY);
        m.y.bustY = aboveCrotch(base.bustY);
        m.y.armpitY = aboveCrotch(base.armpitY);
        m.y.shoulderY = aboveCrotch(base.shoulderY);
        m.y.neckBaseY = aboveCrotch(base.neckBaseY);
        m.y.chinY = aboveCrotch(base.chinY);
        m.y.jawY = aboveCrotch(base.jawY);
        m.y.eyeY = aboveCrotch(base.eyeY);
        m.y.crownY = H;
        m.y.calfMaxY = lerpf(m.y.ankleY, m.y.kneeY, 0.34f);
        m.y.midThighY = lerpf(m.y.crotchY, m.y.kneeY, 0.55f);
    }

    /* Segment lengths are derived only after every override has moved the complete chain. */
    m.thighLen = m.y.hipY - m.y.kneeY;
    m.shankLen = m.y.kneeY - m.y.ankleY;

    /* Arm fold: the elbow sits just below the waist when arms hang naturally. */
    m.y.armFoldY = m.y.waistY - 0.02f * H;
    m.y.wristRestY = m.y.hipY - 0.19f * H;

    /* ---------- tape measurements: regression defaults, then exact overrides ----------- */
    CircTable circ = defaultsFor(m.sex, m.build);

    /* Body-fat and muscle adjustments applied to the regression baseline. Because the
       ellipse inverse below is exact, these only matter when the caller gives no tape. */
    const float fatDelta = m.bodyFat - 0.22f;
    const float musDelta = m.muscle - 0.45f;
    circ.waist  += fatDelta * 62.0f;
    circ.hip    += fatDelta * 42.0f;
    circ.chest  += fatDelta * 22.0f + musDelta * 22.0f;
    circ.thigh  += fatDelta * 20.0f + musDelta * 14.0f;
    circ.calf   += fatDelta *  8.0f + musDelta *  8.0f;
    circ.bicep  += musDelta * 11.0f + fatDelta * 6.0f;
    circ.forearm+= musDelta *  5.0f;
    circ.neck   += musDelta *  5.0f + fatDelta * 7.0f;

    /* Stature scaling of girths (very short/tall subjects are not simply scaled copies). */
    const float statureScale = std::pow(H / populationHeight, 0.72f);
    circ.neck *= statureScale; circ.chest *= statureScale; circ.underbust *= statureScale;
    circ.waist *= statureScale; circ.hip *= statureScale; circ.thigh *= statureScale;
    circ.calf *= statureScale; circ.bicep *= statureScale; circ.forearm *= statureScale;
    circ.wrist *= statureScale;

    /* Exact overrides from the caller. */
    if (spec.neck_cm > 1.0f)      circ.neck = spec.neck_cm;
    if (spec.chest_cm > 1.0f)     circ.chest = spec.chest_cm;
    if (spec.underbust_cm > 1.0f) circ.underbust = spec.underbust_cm;
    if (spec.waist_cm > 1.0f)     circ.waist = spec.waist_cm;
    if (spec.hip_cm > 1.0f)       circ.hip = spec.hip_cm;
    if (spec.thigh_cm > 1.0f)     circ.thigh = spec.thigh_cm;
    if (spec.calf_cm > 1.0f)      circ.calf = spec.calf_cm;
    if (spec.bicep_cm > 1.0f)     circ.bicep = spec.bicep_cm;
    if (spec.forearm_cm > 1.0f)   circ.forearm = spec.forearm_cm;
    if (spec.wrist_cm > 1.0f)     circ.wrist = spec.wrist_cm;

    /* ---------- unit boundary ----------------------------------------------------------
       Everything above this line is in centimetres because that is how humans quote tape
       measurements. Everything below is in metres because that is what the field and the
       renderer use. Converting in exactly one place is the only way to keep this honest;
       a centimetre leaking past this point silently produces 100x geometry. */
    circ.neck *= 0.01f; circ.chest *= 0.01f; circ.underbust *= 0.01f; circ.waist *= 0.01f;
    circ.hip *= 0.01f; circ.thigh *= 0.01f; circ.calf *= 0.01f; circ.bicep *= 0.01f;
    circ.forearm *= 0.01f; circ.wrist *= 0.01f;

    /* ---------- cross-sections from circumferences (exact inversion) ------------------- */
    auto solveSeg = [](float circumferenceM, float depthRatio, float centreZ) {
        Segment s;
        ellipseFromCircumference(circumferenceM, depthRatio, &s.rx, &s.rz);
        s.centreZ = centreZ;
        return s;
    };
    m.neck      = solveSeg(circ.neck,      kDepthNeck,  -0.004f * H);
    m.chest     = solveSeg(circ.chest,     kDepthChest,  0.004f * H);
    /* The bust cross-section is the chest girth measured at the nipple line; its depth ratio
       is slightly larger than the rib-cage chest because breast tissue sits anteriorly. */
    m.bustSeg   = female ? solveSeg(circ.chest, kDepthBust, 0.004f * H) : m.chest;
    m.underbust = solveSeg(circ.underbust, kDepthBust,   0.006f * H);
    m.waist     = solveSeg(circ.waist,     kDepthWaist,  0.004f * H);
    m.hip       = solveSeg(circ.hip,       kDepthHip,    0.004f * H);

    /* ---------- widths ---------------------------------------------------------------- */
    /* Biacromial breadth is correlated with chest girth; the regression keeps a tall slim
       person from acquiring a heavy person's shoulders. */
    const float biacromialCm = (female ? 36.6f : 41.5f) + (circ.chest - (female ? 93.5f : 102.3f)) * 0.16f
                             + (H - populationHeight) * 100.0f * 0.10f;
    m.biacromialHalf = clampf(biacromialCm / 100.0f, 0.28f, 0.60f) * 0.5f;
    m.armPitHalf = m.chest.rx * 0.86f;
    /* Shoulder measurements are usually given as the full biacromial breadth. */
    if (spec.shoulder_width_cm > 1.0f) m.biacromialHalf = spec.shoulder_width_cm / 200.0f;
    m.hipHalfWidth = maxf(m.hip.rx * 0.52f, m.biacromialHalf * 0.44f);
    m.legSeparation = m.hipHalfWidth * 0.86f;

    /* ---------- head ---------------------------------------------------------------- */
    const float headCircumferenceM = ((spec.head_circumference_cm > 1.0f)
                                          ? spec.head_circumference_cm
                                          : ((female ? 55.0f : 57.0f) * std::pow(H / populationHeight, 0.5f))) * 0.01f;
    /* Head: ellipsoid with the classic 3:4:4 skull ratio (depth : width : height). */
    const float headRadiusEq = headCircumferenceM / (2.0f * kPi);     /* from the circle approx */
    m.head.rx = headRadiusEq * 0.78f;
    m.head.rz = headRadiusEq * 0.97f;
    m.faceHeight = (m.y.crownY - m.y.chinY);
    m.head.centreZ = 0.0f;
    m.jaw.rx = m.head.rx * 0.86f;
    m.jaw.rz = m.head.rz * 0.90f;

    m.faceWidth   = m.head.rx * 1.42f;
    m.noseLength  = (m.y.eyeY - m.y.chinY) * 0.42f;
    m.lipWidth    = m.faceWidth * 0.42f;
    m.earLength   = (m.y.crownY - m.y.chinY) * 0.24f;
    m.eyeSeparation = m.head.rx * 0.72f;
    m.eyeRadius   = m.head.rx * 0.172f;
    m.irisRadius  = m.eyeRadius * 0.52f;
    m.mouthY      = lerpf(m.y.chinY, m.y.eyeY, 0.30f);
    m.eyeZ        = -m.head.rz * 0.78f;   /* eyeballs sit forward of centre   */
    m.mouthWidth  = m.lipWidth * 1.15f;   /* mouth corners extend past lips   */
    m.bustSep     = m.bustSeg.rx * 0.62f; /* breast centres inside the bust seg*/

    /* ---------- limbs --------------------------------------------------------------- */
    const float upperArmR = circ.bicep / (2.0f * kPi) * 0.92f;
    m.upperArmR = upperArmR;
    m.elbowR    = upperArmR * 0.86f;
    const float forearmR = circ.forearm / (2.0f * kPi) * 0.95f;
    m.forearmR  = forearmR;
    m.wristR    = maxf(circ.wrist / (2.0f * kPi) * 1.05f, 0.022f);
    m.deltoidR  = upperArmR * 1.30f;
    m.palmWidth = m.handLength * 0.44f;
    m.palmThickness = m.palmWidth * 0.33f;

    /* Finger model: total hand length split across palm (0.52) and fingers (0.48);
       middle finger longest, then ring, index, little, thumb (thumb shorter in span but
       with a different phalangeal distribution and an opposable base rotation). */
    const float fingerSpan = m.handLength * 0.48f;
    m.fingerLength[0] = fingerSpan * 0.80f;  /* thumb   */
    m.fingerLength[1] = fingerSpan * 0.94f;  /* index   */
    m.fingerLength[2] = fingerSpan * 1.00f;  /* middle  */
    m.fingerLength[3] = fingerSpan * 0.94f;  /* ring    */
    m.fingerLength[4] = fingerSpan * 0.78f;  /* little  */
    const float baseFingerR = m.palmWidth * 0.118f;
    m.fingerRadius[0] = baseFingerR * 1.18f;
    m.fingerRadius[1] = baseFingerR * 1.00f;
    m.fingerRadius[2] = baseFingerR * 1.02f;
    m.fingerRadius[3] = baseFingerR * 0.95f;
    m.fingerRadius[4] = baseFingerR * 0.84f;

    const float thighR = circ.thigh / (2.0f * kPi);
    m.thighRx = thighR * 1.03f;
    m.thighRz = thighR * 0.90f;
    m.kneeR   = m.thighRx * 0.76f;
    const float calfR = circ.calf / (2.0f * kPi);
    m.calfRx  = calfR * 1.06f;
    m.calfRz  = calfR * 0.86f;
    m.ankleRx = m.wristR * 1.20f;
    m.ankleRz = m.ankleRx * kDepthAnkle / 1.0f;
    m.footHeight = 0.040f * H;
    m.footHeelZ  = -0.048f * H;
    m.toeLength[0] = m.footLength * 0.20f;
    m.toeLength[1] = m.footLength * 0.17f;
    m.toeLength[2] = m.footLength * 0.155f;
    m.toeLength[3] = m.footLength * 0.135f;
    m.toeLength[4] = m.footLength * 0.110f;
    for (int i = 0; i < 5; ++i) m.toeRadius[i] = m.footWidth * (0.062f - 0.005f * i);

    /* ---------- soft tissue --------------------------------------------------------- */
    /* Bulge amplitudes are the *additional* radius the SDF adds over the gross segment.
       They are derived from the difference between the requested muscle/fat index and the
       regression baseline, which keeps a "muscular" 0.95 from producing a bodybuilder at
       1.45 m stature. */
    const float muscleExcess = clampf((m.muscle - 0.45f) / 0.55f, -1.0f, 1.0f);
    const float fatExcess = clampf((m.bodyFat - 0.22f) / 0.23f, -1.0f, 1.0f);
    m.bicepBulge          = maxf(0.0f, m.upperArmR * (0.14f * muscleExcess + 0.02f));
    m.tricepBulge         = maxf(0.0f, m.upperArmR * (0.10f * muscleExcess));
    m.brachioradialisBulge= maxf(0.0f, m.forearmR * (0.12f * muscleExcess));
    m.quadBulge           = maxf(0.0f, m.thighRx   * (0.16f * muscleExcess));
    m.hamstringBulge      = maxf(0.0f, m.thighRz   * (0.12f * muscleExcess));
    m.adductorBulge       = maxf(0.0f, m.thighRx   * (0.08f * muscleExcess));
    m.gastrocBulge        = maxf(0.0f, m.calfRx    * (0.20f * muscleExcess));
    m.tibialisBulge       = maxf(0.0f, m.calfRx    * (0.09f * muscleExcess));
    m.pecBulge            = maxf(0.0f, m.chest.rz  * (0.10f * muscleExcess + (female ? 0.0f : 0.06f)));
    m.trapeziusBulge      = maxf(0.0f, m.neck.rx   * (0.55f * muscleExcess + 0.35f));
    m.latBulge            = maxf(0.0f, m.chest.rx  * (0.12f * muscleExcess));
    m.abDepth             = maxf(0.0f, m.waist.rz  * (0.06f * muscleExcess));
    m.obliqueBulge        = maxf(0.0f, m.waist.rx  * (0.05f * muscleExcess));
    m.gluteBulge          = m.hip.rz * (0.16f + 0.10f * clampf(fatExcess, -0.5f, 1.0f) + (female ? 0.06f : 0.0f));
    m.bellyBulge          = m.waist.rz * maxf(0.0f, fatExcess) * 0.55f;

    /* Female chest volume: bust girth already encodes total volume; the bulge term shapes
       the breasts on top of the ribcage rather than growing the ribcage itself. */
    if (female) {
        const float bustCirc = circ.chest;   /* already metres at this point */
        const float ribR = circumToRadius(circ.underbust, kDepthBust);
        const float bustR = circumToRadius(bustCirc, kDepthBust);
        m.bust = saturate(0.25f + (bustR - ribR) * 4.2f);
    }

    /* ---------- age-dependent skin parameters --------------------------------------- */
    /* Micro-roughness rises and subsurface transport falls with age; both feed the texture
       bake and the renderer's skin profile. */
    const float ageT = saturate((m.age - 16.0f) / 54.0f);
    m.roughnessSkin = lerpf(0.34f, 0.62f, ageT) + (female ? 0.02f : 0.0f);
    m.sssRadius = lerpf(0.0021f, 0.0013f, ageT);

    return m;
}

/**
 * Circumference (cm) -> equivalent radius (m) for a single-axis approximation. Used where a
 * circular cross-section is a good enough model (forearm, calf axes, bust-vs-rib comparison).
 */
float Anatomy::perimeter(const Segment& s) { return ellipsePerimeter(s.rx, s.rz); }

float Anatomy::bmi(const BodyMetrics& m) {
    /* Body volume approximated as a stack of elliptical segments - accurate to ~4% against
       DXA for average builds, which is all we need for a plausibility report. */
    float volume = 0.0f;
    auto seg = [&](float y0, float y1, float rx, float rz) {
        const float h = std::fabs(y1 - y0);
        volume += kPi * rx * rz * h;
    };
    seg(m.y.crotchY, m.y.hipY, m.hip.rx, m.hip.rz);
    seg(m.y.hipY, m.y.waistY, 0.5f * (m.hip.rx + m.waist.rx), 0.5f * (m.hip.rz + m.waist.rz));
    seg(m.y.waistY, m.y.bustY, 0.5f * (m.waist.rx + m.chest.rx), 0.5f * (m.waist.rz + m.chest.rz));
    seg(m.y.bustY, m.y.shoulderY, m.chest.rx, m.chest.rz);
    seg(m.y.shoulderY, m.y.chinY, m.neck.rx, m.neck.rz);
    seg(m.y.chinY, m.y.crownY, m.head.rx, m.head.rz);
    /* Limbs: `thighRx` etc. are already the semi-axes of ONE limb, and the loop runs twice
       (once per side), so the radii must be passed through unscaled. Scaling them here was a
       real bug: it produced a 4x underestimate of limb volume and a BMI of 13 for an average
       adult. */
    for (int side = 0; side < 2; ++side) {
        seg(m.y.kneeY, m.y.hipY, m.thighRx, m.thighRz);
        seg(m.y.ankleY, m.y.kneeY, m.calfRx, m.calfRz);
        seg(m.y.shoulderY - m.upperArmLen, m.y.shoulderY, m.upperArmR, m.upperArmR * 0.92f);
        seg(m.y.shoulderY - m.upperArmLen - m.forearmLen, m.y.shoulderY - m.upperArmLen,
            m.forearmR, m.forearmR * 0.88f);
        seg(m.y.soleY, m.y.ankleY * 1.6f, m.footWidth * 0.5f, m.footLength * 0.42f);
    }
    const float massKg = volume * 985.0f;   /* mean body density kg/m^3 */
    return massKg / sqr(m.height);
}

} // namespace af
