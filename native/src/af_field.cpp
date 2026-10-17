/*
 * af_field.cpp -- implementation of the human body signed distance field.
 *
 * Construction order and why it matters:
 *   1. Skeleton chains (articulation points) are derived from the anthropometric landmarks
 *      first, because every primitive - capsule, muscle blob, nail - is anchored to them.
 *      This is also what guarantees the field and the rig agree by construction: the rig is
 *      later built from these exact same joint positions, so bind-pose skinning is exact.
 *   2. The trunk is a swept super-elliptical profile rather than a stack of capsules: that is
 *      the only way to get a believable waist-hip-bust rhythm with continuous curvature.
 *   3. Muscle/adipose blobs are unioned with a smooth-min radius that scales with the blob
 *      size, so a heavy build gets soft merges (fat is smooth) and a muscular build gets
 *      tighter merges (muscle boundaries are defined).
 *   4. Facial features are subtractive as well as additive (eye sockets, mouth line, nostril
 *      notches) because a photoreal face reads from its concavities far more than its bumps.
 *
 * Every primitive here is evaluated in metres; the field is approximately 1-Lipschitz, which
 * the extractor relies on for safe sphere-tracing and for skipping empty grid blocks.
 */
#include "af_field.h"
#include "af_mesh.h"

namespace af {

/* =======================================================================================
 * Profile1D -- monotone cubic Hermite (Fritsch-Carlson tangents)
 * ======================================================================================= */
void Profile1D::build() {
    const size_t n = ys_.size();
    ms_.assign(n, 0.0f);
    if (n < 2) return;
    std::vector<float> delta(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        const float dy = ys_[i + 1] - ys_[i];
        delta[i] = (std::fabs(dy) < kEps) ? 0.0f : (vs_[i + 1] - vs_[i]) / dy;
    }
    ms_[0] = delta[0];
    ms_[n - 1] = delta[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) {
        if (delta[i - 1] * delta[i] <= 0.0f) { ms_[i] = 0.0f; continue; }
        const float w1 = 2.0f * (ys_[i + 1] - ys_[i]) + (ys_[i] - ys_[i - 1]);
        const float w2 = (ys_[i + 1] - ys_[i]) + 2.0f * (ys_[i] - ys_[i - 1]);
        ms_[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
    }
}

float Profile1D::eval(float y) const {
    const size_t n = ys_.size();
    if (n == 0) return 0.0f;
    if (n == 1) return vs_[0];
    if (y <= ys_[0]) return vs_[0];
    if (y >= ys_[n - 1]) return vs_[n - 1];
    size_t hi = 1;
    while (hi < n - 1 && ys_[hi] < y) ++hi;
    const size_t lo = hi - 1;
    const float h = ys_[hi] - ys_[lo];
    if (std::fabs(h) < kEps) return vs_[lo];
    const float t = (y - ys_[lo]) / h;
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * vs_[lo] + (t3 - 2 * t2 + t) * h * ms_[lo] +
           (-2 * t3 + 3 * t2) * vs_[hi] + (t3 - t2) * h * ms_[hi];
}

/* =======================================================================================
 * helpers
 * ======================================================================================= */
namespace {

/** Rotate `dir` by `angle` radians about the axis that turns it toward `target`. */
inline Vec3 rotateToward(const Vec3& dir, const Vec3& target, float angle) {
    Vec3 axis = dir.cross(target);
    if (axis.lengthSq() < 1e-10f) return dir;
    axis = axis.normalizedOr({1, 0, 0});
    const Quat q = Quat::fromAxisAngle(axis, angle);
    return q.rotate(dir).normalizedOr(dir);
}

/** Ellipsoid blob, the universal "add flesh here" primitive. */
inline float blob(const Vec3& p, const Vec3& centre, const Vec3& radii) {
    return sdEllipsoid(p - centre, radii);
}

/** Two-lobe blob mirrored across X, for paired anatomy (pecs, glutes, deltoids, ...). */
inline float blob2(const Vec3& p, float cx, float cy, float cz, const Vec3& radii) {
    const float a = blob(p, {cx, cy, cz}, radii);
    const float b = blob(p, {-cx, cy, cz}, radii);
    return minf(a, b);
}

} // namespace

/* =======================================================================================
 * Construction
 * ======================================================================================= */
BodyField::BodyField(const BodyMetrics& metrics) : m_(metrics), noise_(metrics.seed) {
    const float H = m_.height;
    const bool female = (m_.sex == AF_SEX_FEMALE);

    /* ---- neck: a short slightly-forward-leaning taper from neck base to jaw -------------- */
    const Vec3 neckBase(0.0f, m_.y.neckBaseY - 0.02f * H, m_.neck.centreZ);
    const Vec3 neckTop(0.0f, m_.y.jawY + 0.004f * H, m_.neck.centreZ - 0.010f * H);

    /* ---- torso profile control points (y, value) ---------------------------------------- */
    const float crotch = m_.y.crotchY;
    const float hipY = m_.y.hipY;
    const float waistY = m_.y.waistY;
    const float underbustY = m_.y.underbustY;
    const float bustY = m_.y.bustY;
    const float armpitY = m_.y.armpitY;
    const float shoulderY = m_.y.shoulderY;
    const float neckBaseY = m_.y.neckBaseY;

    /* rx (side-to-side half width) - note the pinch at the waist and the flare at the hips. */
    rxProfile_.add(crotch - 0.10f * H, m_.hip.rx * 0.86f);
    rxProfile_.add(crotch, m_.hip.rx * 0.94f);
    rxProfile_.add(hipY, m_.hip.rx * 1.02f);
    rxProfile_.add(lerpf(hipY, waistY, 0.5f), lerpf(m_.hip.rx, m_.waist.rx, 0.45f) * 1.01f);
    rxProfile_.add(waistY, m_.waist.rx);
    rxProfile_.add(lerpf(waistY, underbustY, 0.5f), lerpf(m_.waist.rx, m_.underbust.rx, 0.55f));
    rxProfile_.add(underbustY, m_.underbust.rx);
    rxProfile_.add(bustY, m_.chest.rx * 0.98f);
    rxProfile_.add(armpitY, m_.chest.rx * 1.00f);
    rxProfile_.add(lerpf(armpitY, shoulderY, 0.6f), m_.chest.rx * 0.88f);
    rxProfile_.add(shoulderY, m_.chest.rx * 0.72f);
    rxProfile_.add(neckBaseY, m_.neck.rx * 1.05f);
    rxProfile_.build();

    /* rz (front-to-back half depth) - the lumbar curve and thoracic mass live here. */
    rzProfile_.add(crotch - 0.10f * H, m_.hip.rz * 0.90f);
    rzProfile_.add(crotch, m_.hip.rz * 0.98f);
    rzProfile_.add(hipY, m_.hip.rz * 1.04f);
    rzProfile_.add(lerpf(hipY, waistY, 0.5f), m_.waist.rz * 1.06f);
    rzProfile_.add(waistY, m_.waist.rz);
    rzProfile_.add(underbustY, m_.underbust.rz);
    rzProfile_.add(bustY, m_.bust > 0.05f && female ? m_.underbust.rz * 1.08f : m_.chest.rz * 0.99f);
    rzProfile_.add(armpitY, m_.chest.rz);
    rzProfile_.add(lerpf(armpitY, shoulderY, 0.6f), m_.chest.rz * 0.92f);
    rzProfile_.add(shoulderY, m_.chest.rz * 0.80f);
    rzProfile_.add(neckBaseY, m_.neck.rz * 1.02f);
    rzProfile_.build();

    /* cz (centre-line offset) - lumbar lordosis pulls the waist centre backwards, the bust
       pushes the chest centre forwards. Small numbers, big perceptual effect. */
    czProfile_.add(crotch, m_.hip.centreZ + 0.004f);
    czProfile_.add(hipY, m_.hip.centreZ);
    czProfile_.add(waistY, m_.waist.centreZ - 0.006f * H);
    czProfile_.add(bustY, m_.bustSeg.centreZ + (female ? 0.004f * H * m_.bust : 0.0f));
    czProfile_.add(shoulderY, m_.chest.centreZ + 0.002f * H);
    czProfile_.add(neckBaseY, m_.neck.centreZ);
    czProfile_.build();

    /* ---- chains (articulation points) ---------------------------------------------------- */
    for (int s = 0; s < 2; ++s) {
        const int side = (s == 0) ? 1 : -1;
        Chain& c = (side > 0) ? left_ : right_;
        c.side = side;
        const float sx = static_cast<float>(side);

        /* Arm: A-pose with ~9 degrees of abduction, which is the standard rigging stance
           (arms clear of the torso so skinning, cloth and IK all behave). */
        const float abduct = radians(9.0f * (female ? 1.15f : 1.0f));
        c.shoulder = {sx * m_.biacromialHalf, m_.y.shoulderY, -0.006f * H};
        const Vec3 upperDir(sx * std::sin(abduct), -std::cos(abduct), 0.02f);
        c.elbow = c.shoulder + upperDir * m_.upperArmLen;
        const Vec3 foreDir(sx * std::sin(abduct * 0.55f), -std::cos(abduct * 0.55f), -0.015f);
        c.wrist = c.elbow + foreDir * m_.forearmLen;

        /* Hand frame: fingers hang down, palm faces the thigh, index finger toward the front. */
        const Vec3 dirDown = (c.wrist - c.elbow).normalizedOr({0, -1, 0});
        const Vec3 palmNormal(-sx, 0.0f, 0.0f);              /* points at the body    */
        c.palmCentre = c.wrist + dirDown * (m_.handLength * 0.26f);
        const Vec3 across = palmNormal.cross(dirDown).normalizedOr({0, 0, 1});

        const float spacing = m_.palmWidth * 0.265f;
        const Vec3 knuckleLineCentre = c.palmCentre + dirDown * (m_.handLength * 0.20f);
        for (int i = 0; i < 5; ++i) {
            if (i == 0) {
                /* Thumb: planted on the front-inner edge of the palm, opposed ~45 degrees. */
                const Vec3 base = c.palmCentre + across * (spacing * 2.30f) +
                                  palmNormal * (m_.palmThickness * 0.55f) -
                                  dirDown * (m_.handLength * 0.10f);
                const Vec3 thumbDir = rotateToward(dirDown, -palmNormal, radians(52.0f));
                const Vec3 dir2 = rotateToward(thumbDir, across, radians(28.0f)).normalizedOr(thumbDir);
                const float L = m_.fingerLength[0];
                c.fingerKnuckle[0] = base;
                c.fingerJoints[0][0] = base + dir2 * (L * 0.50f);
                c.fingerJoints[0][1] = c.fingerJoints[0][0] + dir2 * (L * 0.28f);
                c.fingerJoints[0][2] = c.fingerJoints[0][1] + dir2 * (L * 0.22f);
                c.fingerTip[0] = c.fingerJoints[0][2];
                continue;
            }
            const float acrossOffset = static_cast<float>(2 - i) * spacing;   /* index..little */
            const Vec3 knuckle = knuckleLineCentre + across * acrossOffset +
                                 palmNormal * (m_.palmThickness * 0.10f);
            const float L = m_.fingerLength[i];
            /* Progressive flexion: 8 / 18 / 26 degrees, the classic relaxed hand. */
            const Vec3 d1 = rotateToward(dirDown, -palmNormal, radians(8.0f));
            const Vec3 d2 = rotateToward(d1, -palmNormal, radians(10.0f));
            const Vec3 d3 = rotateToward(d2, -palmNormal, radians(8.0f));
            c.fingerKnuckle[i] = knuckle;
            c.fingerJoints[i][0] = knuckle + d1 * (L * 0.44f);
            c.fingerJoints[i][1] = c.fingerJoints[i][0] + d2 * (L * 0.32f);
            c.fingerJoints[i][2] = c.fingerJoints[i][1] + d3 * (L * 0.24f);
            c.fingerTip[i] = c.fingerJoints[i][2];
            c.middleTip = c.fingerTip[2];
        }

        /* Legs: hip -> knee -> ankle, with a small natural genu valgum offset and the
           ankle sitting slightly behind the knee. Toe-out of 7 degrees matches a relaxed
           stance and gives the foot field a sensible axis. */
        c.hipJoint = {sx * m_.legSeparation, m_.y.hipY, 0.0f};
        c.knee = {sx * (m_.legSeparation + 0.004f), m_.y.kneeY, 0.006f};
        c.ankle = {sx * (m_.legSeparation + 0.006f), m_.y.ankleY, -0.004f};
        c.heel = {c.ankle.x, m_.y.soleY + m_.footHeight * 0.5f, c.ankle.z + m_.footHeelZ};
        c.ball = {c.ankle.x + sx * 0.008f, m_.y.soleY + m_.footHeight * 0.42f,
                  c.ankle.z - m_.footLength * 0.52f};
        c.toeTip = {c.ball.x, c.ball.y, c.ball.z - m_.toeLength[1] - m_.toeRadius[1]};
    }

    /* ---- face anchors ------------------------------------------------------------------- */
    const float headC = (m_.y.crownY + m_.y.chinY) * 0.5f;
    const float headH = m_.y.crownY - m_.y.chinY;
    const float zFront = -m_.head.rz * 0.98f;                      /* -Z is forward */
    noseBridge_ = {0.0f, m_.y.eyeY - headH * 0.02f, zFront * 0.86f};
    noseTip_ = {0.0f, m_.y.eyeY - headH * 0.075f, zFront * 1.28f};
    chin_ = {0.0f, m_.y.chinY + headH * 0.055f, zFront * 0.90f};
    browRidge_ = {0.0f, m_.y.eyeY + headH * 0.030f, zFront * 0.92f};
    mouthCentre_ = {0.0f, m_.mouthY, zFront * 0.94f};
    for (int s = 0; s < 2; ++s) {
        const float sx = (s == 0) ? 1.0f : -1.0f;
        jawCorner_[s] = {sx * m_.jaw.rx * 1.02f, m_.y.chinY + headH * 0.20f, zFront * 0.42f};
        cheek_[s] = {sx * m_.head.rx * 0.90f, m_.y.chinY + headH * 0.30f, zFront * 0.70f};
        eyeCentre_[s] = {sx * m_.eyeSeparation * 0.5f, m_.y.eyeY, zFront * 0.80f};
        earCentre_[s] = {sx * m_.head.rx * 1.05f, m_.y.eyeY - headH * 0.055f, zFront * 0.10f};
    }
    (void)headC; (void)neckBase; (void)neckTop;
}

/* =======================================================================================
 * Torso
 * ======================================================================================= */
float BodyField::torsoProfileRx(float y) const { return maxf(rxProfile_.eval(y), 0.012f); }
float BodyField::torsoProfileRz(float y) const { return maxf(rzProfile_.eval(y), 0.012f); }
float BodyField::torsoProfileCz(float y) const { return czProfile_.eval(y); }

float BodyField::torso(const Vec3& p) const {
    const float yMin = m_.y.crotchY - m_.thighLen * 0.30f;
    const float yMax = m_.y.neckBaseY + 0.010f;
    const float y = clampf(p.y, yMin, yMax);
    const float rx = torsoProfileRx(y);
    const float rz = torsoProfileRz(y);
    const float cz = torsoProfileCz(y);

    /* Super-elliptical cross section (exponent 2.45) - a pure ellipse makes chests and
       abdomens look like sausages; real torsos are flatter in front. */
    const float nx = p.x / rx;
    const float nz = (p.z - cz) / rz;
    const float e = 2.45f;
    const float k = std::pow(std::pow(std::fabs(nx), e) + std::pow(std::fabs(nz), e), 1.0f / e);
    /* Both terms are SIGNED distances and both are negative inside, which is what makes the
       union form below correct: inside the profile tube max() picks the nearest wall, and
       outside it picks the nearest of the radial surface and the two end caps. The final
       length() term rounds the cap rims, which stops the shoulders from ending in a hard
       band. (Using max() against a non-negative "vertical" term is the classic mistake here:
       it silently returns 0 everywhere inside the slab, i.e. an empty body.) */
    const float radial = (k - 1.0f) * minf(rx, rz);
    const float vertical = maxf(yMin - p.y, p.y - yMax);
    float d = minf(maxf(radial, vertical), 0.0f) +
              Vec3(maxf(radial, 0.0f), maxf(vertical, 0.0f), 0.0f).length();

    const float H = m_.height;
    const bool female = (m_.sex == AF_SEX_FEMALE);
    const float chestZ = -m_.chest.rz * 0.85f;
    const float backZ = m_.chest.rz * 0.85f;
    const float mergeK = lerpf(0.030f, 0.014f, saturate(m_.muscle));

    /* --- chest: pectorals / bust ------------------------------------------------------- */
    if (female && m_.bust > 0.02f) {
        const float r = m_.chest.rx * (0.40f + 0.10f * m_.bust);
        const Vec3 radii(r * 0.92f, r * lerpf(0.85f, 1.05f, m_.bust), r * lerpf(0.82f, 1.02f, m_.bust));
        const float cx = m_.chest.rx * 0.44f;
        const float cz2 = m_.underbust.rz * 0.95f - m_.bustSeg.rz * (0.20f + 0.22f * m_.bust);
        d = smin(d, blob2(p, cx, m_.y.bustY + 0.004f * H, cz2, radii), 0.022f);
    } else {
        const float pecR = m_.chest.rx * (0.42f + 0.10f * m_.muscle);
        const Vec3 radii(pecR, pecR * 0.72f, pecR * 0.52f);
        d = smin(d, blob2(p, m_.chest.rx * 0.46f, m_.y.bustY + 0.005f * H,
                          chestZ * 0.72f, radii), mergeK);
    }

    /* --- abdomen: rectus columns + transverse crease ----------------------------------- */
    const float abW = m_.waist.rx * (0.60f + 0.16f * m_.muscle);
    const float abH = (m_.y.waistY - m_.y.hipY) * 0.34f;
    const Vec3 abRadii(abW * 0.5f, abH, m_.waist.rz * (0.34f + 0.10f * m_.muscle + m_.abDepth * 2.0f));
    d = smin(d, blob2(p, m_.waist.rx * 0.34f, (m_.y.waistY + m_.y.underbustY) * 0.5f - abH * 0.30f,
                      -m_.waist.rz * 0.72f, abRadii), 0.020f);

    /* --- obliques / serratus ----------------------------------------------------------- */
    if (m_.obliqueBulge > 1e-4f) {
        const Vec3 radii(m_.waist.rx * 0.30f, (m_.y.waistY - m_.y.hipY) * 0.28f, m_.waist.rz * 0.34f);
        d = smin(d, blob2(p, m_.waist.rx * 0.82f, lerpf(m_.y.waistY, m_.y.hipY, 0.35f),
                          -m_.waist.rz * 0.30f, radii), mergeK);
    }

    /* --- belly (adipose, heavy builds) -------------------------------------------------- */
    if (m_.bellyBulge > 1e-4f) {
        const Vec3 radii(m_.waist.rx * 0.82f, (m_.y.waistY - m_.y.hipY) * 0.52f, m_.waist.rz * 0.55f);
        d = smin(d, blob(p, {0.0f, lerpf(m_.y.hipY, m_.y.waistY, 0.55f), -m_.waist.rz * 0.95f}, radii), 0.035f);
    }

    /* --- glutes ------------------------------------------------------------------------ */
    {
        const float gz = m_.hip.rz * (0.62f + 0.12f * saturate(m_.bodyFat));
        const Vec3 radii(m_.hip.rx * 0.52f, m_.hip.rz * 0.62f, m_.hip.rz * 0.52f);
        d = smin(d, blob2(p, m_.hip.rx * 0.46f, m_.y.hipY - m_.hip.rz * 0.34f, gz, radii), 0.030f);
    }

    /* --- lumbar / spinal groove (subtractive: the back is not a barrel) -----------------
       Carving rule used everywhere in this file: to remove the region {shape < 0} from a
       field d, you need d = smax(d, -shape, k). Writing `smax(d, shape)` treats the *space
       outside* the shape as solid and carves the entire body away - which is exactly the
       class of sign error that turns an avatar into a shell. */
    {
        const float groove = sdCapsule(p, {0.0f, m_.y.hipY + 0.02f * H, m_.hip.rz * 0.93f},
                                       {0.0f, m_.y.armpitY, m_.chest.rz * 0.90f}, 0.010f);
        d = smax(d, -groove, 0.030f);
    }

    /* --- trapezius --------------------------------------------------------------------- */
    for (int s = 0; s < 2; ++s) {
        const float sx = (s == 0) ? 1.0f : -1.0f;
        const Vec3 from(sx * m_.neck.rx * 0.55f, m_.y.neckBaseY + 0.008f * H, m_.neck.centreZ + 0.004f);
        const Vec3 to(sx * m_.biacromialHalf * 0.96f, m_.y.shoulderY - 0.004f * H, 0.002f);
        d = smin(d, sdTaperedCapsule(p, from, to, m_.neck.rx * 0.42f,
                                     m_.trapeziusBulge * 0.42f + 0.016f), 0.032f);
    }

    /* --- latissimus / ribs (only visibly present on muscular builds) -------------------- */
    if (m_.latBulge > 1e-4f) {
        const Vec3 radii(m_.chest.rx * 0.34f, (m_.y.armpitY - m_.y.waistY) * 0.42f, m_.chest.rz * 0.46f);
        d = smin(d, blob2(p, m_.chest.rx * 0.80f, lerpf(m_.y.armpitY, m_.y.waistY, 0.40f),
                          m_.chest.rz * 0.30f, radii), mergeK);
    }

    /* --- iliac crest / hip bones ------------------------------------------------------- */
    for (int s = 0; s < 2; ++s) {
        const float sx = (s == 0) ? 1.0f : -1.0f;
        d = smin(d, blob(p, {sx * m_.hip.rx * 0.70f, m_.y.hipY + 0.012f * H, -m_.hip.rz * 0.42f},
                         Vec3(m_.hip.rx * 0.34f, m_.hip.rz * 0.20f, m_.hip.rz * 0.30f)), 0.028f);
    }

    /*
     * --- neck ------------------------------------------------------------------------------
     * BOTH domains carry the neck, because the domain cut plane (neckCutY) has to slice a real
     * tube in both of them; the two definitions are therefore identical, term for term, so that
     * the smooth union at the top of the build is a no-op inside the neck and a fillet only where
     * the neck meets the shoulders and the jaw.
     *
     * The tube runs up past the jaw and its cap is buried inside the skull, which is where a neck
     * actually ends. It is safe to do that because the oral shapes are applied to the FINAL union
     * (see BodyField::applyOralShapes) rather than inside the head's field: an earlier revision cut
     * the mouth out of the head alone and then smooth-unioned the trunk over it, so the 5 cm-wide
     * neck tube silently refilled the cavity and the avatar came out with a closed face no matter
     * how well the mouth itself was modelled.
     */
    {
        const Vec3 neckBase(0.0f, m_.y.neckBaseY - 0.045f * H, m_.neck.centreZ);
        const Vec3 neckTop(0.0f, m_.y.jawY + 0.010f * H, m_.neck.centreZ - 0.010f * H);
        d = smin(d, sdTaperedCapsule(p, neckBase, neckTop, m_.neck.rx * 1.22f, m_.neck.rx * 0.94f), 0.030f);
        /* sterno-mastoid definition on low-body-fat builds */
        if (m_.bodyFat < 0.24f) {
            for (int s = 0; s < 2; ++s) {
                const float sx = (s == 0) ? 1.0f : -1.0f;
                const Vec3 a(sx * m_.neck.rx * 0.62f, m_.y.neckBaseY - 0.012f * m_.height, -m_.neck.rz * 0.55f);
                const Vec3 b(sx * m_.neck.rx * 0.30f, m_.y.jawY, -m_.neck.rz * 0.80f);
                d = smin(d, sdCapsule(p, a, b, m_.neck.rx * 0.13f), 0.022f);
            }
        }
    }

    /* --- fine surface detail: muscle striation & skin break-up -------------------------
       Applied as a bounded displacement so it never dominates the silhouette. It exists so
       the baked normal map and the mesh agree: geometric micro-relief reads as real skin
       under raking light, which flat-shaded decals cannot fake. */
    {
        const float striation = noise_.ridged(p * 26.0f, 3, 2.05f, 0.55f) - 0.42f;
        const float pores = noise_.cell(p, 78.0f) - 0.45f;
        const float amp = 0.0016f * (1.0f - saturate(m_.bodyFat)) + 0.0009f;
        d -= (striation * 0.75f + pores * 0.35f) * amp;
    }
    return d;
}

/* =======================================================================================
 * Arms
 * ======================================================================================= */
float BodyField::armChain(const Vec3& p, const Chain& c) const {
    const float sx = static_cast<float>(c.side);
    /* Upper arm and forearm as tapered capsules; the elbow keeps a slightly larger radius
       so the joint does not pinch when the arm bends during animation. */
    float d = sdTaperedCapsule(p, c.shoulder, c.elbow, m_.upperArmR * 1.02f, m_.elbowR);
    d = smin(d, sdTaperedCapsule(p, c.elbow, c.wrist, m_.elbowR * 0.98f, m_.wristR), 0.016f);

    /* Deltoid cap - the single most important shape for reading a shoulder. */
    const Vec3 deltoidCentre = c.shoulder + Vec3(sx * m_.deltoidR * 0.28f, m_.deltoidR * 0.30f, 0.0f);
    d = smin(d, blob(p, deltoidCentre,
                     Vec3(m_.deltoidR * 1.05f, m_.deltoidR * 1.10f, m_.deltoidR * 0.92f)), 0.030f);

    /* Biceps (anterior), triceps (posterior), brachioradialis (forearm mass). */
    const Vec3 upDir = (c.elbow - c.shoulder).normalizedOr({0, -1, 0});
    const Vec3 upMid = c.shoulder + upDir * (m_.upperArmLen * 0.46f);
    if (m_.bicepBulge > 1e-4f) {
        d = smin(d, blob(p, upMid + Vec3(0, 0, -m_.upperArmR * 0.62f),
                         Vec3(m_.upperArmR * 0.62f, m_.upperArmLen * 0.24f, m_.upperArmR * 0.44f + m_.bicepBulge)), 0.020f);
    }
    if (m_.tricepBulge > 1e-4f) {
        d = smin(d, blob(p, c.shoulder + upDir * (m_.upperArmLen * 0.44f) + Vec3(0, 0, m_.upperArmR * 0.60f),
                         Vec3(m_.upperArmR * 0.60f, m_.upperArmLen * 0.26f, m_.upperArmR * 0.42f + m_.tricepBulge)), 0.022f);
    }
    if (m_.brachioradialisBulge > 1e-4f) {
        const Vec3 foreDir = (c.wrist - c.elbow).normalizedOr({0, -1, 0});
        d = smin(d, blob(p, c.elbow + foreDir * (m_.forearmLen * 0.26f) + Vec3(0, 0, -m_.forearmR * 0.55f),
                         Vec3(m_.forearmR * 0.62f, m_.forearmLen * 0.22f, m_.forearmR * 0.52f + m_.brachioradialisBulge)), 0.018f);
    }
    /* Ulna groove (subtractive) - a cheap trick that instantly makes an arm look anatomical. */
    {
        const Vec3 foreDir = (c.wrist - c.elbow).normalizedOr({0, -1, 0});
        const Vec3 grooveAxis = foreDir.cross(Vec3(sx, 0, 0)).normalizedOr({0, 0, 1});
        const float groove = sdCapsule(p, c.elbow + grooveAxis * (m_.forearmR * 0.62f),
                                       c.wrist + grooveAxis * (m_.wristR * 0.62f), 0.009f);
        d = smax(d, -groove, 0.020f);
    }
    /* Wrist cap so the hand domain has a matching tube on its side of the cut plane. */
    d = smin(d, sdCapsule(p, c.wrist, c.wrist + (c.palmCentre - c.wrist) * 0.35f, m_.wristR * 0.98f), 0.010f);
    return d;
}

/* =======================================================================================
 * Legs
 * ======================================================================================= */
float BodyField::legChain(const Vec3& p, const Chain& c) const {
    const float sx = static_cast<float>(c.side);
    float d = sdTaperedCapsule(p, c.hipJoint, c.knee, m_.thighRx * 1.03f, m_.kneeR);
    d = smin(d, sdTaperedCapsule(p, c.knee, c.ankle, m_.calfRx * 0.94f, m_.ankleRx), 0.018f);

    const Vec3 thighDir = (c.knee - c.hipJoint).normalizedOr({0, -1, 0});
    const Vec3 kneeToAnkle = (c.ankle - c.knee).normalizedOr({0, -1, 0});
    const float legMerge = lerpf(0.034f, 0.022f, saturate(m_.muscle));

    /* Quads (anterior), hamstrings (posterior), adductors (medial). */
    if (m_.quadBulge > 1e-4f) {
        const Vec3 radii(m_.thighRx * 0.62f, m_.thighLen * 0.30f, m_.thighRz * 0.46f + m_.quadBulge);
        d = smin(d, blob(p, c.hipJoint + thighDir * (m_.thighLen * 0.42f) + Vec3(0, 0, -m_.thighRz * 0.52f), radii), legMerge);
    }
    if (m_.hamstringBulge > 1e-4f) {
        const Vec3 radii(m_.thighRx * 0.58f, m_.thighLen * 0.24f, m_.thighRz * 0.44f + m_.hamstringBulge);
        d = smin(d, blob(p, c.hipJoint + thighDir * (m_.thighLen * 0.34f) + Vec3(0, 0, m_.thighRz * 0.50f), radii), legMerge);
    }
    if (m_.adductorBulge > 1e-4f) {
        const Vec3 radii(m_.thighRx * 0.46f, m_.thighLen * 0.28f, m_.thighRz * 0.42f);
        d = smin(d, blob(p, c.hipJoint + thighDir * (m_.thighLen * 0.34f) - Vec3(sx * m_.thighRx * 0.55f, 0, 0), radii), legMerge);
    }

    /* Knee: patella in front, condyles on the sides, popliteal hollow behind. */
    d = smin(d, blob(p, c.knee + Vec3(0, m_.kneeR * 0.10f, -m_.kneeR * 0.62f),
                     Vec3(m_.kneeR * 0.60f, m_.kneeR * 0.72f, m_.kneeR * 0.42f)), 0.018f);
    {
        const float popliteal = sdEllipsoid(p - (c.knee + Vec3(0, 0, m_.kneeR * 0.85f)),
                                            Vec3(m_.kneeR * 0.52f, m_.kneeR * 0.62f, m_.kneeR * 0.34f));
        d = smax(d, -popliteal - m_.kneeR * 0.10f, 0.020f);
    }

    /* Gastrocnemius (two heads), soleus (low, wide), tibialis anterior. */
    if (m_.gastrocBulge > 1e-4f) {
        for (int h = 0; h < 2; ++h) {
            const float hx = (h == 0 ? 1.0f : -1.0f) * sx * m_.calfRx * 0.26f;
            const Vec3 radii(m_.calfRx * 0.44f, m_.shankLen * 0.24f, m_.calfRz * 0.42f + m_.gastrocBulge);
            d = smin(d, blob(p, c.knee + kneeToAnkle * (m_.shankLen * 0.30f) + Vec3(hx, 0, m_.calfRz * 0.52f), radii), 0.026f);
        }
    }
    if (m_.tibialisBulge > 1e-4f) {
        const Vec3 radii(m_.calfRx * 0.38f, m_.shankLen * 0.22f, m_.calfRz * 0.34f + m_.tibialisBulge);
        d = smin(d, blob(p, c.knee + kneeToAnkle * (m_.shankLen * 0.34f) + Vec3(-sx * m_.calfRx * 0.42f, 0, -m_.calfRz * 0.34f), radii), 0.022f);
    }
    /* Ankle bones: the visual tell of a real ankle. */
    for (int b = 0; b < 2; ++b) {
        const float bx = (b == 0 ? -1.0f : 1.0f) * m_.ankleRx * 0.78f;
        const float by = (b == 0 ? 0.012f : 0.006f);
        d = smin(d, blob(p, c.ankle + Vec3(bx, by, 0.004f),
                         Vec3(m_.ankleRx * 0.34f, m_.ankleRx * 0.34f, m_.ankleRx * 0.30f)), 0.014f);
    }
    /* Wrist cap for the foot domain. */
    d = smin(d, sdCapsule(p, c.ankle, c.ankle + (c.heel - c.ankle) * 0.35f, m_.ankleRx), 0.010f);
    return d;
}

/* =======================================================================================
 * Feet
 * ======================================================================================= */
float BodyField::footShape(const Vec3& p, const Chain& c) const {
    const float sx = static_cast<float>(c.side);
    /*
     * The foot is built in a frame anchored at the ANKLE, with toe-out expressed as a yaw about
     * the vertical through that point. Building it around the world origin instead (as an earlier
     * revision did) puts both feet on the midline: the left and right soles overlap in the gap
     * between the legs, the two blobs bridge into one slab under the crotch, and the mesh's
     * lowest point stops being the sole at all. Every lateral offset below is therefore relative
     * to the ankle, and the ankle stump capsule sits at the local origin so the foot domain stays
     * welded to the leg that the shin field is building.
     */
    const float yaw = radians(7.0f) * sx;
    const float cs = std::cos(-yaw), sn = std::sin(-yaw);
    const Vec3 r = p - c.ankle;
    const Vec3 q(r.x * cs - r.z * sn, r.y, r.x * sn + r.z * cs);
    const auto toLocal = [&](const Vec3& w) {
        const Vec3 t = w - c.ankle;
        return Vec3(t.x * cs - t.z * sn, t.y, t.x * sn + t.z * cs);
    };
    const Vec3 heelL = toLocal(c.heel);
    const Vec3 ballL = toLocal(c.ball);

    const float halfW = m_.footWidth * 0.5f;
    /* `ground` is the contact plane: soleY + soleGap. ground == 0 in the default build, so the
       avatar drops onto the floor plane exactly. Every primitive below is positioned by its
       lowest point, which is what keeps the sole flat instead of submerged. */
    const float soleGap = 0.002f;
    const float groundY = m_.y.soleY + soleGap;          /* world contact plane, usually y = 0 */
    /* The placement maths below is written in the ankle-local frame, so the contact plane has to
       be expressed ankle-relative as well - mixing the two put the whole foot 63 mm too high. */
    const float ground = groundY - c.ankle.y;
    const float archZ = (heelL.z + ballL.z) * 0.5f;

    /* Main foot mass: heel wedge -> arch -> forefoot, as three overlapping rounded boxes.
       Heel width 0.72 * forefoot, which is what stops the "clown shoe" look of a single box. */
    const float heelHalfH = m_.footHeight * 0.50f;
    const float archHalfH = m_.footHeight * 0.44f;
    const float ballHalfH = m_.footHeight * 0.40f;
    const float heelRound = m_.footHeight * 0.16f;
    const float archRound = m_.footHeight * 0.14f;
    const float ballRound = m_.footHeight * 0.13f;
    const float heelLen = std::fabs(ballL.z - heelL.z) * 0.42f;
    const float footLen = std::fabs(ballL.z - heelL.z);

    float d = sdRoundBox(q - Vec3(heelL.x, ground + heelHalfH + heelRound, heelL.z + heelLen * 0.20f),
                         Vec3(halfW * 0.72f, heelHalfH, heelLen), heelRound);
    d = smin(d, sdRoundBox(q - Vec3(archZ * 0.0f + ballL.x * 0.35f, ground + archHalfH + archRound, archZ),
                           Vec3(halfW * 0.80f, archHalfH, footLen * 0.26f), archRound), 0.016f);
    d = smin(d, sdRoundBox(q - Vec3(ballL.x + sx * 0.002f, ground + ballHalfH + ballRound,
                                    ballL.z + footLen * 0.06f),
                           Vec3(halfW * 1.02f, ballHalfH, footLen * 0.20f), ballRound), 0.014f);

    /* Forefoot / midfoot bridge so the ball and heel do not read as separate blocks. */
    d = smin(d, sdRoundBox(q - Vec3(ballL.x * 0.6f, ground + archHalfH * 0.92f + archRound,
                                    (ballL.z + heelL.z) * 0.5f),
                           Vec3(halfW * 0.78f, archHalfH * 0.92f, footLen * 0.30f), archRound), 0.018f);

    /* Longitudinal arch (subtractive: smax(d, -shape, k) carves instead of adding) and the
       metatarsal ridge. The arch ellipsoid is centred just above the contact plane so the cut
       eats the midfoot sole but leaves heel and ball on the ground. */
    {
        const float arch = sdEllipsoid(q - Vec3(sx * halfW * 0.32f, ground + m_.footHeight * 0.10f, archZ),
                                       Vec3(halfW * 0.52f, m_.footHeight * 0.42f, footLen * 0.24f));
        d = smax(d, -arch, 0.016f);
        d = smin(d, sdCapsule(q, Vec3(-halfW * 0.6f, ground + ballHalfH * 1.05f, ballL.z),
                              Vec3(halfW * 0.6f, ground + ballHalfH * 0.98f, ballL.z), m_.footHeight * 0.18f), 0.020f);
    }

    /* Five toes, phalangeal-length scaled, with slight plantar flexion at the ball.
       `tx` is medial-to-lateral relative to the ankle, big toe on the medial side. */
    for (int i = 0; i < 5; ++i) {
        const float spread = (static_cast<float>(2 - i)) * (m_.footWidth * 0.185f);
        const float tx = -sx * spread;
        const float ty = ground + m_.toeRadius[i] * 0.92f;
        const Vec3 from(tx, ty, ballL.z - m_.toeRadius[i] * 0.4f);
        const Vec3 to(tx + sx * (0.004f * static_cast<float>(i)), ty - m_.footHeight * 0.015f,
                      ballL.z - m_.toeLength[i] - m_.toeRadius[i] * 0.4f);
        d = smin(d, sdTaperedCapsule(q, from, to, m_.toeRadius[i] * 1.05f, m_.toeRadius[i] * 0.92f), 0.008f);
        /* Nail plate bulge on the dorsal side of the distal phalanx. */
        d = smin(d, blob(q, to + Vec3(0, m_.toeRadius[i] * 0.34f, -m_.toeRadius[i] * 0.20f),
                         Vec3(m_.toeRadius[i] * 0.62f, m_.toeRadius[i] * 0.22f, m_.toeRadius[i] * 0.60f)), 0.006f);
    }
    /* Ankle stump, at the local origin, so the foot and shin fields overlap across the joint. */
    d = smin(d, sdCapsule(q, Vec3(0, -0.025f, 0.0f), Vec3(0, 0.025f, 0.002f), m_.ankleRx * 1.02f), 0.014f);

    /*
     * Flatten the sole onto the contact plane.
     *
     * Every overlap above is a smooth union, and a smooth union pulls the surface OUTWARD: two
     * coincident surfaces blend to a surface up to k/4 further out, so the three sole boxes (whose
     * bottoms all sit exactly on `ground`) blended with k = 14-18 mm pushed the sole 4-5 mm BELOW
     * the floor. Measured: the field's lowest surface point was y = -0.0035 with ground at +0.002.
     * Intersecting with the half-space y >= ground afterwards both removes everything below the
     * plane and leaves a genuinely flat sole to stand on, which is what the renderer and the GLB
     * exporter need. k is small (4 mm) so the edge where the sole meets the side of the foot stays
     * a crisp corner - a foot is not a soft object.
     */
    d = smax(d, ground - q.y, 0.004f);
    return d;
}

/* =======================================================================================
 * Hands
 * ======================================================================================= */
float BodyField::handShape(const Vec3& p, const Chain& c) const {
    const float sx = static_cast<float>(c.side);
    const Vec3 dirDown = (c.wrist - c.elbow).normalizedOr({0, -1, 0});
    const Vec3 palmNormal(-sx, 0.0f, 0.0f);
    const Vec3 across = palmNormal.cross(dirDown).normalizedOr({0, 0, 1});

    /* Palm: a flattened rounded box (thinner at the wrist, wider at the knuckles). */
    float d = sdRoundBox(p - c.palmCentre,
                         Vec3(m_.palmWidth * 0.5f, m_.handLength * 0.26f, m_.palmThickness * 0.5f),
                         m_.palmThickness * 0.45f);
    {
        /* Rotate the palm block into the hand frame by blending three sliver boxes; cheaper
           and more stable than building a rotated box SDF with sheared basis. */
        const Vec3 q = p - c.palmCentre;
        const float alongDown = q.dot(dirDown);
        const float alongAcross = q.dot(across);
        const float alongNormal = q.dot(palmNormal);
        const float halfLen = m_.handLength * 0.26f;
        const float taper = 1.0f + 0.10f * saturate(-alongDown / maxf(halfLen, 1e-4f));
        d = smin(d, sdRoundBox(Vec3(alongAcross, alongDown, alongNormal),
                               Vec3(m_.palmWidth * 0.5f * taper, halfLen, m_.palmThickness * 0.5f),
                               m_.palmThickness * 0.45f), 0.012f);
        /* Thenar (thumb) pad and hypothenar pad - both read strongly at any distance. */
        d = smin(d, blob(Vec3(alongAcross, alongDown, alongNormal),
                         Vec3(m_.palmWidth * 0.42f, m_.handLength * 0.05f, -m_.palmThickness * 0.10f),
                         Vec3(m_.palmWidth * 0.32f, m_.handLength * 0.16f, m_.palmThickness * 0.52f)), 0.018f);
    }

    /* Wrist stump for the cut plane. */
    d = smin(d, sdCapsule(p, c.wrist - dirDown * 0.02f, c.wrist + dirDown * 0.012f, m_.wristR), 0.012f);

    /* Fingers: three phalanges each, with joint capsules that keep the silhouette round. */
    for (int i = 0; i < 5; ++i) {
        const Vec3 j0 = c.fingerKnuckle[i];
        const Vec3 j1 = c.fingerJoints[i][0];
        const Vec3 j2 = c.fingerJoints[i][1];
        const Vec3 j3 = c.fingerJoints[i][2];
        const float r0 = m_.fingerRadius[i];
        const float r1 = r0 * (i == 0 ? 0.96f : 0.90f);
        const float r2 = r0 * (i == 0 ? 0.92f : 0.80f);
        const float r3 = r0 * (i == 0 ? 0.88f : 0.68f);
        /* Blend radius kept small (2.5 mm): adjacent fingers are only ~2 mm apart at the base,
           and a blend comparable to that gap would fuse them into a mitten. The phalanges of
           one finger overlap along their axis, so the small radius still reads as a smooth joint. */
        d = smin(d, sdTaperedCapsule(p, j0, j1, r0, r1), 0.0025f);
        d = smin(d, sdTaperedCapsule(p, j1, j2, r1, r2), 0.0025f);
        d = smin(d, sdTaperedCapsule(p, j2, j3, r2, r3), 0.0025f);
        /* Knuckle and inter-phalangeal joint bulges. */
        d = smin(d, blob(p, j1, Vec3(r1 * 1.10f, r1 * 1.10f, r1 * 1.10f)), 0.0025f);
        d = smin(d, blob(p, j2, Vec3(r2 * 1.08f, r2 * 1.08f, r2 * 1.08f)), 0.0022f);
        /* Fingertip pad + nail bed on the dorsal side. */
        d = smin(d, blob(p, j3 + (j3 - j2).normalizedOr({0, -1, 0}) * (r3 * 0.35f),
                         Vec3(r3 * 0.90f, r3 * 0.90f, r3 * 0.90f)), 0.0022f);
        const Vec3 dorsal = (j3 - j2).normalizedOr({0, -1, 0}).cross(across).normalizedOr(palmNormal);
        d = smin(d, blob(p, j3 - dorsal * (r3 * 0.35f) + (j3 - j2).normalizedOr({0, -1, 0}) * (r3 * 0.75f),
                         Vec3(r3 * 0.68f, r3 * 0.20f, r3 * 0.55f)), 0.0020f);
    }
    return d;
}

/* =======================================================================================
 * Head and face
 * ======================================================================================= */
/* ==========================================================================================
 * Facial sub-shapes.
 *
 * Everything below is shared: headShape() unions them into the head surface and
 * classifyRegion() asks the same functions which material a surface point belongs to. The head
 * is otherwise one smooth mass, and the difference between "a face" and "a face with a mouth" is
 * entirely about what is carved out of it and what sits inside the cavity.
 * ========================================================================================== */

Vec3 BodyField::archCentre() const {
    /*
     * One anchor for the whole mouth. The dental arch, the oral cavity, the aperture and the
     * classifier are all expressed in terms of this point, because the first version of this code
     * placed the teeth from one formula and the void they live in from another, and the two ended
     * up 7 mm apart: every tooth was buried in the cheek and the mouth contained no geometry at
     * all. Anything that has to agree is derived from here, once.
     */
    const Vec3 m = mouthCentre_;
    const float archRz = m_.head.rz * 0.30f;
    const float band = m_.lipWidth * 0.15f;
    return Vec3(m.x, m.y, m.z + 0.002f + archRz + band + 0.003f);
}

float BodyField::lipsShape(const Vec3& p) const {
    const float lw = m_.lipWidth;
    const Vec3 m = mouthCentre_;
    const float upThick = lw * (m_.sex == AF_SEX_FEMALE ? 0.20f : 0.16f);
    const float loThick = lw * (m_.sex == AF_SEX_FEMALE ? 0.26f : 0.20f);
    float lips = sdEllipsoid(p - Vec3(m.x, m.y + upThick * 0.60f, m.z), Vec3(lw * 0.50f, upThick, lw * 0.26f));
    lips = smin(lips, sdEllipsoid(p - Vec3(m.x, m.y - loThick * 0.55f, m.z), Vec3(lw * 0.46f, loThick, lw * 0.28f)), 0.010f);
    for (int side = 0; side < 2; ++side) {
        const float sx = (side == 0) ? 1.0f : -1.0f;
        lips = smin(lips, blob(p, Vec3(m.x + sx * lw * 0.50f, m.y - lw * 0.02f, m.z - lw * 0.04f),
                               Vec3(lw * 0.10f, lw * 0.14f, lw * 0.12f)), 0.008f);
    }
    return lips;
}

float BodyField::mouthAperture(const Vec3& p) const {
    /*
     * The opening itself: a 40 mm wide, 2.6 mm tall slot cut clean through the front wall of the
     * lips. This is what turns the vertical gap between the vermilion envelopes into a real hole
     * rather than a hidden pocket, and it is why the upper and lower lips read as separate forms
     * with a line between them: everything above the slot is the upper lip, everything below is
     * the lower lip, and the slot is the dark interior.
     */
    const Vec3 m = mouthCentre_;
    const float lw = m_.lipWidth;
    /*
     * Depth is 25% of the head's front-to-back radius, which puts the slot's leading face a few
     * millimetres in FRONT of the lip surface. That is deliberate and load-bearing: at 16% the slot
     * stopped short of the skin, the mouth was a closed pocket with a smooth face over it, and the
     * avatar had no mouth line at all. The slot must break the surface; everything above and below
     * it is then the upper and lower lip by construction.
     */
    const Vec3 c(m.x, m.y, m.z + 0.004f);
    return sdRoundBox(p - c, Vec3(m_.mouthWidth * 0.40f, lw * 0.012f, m_.head.rz * 0.25f), 0.0012f);
}

float BodyField::oralCavity(const Vec3& p) const {
    /*
     * The void behind the aperture, sized to CONTAIN the dental arch and the tongue with a small
     * clearance. An elliptical column in the horizontal plane, clipped vertically to a quarter of
     * the face height.
     *
     * The ellipse is not a stylistic choice. A box would have a flat front face, and a flat face
     * at the lip plane is 3-7 mm in front of the skin at the corners of the mouth (the head curves
     * away, the box does not), so the carve would punch holes in the cheeks. An ellipse whose
     * front is tangent to the arch retreats with x exactly as the cheek does, and never breaks the
     * surface except through the aperture.
     */
    const Vec3 c = archCentre();
    const float h = m_.y.eyeY - m_.y.chinY;
    const float archRx = m_.mouthWidth * 0.46f;
    const float archRz = m_.head.rz * 0.30f;
    const float band = m_.lipWidth * 0.15f;
    const float rx = archRx + band + 0.004f;
    const float rz = archRz + band + 0.006f;
    const float ex = (p.x - c.x) / rx;
    const float ez = (p.z - c.z) / rz;
    float cavity = (std::sqrt(ex * ex + ez * ez) - 1.0f) * minf(rx, rz);
    cavity = smax(cavity, std::fabs(p.y - c.y) - h * 0.125f, 0.004f);
    return cavity;
}

float BodyField::teethShape(const Vec3& p) const {
    /*
     * Dental arches: an elliptical band in the horizontal plane, closed at the front and open at
     * the back (a horseshoe), split at the occlusal plane into an upper and a lower arch.
     *
     * The band is the tooth row plus the alveolar bone around it; the split at the lip line is
     * where the incisal edges meet. Everything above the split and inside the band is the upper
     * arch: crowns in the lowest 6 mm, gums above them, roots disappearing into the maxilla. Same
     * mirrored below. The result is one connected shell per arch, closing on itself inside the
     * bone, which is what a skull actually looks like and what a watertight mesh needs.
     */
    const Vec3 c = archCentre();
    const float archRx = m_.mouthWidth * 0.46f;
    const float archRz = m_.head.rz * 0.30f;
    const float band = m_.lipWidth * 0.15f;
    const float crownH = (m_.y.eyeY - m_.y.chinY) * 0.052f;
    const float gumH = crownH * 1.7f;

    const float ex = (p.x - c.x) / archRx;
    const float ez = (p.z - c.z) / archRz;
    const float radial = (std::sqrt(ex * ex + ez * ez) - 1.0f) * minf(archRx, archRz);
    float arch = std::fabs(radial) - band;
    arch = smax(arch, p.z - (c.z + archRz * 0.45f), 0.002f);      /* open the back */

    float upper = smax(arch, c.y - p.y, 0.0015f);
    upper = smax(upper, p.y - (c.y + crownH + gumH), 0.0025f);
    float lower = smax(arch, p.y - c.y, 0.0015f);
    lower = smax(lower, (c.y - crownH - gumH) - p.y, 0.0025f);
    return minf(upper, lower);
}

float BodyField::tongueShape(const Vec3& p) const {
    const Vec3 m = mouthCentre_;
    const float h = m_.y.eyeY - m_.y.chinY;
    const Vec3 c(m.x, m.y - h * 0.045f, m.z + m_.head.rz * 0.30f);
    float t = sdEllipsoid(p - c, Vec3(m_.mouthWidth * 0.33f, h * 0.030f, m_.head.rz * 0.30f));
    /* Tip, angled up towards the back of the upper incisors. */
    t = smin(t, blob(p, c + Vec3(0.0f, h * 0.012f, -m_.head.rz * 0.28f),
                     Vec3(m_.mouthWidth * 0.20f, h * 0.016f, m_.head.rz * 0.12f)), 0.004f);
    return t;
}

float BodyField::eyeballShape(const Vec3& p, int side) const {
    /*
     * Sclera sphere plus a corneal bulge on the front. The bulge matters for more than looks: the
     * renderer refracts through the cornea, and without a protruding anterior surface there is no
     * refraction volume to sample, only a flat disc painted with an iris.
     */
    const int idx = (side > 0) ? 0 : 1;
    const Vec3 e = eyeCentre_[static_cast<size_t>(idx)];
    const float r = m_.eyeRadius;
    /* The socket ceiling used to sit at the eyeball centre; lifting it by a fifth of the radius
       gives the lid a place to close onto and stops the sclera from being sliced flat on top. */
    const float lidClear = r * 0.22f;
    float eyeball = (p - Vec3(e.x, e.y + lidClear * 0.0f, e.z)).length() - r;
    const Vec3 corneaCentre = e + Vec3(0.0f, 0.0f, -r * 0.58f);
    eyeball = smin(eyeball, sdEllipsoid(p - corneaCentre, Vec3(r * 0.74f, r * 0.68f, r * 0.66f)), 0.005f);
    return eyeball;
}

float BodyField::browShape(const Vec3& p) const {
    /* Eyebrow band: a dilated copy of the brow ridge capsule, so it selects the ridge's surface
       and about a millimetre around it - enough for the fibre material to paint the hair. */
    const float r = m_.eyeRadius;
    const float browLift = (m_.sex == AF_SEX_FEMALE) ? 1.15f : 1.0f;
    float band = 1e9f;
    for (int side = 0; side < 2; ++side) {
        const float sx = (side == 0) ? 1.0f : -1.0f;
        const Vec3 e = eyeCentre_[static_cast<size_t>(side)];
        const Vec3 inner(e.x - sx * r * 1.30f, e.y + r * 1.25f * browLift, e.z + r * 0.12f);
        const Vec3 outer(e.x + sx * r * 1.70f, e.y + r * 1.05f * browLift, e.z - r * 0.10f);
        band = minf(band, sdTaperedCapsule(p, inner, outer, r * 0.30f, r * 0.20f));
    }
    return band - m_.eyeRadius * 0.07f;
}

float BodyField::skull(const Vec3& p) const {
    const float headH = m_.y.crownY - m_.y.chinY;
    const float headC = (m_.y.crownY + m_.y.chinY) * 0.5f;
    const Vec3 origin(0.0f, headC, 0.0f);
    const Vec3 q = p - origin;

    /* Cranium: an ellipsoid with the occiput pulled back and the vertex flattened slightly.
       The vertical semi-axis is exactly headH/2 so that the very top of the skull IS the
       stature landmark: a crown that overshoots makes every height check in the SDK a lie. */
    const Vec3 radii(m_.head.rx * 1.03f, headH * 0.50f, m_.head.rz * 1.10f);
    float d = sdEllipsoid(q, radii);
    /* Crown cap: tangent to y = crownY from below (centre at crownY - r, radius r). */
    {
        const float crownR = headH * 0.16f;
        d = smin(d, blob(p, {0.0f, m_.y.crownY - crownR, m_.head.rz * 0.35f},
                         Vec3(m_.head.rx * 0.95f, crownR, m_.head.rz * 0.72f)), 0.030f);
    }
    /* Occiput. */
    d = smin(d, blob(p, {0.0f, headC - headH * 0.06f, m_.head.rz * 0.86f},
                     Vec3(m_.head.rx * 0.82f, headH * 0.20f, m_.head.rz * 0.34f)), 0.030f);
    /* Mandible / jawline, narrower in front, with an angular chin. */
    d = smin(d, blob(p, {0.0f, m_.y.chinY + headH * 0.10f, 0.0f},
                     Vec3(m_.jaw.rx * 1.02f, headH * 0.14f, m_.jaw.rz * 1.02f)), 0.032f);
    d = smin(d, blob(p, chin_, Vec3(m_.head.rx * 0.34f, headH * 0.07f, m_.head.rz * 0.34f)), 0.022f);
    for (int s = 0; s < 2; ++s) {
        d = smin(d, blob(p, jawCorner_[s], Vec3(m_.jaw.rx * 0.34f, headH * 0.09f, m_.jaw.rz * 0.42f)), 0.024f);
        d = smin(d, blob(p, cheek_[s], Vec3(m_.head.rx * 0.34f, headH * 0.11f, m_.head.rz * 0.30f)), 0.026f);
        /* Zygomatic arch. */
        d = smin(d, sdCapsule(p, {cheek_[s].x * 1.02f, m_.y.eyeY - headH * 0.03f, cheek_[s].z * 0.86f},
                              earCentre_[s] + Vec3(0, 0, -m_.head.rz * 0.30f), m_.head.rx * 0.06f), 0.020f);
    }
    /* Brow ridge (subtle: strong ridges read as male, the amplitude comes from sex/build). */
    {
        const float ridgeAmp = (m_.sex == AF_SEX_FEMALE) ? 0.62f : 1.0f;
        d = smin(d, sdCapsule(p, {browRidge_.x - m_.head.rx * 0.78f * ridgeAmp, browRidge_.y, browRidge_.z},
                              {browRidge_.x + m_.head.rx * 0.78f * ridgeAmp, browRidge_.y, browRidge_.z},
                              m_.head.rx * 0.13f * ridgeAmp), 0.024f);
    }
    /* Temple hollows and cheek hollows (subtractive). The carve volumes are centred slightly
       OUTSIDE the skull surface, so only their inner wall intersects the head and the result
       is a shallow dish rather than a bite taken out of the face. */
    for (int s = 0; s < 2; ++s) {
        const float sx = (s == 0) ? 1.0f : -1.0f;
        d = smax(d, -sdEllipsoid(p - Vec3(sx * m_.head.rx * 1.16f, m_.y.eyeY + headH * 0.06f,
                                          -m_.head.rz * 0.28f),
                                 Vec3(m_.head.rx * 0.42f, headH * 0.10f, m_.head.rz * 0.40f)), 0.026f);
        if (m_.bodyFat < 0.26f) {
            d = smax(d, -sdEllipsoid(p - Vec3(sx * m_.head.rx * 0.94f, m_.y.chinY + headH * 0.30f,
                                              -m_.head.rz * 0.92f),
                                     Vec3(m_.head.rx * 0.34f, headH * 0.115f, m_.head.rz * 0.34f)), 0.024f);
        }
    }
    /* Ears: flattened, slightly rotated discs with a concha cavity. */
    for (int s = 0; s < 2; ++s) {
        const float sx = (s == 0) ? 1.0f : -1.0f;
        const Vec3 e = earCentre_[s];
        const float tilt = radians(12.0f) * sx;
        const Vec3 rq(p.x - e.x, p.y - e.y, p.z - e.z);
        const float rx = rq.x * std::cos(tilt) - rq.y * std::sin(tilt);
        const float ry = rq.x * std::sin(tilt) + rq.y * std::cos(tilt);
        const Vec3 er(rx, ry, rq.z);
        float ear = sdEllipsoid(er, Vec3(m_.head.rx * 0.075f, m_.earLength * 0.52f, m_.head.rz * 0.30f));
        ear = smax(ear, -sdEllipsoid(er - Vec3(sx * m_.head.rx * 0.03f, 0, -m_.head.rz * 0.03f),
                                     Vec3(m_.head.rx * 0.055f, m_.earLength * 0.34f, m_.head.rz * 0.20f)) - 0.004f, 0.010f);
        d = smin(d, ear, 0.016f);
    }
    return d;
}

float BodyField::faceFeatures(const Vec3& p) const {
    const float headH = m_.y.crownY - m_.y.chinY;
    float d = skull(p);

    /* Nose: dorsum, tip, alae, nostril notches. The ridge runs from the brow to the tip. */
    {
        const Vec3 bridge(noseBridge_.x, noseBridge_.y, noseBridge_.z);
        const Vec3 tip(noseTip_.x, noseTip_.y, noseTip_.z);
        const float width = m_.head.rx * 0.19f;
        float nose = sdTaperedCapsule(p, bridge + Vec3(0, headH * 0.02f, 0), tip, width * 0.62f, width * 0.86f);
        nose = smin(nose, blob(p, tip + Vec3(0, -headH * 0.008f, width * 0.10f),
                               Vec3(width * 0.72f, width * 0.62f, width * 0.72f)), 0.010f);
        for (int s = 0; s < 2; ++s) {
            const float sx = (s == 0) ? 1.0f : -1.0f;
            nose = smin(nose, blob(p, tip + Vec3(sx * width * 0.72f, -headH * 0.004f, width * 0.05f),
                                   Vec3(width * 0.48f, width * 0.40f, width * 0.44f)), 0.008f);
            const float notch = sdEllipsoid(p - (tip + Vec3(sx * width * 0.44f, -headH * 0.020f, -width * 0.10f)),
                                            Vec3(width * 0.30f, width * 0.28f, width * 0.26f));
            nose = smax(nose, -notch - 0.003f, 0.008f);
        }
        /* Philtrum groove. */
        const float philtrum = sdCapsule(p, tip + Vec3(0, -headH * 0.022f, -width * 0.20f),
                                         mouthCentre_ + Vec3(0, headH * 0.020f, -headH * 0.005f), width * 0.16f);
        nose = smax(nose, -philtrum - 0.002f, 0.010f);
        d = smin(d, nose, 0.018f);
    }

    /* Lips: the shared vermilion envelope (see lipsShape) blended into the face. */
    d = smin(d, lipsShape(p), 0.012f);

    /* Eye sockets: subtract so an eyeball placed at eyeCentre_ sits inside a real orbit with
       a visible lid margin. The lids themselves are added back as thin crescents. */
    for (int s = 0; s < 2; ++s) {
        const float sx = (s == 0) ? 1.0f : -1.0f;
        const Vec3 e = eyeCentre_[s];
        const float r = m_.eyeRadius;
        const float socket = sdEllipsoid(p - (e + Vec3(0, 0, r * 0.30f)),
                                         Vec3(r * 1.55f, r * 1.35f, r * 1.25f));
        d = smax(d, -socket, 0.014f);
        /* Upper lid. */
        d = smin(d, sdEllipsoid(p - (e + Vec3(0, r * 0.62f, -r * 0.22f)),
                                Vec3(r * 1.42f, r * 0.72f, r * 0.95f)), 0.010f);
        /* Lower lid. */
        d = smin(d, sdEllipsoid(p - (e + Vec3(0, -r * 0.66f, -r * 0.20f)),
                                Vec3(r * 1.38f, r * 0.60f, r * 0.92f)), 0.010f);
        /* Inner canthus (epicanthic fold) and outer canthus. */
        d = smin(d, blob(p, e + Vec3(-sx * r * 1.28f, r * 0.10f, -r * 0.10f),
                         Vec3(r * 0.55f, r * 0.62f, r * 0.72f)), 0.008f);
        /* Eyebrow ridge mass - thin, wide, angled with regard to sex. */
        const float browLift = (m_.sex == AF_SEX_FEMALE) ? 1.15f : 1.0f;
        const Vec3 inner = {e.x - sx * r * 1.30f, e.y + r * 1.25f * browLift, e.z + r * 0.12f};
        const Vec3 outer = {e.x + sx * r * 1.70f, e.y + r * 1.05f * browLift, e.z - r * 0.10f};
        d = smin(d, sdTaperedCapsule(p, inner, outer, r * 0.30f, r * 0.20f), 0.014f);
    }

    /*
     * Eyeballs, added LAST: the socket carve above is subtractive, so an eyeball unioned before it
     * would be scooped out along with the orbit. Sclera + cornea sit inside the socket with the
     * lids overlapping the corneal bulge, which is what gives the renderer a refraction volume and
     * an eye that does not look painted on.
     */
    for (int side = 0; side < 2; ++side) {
        d = smin(d, eyeballShape(p, side), 0.003f);
    }

    /* Fine facial micro-relief. Amplitude is deliberately tiny (0.35 mm) - it exists to give
       the normal map something real to agree with, and to break up specular highlights. */
    {
        const float pores = noise_.cell(p, 190.0f) - 0.42f;
        const float fine = noise_.fbm(p * 60.0f, 3, 2.1f, 0.5f);
        const float amp = lerpf(0.00055f, 0.00022f, saturate((m_.age - 16.0f) / 54.0f));
        d -= (pores * 0.6f + fine * 0.45f) * amp;
    }
    return d;
}

/* =======================================================================================
 * Domain entry points
 * ======================================================================================= */
float BodyField::body(const Vec3& p) const {
    float d = torso(p);
    /*
     * Left and right limbs are unioned with a HARD min, never a smooth one. Two surfaces that
     * are merely close - the inner thighs, the two feet - would otherwise be bridged by a web:
     * smooth-min subtracts up to k/4 from the smaller distance, and with k comparable to the gap
     * the field turns negative in the empty space between them (measured: it produced a 6 mm
     * sheet of phantom geometry under the arches, which the floor-drop then read as the sole).
     * Same-side limb-to-torso blends keep a smooth union, where a real fillet is wanted and the
     * parts genuinely overlap.
     */
    const float dLeft = smin(smin(d, armChain(p, left_), 0.034f), legChain(p, left_), 0.030f);
    const float dRight = smin(smin(d, armChain(p, right_), 0.034f), legChain(p, right_), 0.030f);
    return minf(dLeft, dRight);
}

void BodyField::applyOralShapes(float& d, const Vec3& p) const {
    /*
     * The mouth, in three cuts, ordered so the results compose:
     *   1. the aperture, a slot through the front wall of the lips -> a real opening in the skin;
     *   2. the cavity behind it, big enough to hold the dental arches and the tongue;
     *   3. teeth and tongue, unioned back in as shells living inside that void.
     *
     * This runs on the FINAL union (the whole body: trunk, neck, head, limbs), not inside head().
     * A hole is a property of the finished solid, not of one of the parts that happen to be there:
     * when the mouth was carved in the head's field alone, the trunk's neck - which is a 5 cm tube
     * whose top sits just under the jaw - was smooth-unioned over the cavity afterwards and filled
     * it back in, leaving a face with lips and no mouth behind them.
     *
     * A hard-ish maximum (k = 1.5 mm) is right for the aperture: a soft one rounds the lip line
     * into a dimple and destroys the vermilion border the texture bake reads as lip material.
     */
    d = smax(d, -mouthAperture(p), 0.0015f);
    d = smax(d, -oralCavity(p), 0.008f);
    d = smin(d, teethShape(p), 0.003f);
    d = smin(d, tongueShape(p), 0.004f);
}

float BodyField::head(const Vec3& p) const {
    float d = faceFeatures(p);
    /*
     * The neck: a tapered tube from the shoulders up to just BENEATH the chin. Two constraints
     * fight here and the second one is easy to miss.
     *   - it has to cross the neck cut plane (1.45 m) as a single tube, so the trunk's and head's
     *     surfaces meet on a clean ring;
     *   - it must NOT reach as high as the mouth. The neck's radius is ~5-6 cm and its axis is
     *     close to the body's midline, while the oral cavity sits 40 mm behind the lips at almost
     *     the same height. A capsule whose top sat at the jaw line (the previous revision) filled
     *     the whole mouth, so the cavity carve, the teeth and the tongue were all buried inside
     *     solid material and every one of those shapes silently produced no surface at all.
     * Capping the tube 10 mm below the chin satisfies both: it still crosses the 1.45 m plane, and
     * it stays clear of the cavity, which bottoms out 20 mm above the chin.
     */
    const Vec3 neckBase(0.0f, m_.y.neckBaseY - 0.045f * m_.height, m_.neck.centreZ);
    const Vec3 neckTop(0.0f, m_.y.chinY - 0.010f, m_.neck.centreZ - 0.006f);
    d = smin(d, sdTaperedCapsule(p, neckBase, neckTop, m_.neck.rx * 1.22f, m_.neck.rx * 0.94f), 0.030f);
    return d;
}

float BodyField::hand(const Vec3& p, int side) const {
    return handShape(p, chain(side));
}

float BodyField::foot(const Vec3& p, int side) const {
    return footShape(p, chain(side));
}

/* =======================================================================================
 * Region classification
 * ======================================================================================= */
/* ==========================================================================================
 * Apparel.
 *
 * One rule generates every garment in the catalogue: a garment is the body's own distance field
 * inflated by a thickness and intersected with a domain. That is what lets the same code dress a
 * 1.62 m heavy build and a 1.86 m slim one identically well - the cloth follows the body because it
 * IS the body, offset.
 *
 * Three consequences worth stating, because they are what make this tractable:
 *   - a dressed avatar's skin under the cloth simply is not in the mesh (the inflated body
 *     contains the body, so the torso's surface is interior and the extractor never sees it),
 *     which is both correct and cheaper than modelling clothes over a full body;
 *   - the collar, hems, armholes and cuffs are the DOMAIN's boundaries, so a garment's outline is
 *     an intersection curve rather than modelled trim - crisp, and automatically correct;
 *   - the shells are solids, not membranes. A 4 mm membrane is thinner than the extraction lattice
 *     and would be sampled the way the mouth aperture was; a solid is robust at any lattice size.
 * ========================================================================================== */

float BodyField::armsCut(const Vec3& p, float thickness) const {
    /* The arms, inflated. Subtracted from a garment's band so a sleeveless garment has real
       armholes: the band covers the body's silhouette, and this punches the arms out of it. */
    const float t = thickness + 0.004f;
    return minf(armChain(p, left_) - t, armChain(p, right_) - t);
}

float BodyField::sleeveSolid(const Vec3& p, float thickness, float reach) const {
    /*
     * A sleeve: the inflated arm, limited to `reach` metres along the arm from the shoulder. The
     * limit is a sphere around the shoulder joint rather than a cut perpendicular to the arm's
     * axis, which is deliberate - a sleeve end is roughly perpendicular to the arm whatever the
     * pose, and a sphere keeps the boundary smooth and cheap. reach = 0 gives a shoulder strap.
     */
    float d = 1e9f;
    for (int side = -1; side <= 1; side += 2) {
        const Chain& c = chain(side);
        const float tube = armChain(p, chain(side)) - thickness;
        const float limit = (p - c.shoulder).length() - reach;
        d = minf(d, smax(tube, limit, 0.010f));
    }
    return d;
}

float BodyField::clothUpper(const Vec3& p) const {
    if (apparel_.outfit == 0) return 1e9f;
    const float H = m_.height;
    float thickness = 0.005f;        /* T-shirt                        */
    float hemY = m_.y.hipY - 0.02f * H;
    float topY = m_.y.shoulderY + 0.015f;
    float sleeveReach = 0.0f;
    float skirtHemY = 0.0f;          /* > 0 turns the upper piece into a dress */
    bool  gearPlates = false;
    switch (apparel_.outfit) {
        case 1:  /* T-shirt   */ thickness = 0.005f; sleeveReach = m_.upperArmLen * 0.35f; break;
        case 2:  /* Dress     */ thickness = 0.004f; hemY = m_.y.hipY - 0.02f * H;
                                 topY = m_.y.shoulderY + 0.020f; sleeveReach = 0.0f;
                                 skirtHemY = m_.y.hipY - m_.thighLen * 0.62f; break;
        case 3:  /* Suit      */ thickness = 0.011f; sleeveReach = m_.upperArmLen + m_.forearmLen * 0.92f;
                                 hemY = m_.y.hipY + 0.02f * H; break;
        case 4:  /* Jeans + jacket */ thickness = 0.014f; sleeveReach = m_.upperArmLen + m_.forearmLen * 0.80f;
                                 hemY = m_.y.waistY - 0.02f * H; break;
        case 5:  /* Plate armour */ thickness = 0.020f; sleeveReach = m_.upperArmLen * 0.55f;
                                 hemY = m_.y.hipY + 0.01f * H; gearPlates = true; break;
        case 6:  /* Leather armour */ thickness = 0.013f; sleeveReach = m_.upperArmLen + m_.forearmLen * 0.60f;
                                 hemY = m_.y.hipY; gearPlates = true; break;
        case 7:  /* Spacesuit */ thickness = 0.026f; sleeveReach = m_.upperArmLen + m_.forearmLen * 1.02f;
                                 hemY = m_.y.crotchY + 0.01f * H; gearPlates = true; break;
        case 8:  /* Athletic  */ thickness = 0.0035f; sleeveReach = m_.upperArmLen * 0.22f;
                                 hemY = m_.y.hipY - 0.03f * H; break;
        case 9:  /* Coat      */ thickness = 0.017f; sleeveReach = m_.upperArmLen + m_.forearmLen * 1.0f;
                                 hemY = m_.y.crotchY - m_.thighLen * 0.10f; break;
        default: return 1e9f;
    }
    (void)gearPlates;
    thickness = maxf(thickness, minLayer());

    /* The band: a super-elliptical column through the torso, from the hem to the collar. */
    const float y = clampf(p.y, hemY, topY);
    const float rx = torsoProfileRx(y) * 1.10f + thickness;
    const float rz = torsoProfileRz(y) * 1.10f + thickness;
    const float cz = torsoProfileCz(y);
    const float nx = p.x / rx;
    const float nz = (p.z - cz) / rz;
    const float e = 2.45f;
    const float kk = std::pow(std::pow(std::fabs(nx), e) + std::pow(std::fabs(nz), e), 1.0f / e);
    const float radial = (kk - 1.0f) * minf(rx, rz);
    float domain = smax(radial, maxf(hemY - p.y, p.y - topY), 0.010f);

    /* Arms out of the band, then the sleeves back in. */
    domain = smax(domain, -armsCut(p, thickness), 0.008f);
    domain = smin(domain, sleeveSolid(p, thickness, sleeveReach), 0.012f);

    /*
     * Dresses flare: below the hip the domain widens with depth, kept as a separate cone so the
     * upper body stays fitted. The skirt is closed at the bottom because it is a solid - the legs
     * inside it are hidden, which is exactly what a skirt does.
     */
    if (skirtHemY > 0.0f) {
        const float waistY = m_.y.hipY;
        const float kw = m_.waist.rx * 1.16f;
        const float flare = 1.0f + 1.35f * saturate((waistY - p.y) / maxf(1e-3f, waistY - skirtHemY));
        const float krx = kw * flare;
        const float krz = m_.waist.rz * 1.30f * flare;
        const float nkx = p.x / krx;
        const float nkz = p.z / krz;
        const float k2 = std::pow(std::pow(std::fabs(nkx), e) + std::pow(std::fabs(nkz), e), 1.0f / e);
        const float skirt = maxf((k2 - 1.0f) * minf(krx, krz), maxf(skirtHemY - p.y, p.y - waistY));
        domain = smin(domain, skirt, 0.012f);
    }

    /* The inflated body, clipped to the domain. Hard maximum: a hem is an edge, not a fillet. */
    return smax(body(p) - thickness, domain, 0.0018f);
}

float BodyField::clothLower(const Vec3& p) const {
    if (apparel_.outfit == 0) return 1e9f;
    const float H = m_.height;
    float thickness = 0.0f;
    float hemY = 0.0f;
    switch (apparel_.outfit) {
        case 2:  thickness = 0.004f; hemY = m_.y.hipY - m_.thighLen * 0.62f; break;  /* dress: tights */
        case 3:  thickness = 0.010f; hemY = m_.y.ankleY + 0.045f; break;             /* trousers      */
        case 4:  thickness = 0.014f; hemY = m_.y.ankleY + 0.030f; break;             /* jeans         */
        case 5:  thickness = 0.018f; hemY = m_.y.ankleY + 0.10f; break;              /* greaves       */
        case 6:  thickness = 0.013f; hemY = m_.y.ankleY + 0.08f; break;
        case 7:  thickness = 0.024f; hemY = m_.y.ankleY + 0.06f; break;
        case 8:  thickness = 0.0035f; hemY = m_.y.kneeY + m_.shankLen * 0.18f; break; /* shorts      */
        case 9:  thickness = 0.012f; hemY = m_.y.ankleY + 0.06f; break;
        default: return 1e9f;                                                        /* T-shirt: none */
    }
    thickness = maxf(thickness, minLayer());
    const float topY = m_.y.waistY + 0.030f;

    /* The legs band: hips and both legs, from the hem up past the waist. The gap between the legs
       is air in the body's field, so the crotch forms by itself - no explicit cut needed. */
    const float y = clampf(p.y, hemY, topY);
    const float rx = torsoProfileRx(y) * 1.12f + thickness;
    const float rz = torsoProfileRz(y) * 1.12f + thickness;
    const float cz = torsoProfileCz(y);
    const float nx = p.x / rx;
    const float nz = (p.z - cz) / rz;
    const float e = 2.45f;
    const float kk = std::pow(std::pow(std::fabs(nx), e) + std::pow(std::fabs(nz), e), 1.0f / e);
    float domain = smax((kk - 1.0f) * minf(rx, rz), maxf(hemY - p.y, p.y - topY), 0.012f);
    /* Arms hang beside the hips: keep them out of the trousers. */
    domain = smax(domain, -armsCut(p, thickness + 0.02f), 0.010f);
    (void)H;
    return smax(body(p) - thickness, domain, 0.0018f);
}

float BodyField::shoeShape(const Vec3& p) const {
    if (apparel_.footwear == 0) return 1e9f;
    float thickness = 0.006f;
    float topY = m_.y.ankleY + 0.055f;     /* shoe height above the ankle */
    float soleDrop = 0.0f;                 /* heel: how far the sole hangs below the foot */
    switch (apparel_.footwear) {
        case 1: thickness = 0.007f; topY = m_.y.ankleY + 0.045f; break;   /* sneaker */
        case 2: thickness = 0.011f; topY = m_.y.kneeY - m_.shankLen * 0.30f; break; /* boot */
        case 3: thickness = 0.016f; topY = m_.y.ankleY + 0.075f; break;   /* sabaton */
        case 4: thickness = 0.004f; topY = m_.y.ankleY + 0.020f; soleDrop = 0.075f; break; /* heel */
        default: return 1e9f;
    }
    thickness = maxf(thickness, minLayer());
    float d = 1e9f;
    for (int side = -1; side <= 1; side += 2) {
        const Chain& c = chain(side);
        const Chain& c_inner = c;   /* the heel's arch is built from the same foot chain */
        /*
         * The shoe is the inflated foot PLUS a wrap around the ankle, so the opening is a ring
         * around the leg rather than a hole in the top of the foot. The vertical clip at topY then
         * cuts the wrap into the shoe's collar.
         */
        const float footSolid = foot(p, side) - thickness;
        const float wrap = (p - Vec3(c.ankle.x, c.ankle.y - m_.footHeight * 0.35f, c.ankle.z)).length() -
                           (m_.ankleRx + thickness + 0.006f);
        float solid = minf(footSolid, wrap);
        /* A heel is a wedge under the back of the foot, not a raised sole: keep it behind the ball. */
        if (soleDrop > 0.0f) {
            const float back = smax(solid, p.z - (c.ball.z + 0.030f), 0.004f);
            const float spike = sdRoundBox(p - Vec3(c.heel.x, m_.y.soleY + soleDrop * 0.5f, c.heel.z + 0.010f),
                                           Vec3(m_.footWidth * 0.16f, soleDrop * 0.5f, m_.footWidth * 0.16f), 0.004f);
            solid = smin(solid, spike, 0.006f);
            (void)back;
        }
        /*
         * The shoe has to stand ON the floor. The foot's sole is already at the field's ground
         * level, so inflating it by the sole's thickness would push the sole through y = 0 and the
         * avatar would sink into its own floor plane. Clipping at y = 0 flattens the sole exactly
         * where the ground is, which is both correct and what the grounding contract requires.
         */
        solid = smax(solid, -p.y, 0.0025f);
        if (soleDrop > 0.0f) {
            /* Heel: raise the arch. The toe pad and the heel block keep the floor, the midfoot is
               carved away from underneath - the silhouette of a heel without moving the foot. */
            const Vec3 arc(0.0f, 0.0f, (c_inner.ball.z + c_inner.heel.z) * 0.5f);
            const float halfZ = std::fabs(c_inner.ball.z - c_inner.heel.z) * 0.42f;
            float voidArch = sdEllipsoid(p - Vec3(0.0f, soleDrop * 0.55f, arc.z),
                                         Vec3(m_.footWidth * 1.2f, soleDrop * 0.55f, halfZ));
            solid = smax(solid, -voidArch, 0.003f);
        }
        solid = smax(solid, p.y - topY, 0.006f);
        d = minf(d, solid);
    }
    return d;
}

float BodyField::gearShape(const Vec3& p) const {
    /*
     * Equipment: the parts of a kit that are not cloth. These are the pieces the second image of a
     * two-reference generation transfers - a pauldron, a chest plate, a belt, a backpack - and they
     * carry the GEAR material, which is metallic and much rougher than cloth.
     */
    if (apparel_.outfit != 5 && apparel_.outfit != 6 && apparel_.outfit != 7) return 1e9f;
    const float H = m_.height;
    const bool plate = (apparel_.outfit == 5);
    const bool suit = (apparel_.outfit == 7);
    float d = 1e9f;

    /* Pauldrons: domes over the deltoids, dropping over the upper arm. */
    for (int side = -1; side <= 1; side += 2) {
        const Chain& c = chain(side);
        const Vec3 s = c.shoulder;
        const float r = m_.deltoidR * (suit ? 1.65f : 1.35f);
        float pad = sdEllipsoid(p - Vec3(s.x + side * r * 0.18f, s.y + r * 0.16f, s.z), Vec3(r, r * 0.72f, r * 0.92f));
        pad = smax(pad, s.y + r * 0.10f - p.y, 0.008f);           /* open underneath */
        d = minf(d, pad);
    }
    /* Chest plate: a shell over the sternum, following the ribcage. */
    {
        const float r = m_.chest.rx * (suit ? 1.10f : 0.92f);
        const Vec3 cc(0.0f, m_.y.bustY + 0.02f * H, -(m_.chest.rz * 0.55f));
        float cp = sdEllipsoid(p - cc, Vec3(r * 0.92f, r * 0.78f, r * 0.95f));
        cp = smax(cp, -(p.y - (m_.y.underbustY - 0.02f * H)), 0.010f);      /* not below the ribs */
        d = minf(d, cp);
    }
    /* Belt / harness ring at the waist. */
    {
        const float rr = m_.waist.rx * 1.26f;
        const float dy = p.y - (m_.y.waistY + 0.005f);
        const float dz = (p.z - m_.waist.centreZ);
        const float ring = std::sqrt(dy * dy + dz * dz) - 0.016f;
        const float radial = std::sqrt(p.x * p.x * 0.92f + dz * dz * 0.35f) - rr;
        d = minf(d, smax(ring, std::fabs(radial) - 0.012f, 0.006f));
    }
    if (plate) {
        /* Thigh tassets and a plackart ledge. */
        for (int side = -1; side <= 1; side += 2) {
            const Vec3 hj = chain(side).hipJoint;
            const float r = m_.thighRx * 1.22f;
            float t = sdEllipsoid(p - Vec3(hj.x, hj.y - m_.thighLen * 0.12f, hj.z - m_.thighRz * 0.35f),
                                  Vec3(r * 0.95f, r * 0.85f, r * 0.80f));
            t = smax(t, (hj.y - m_.thighLen * 0.34f) - p.y, 0.010f);
            d = minf(d, t);
        }
    }
    if (suit) {
        /* Life support pack: a box behind the shoulders. */
        const float ry = m_.chest.rx * 0.52f;
        const Vec3 bc(0.0f, m_.y.bustY + 0.03f * H, m_.chest.rz * 0.62f);
        d = minf(d, sdRoundBox(p - bc, Vec3(ry, ry * 1.15f, ry * 0.70f), 0.030f));
    }
    return d;
}

float BodyField::hairShape(const Vec3& p) const {
    if (apparel_.hair == 0) return 1e9f;
    const float headH = m_.y.crownY - m_.y.chinY;
    const float crown = m_.y.crownY;
    float thickness = 0.008f;
    float length = 0.0f;        /* how far below the crown the mass hangs */
    float volume = 0.0f;        /* extra bulk around the skull            */
    float crest = 0.0f;         /* mohawk: restrict to a midline strip      */
    float bun = 0.0f, tail = 0.0f, braids = 0.0f;
    switch (apparel_.hair) {
        case 1: thickness = 0.006f; break;                                  /* buzz       */
        case 2: thickness = 0.013f; volume = 0.010f; break;                  /* short      */
        case 3: thickness = 0.016f; volume = 0.018f; length = headH * 0.55f; break;
        case 4: thickness = 0.018f; volume = 0.024f; length = headH * 2.6f; break;   /* long */
        case 5: thickness = 0.014f; volume = 0.012f; tail = 0.30f; break;    /* ponytail   */
        case 6: thickness = 0.014f; volume = 0.014f; bun = 0.055f; break;    /* bun        */
        case 7: thickness = 0.014f; volume = 0.012f; braids = 0.36f; break;  /* braids     */
        case 8: thickness = 0.030f; volume = 0.055f; break;                  /* afro       */
        case 9: thickness = 0.026f; volume = 0.006f; crest = 0.030f; break;  /* mohawk     */
        default: return 1e9f;
    }

    thickness = maxf(thickness, minLayer() * 0.75f);   /* hair may be a little thinner than cloth */
    /* The mass: the inflated skull. */
    float d = head(p) - thickness;

    /*
     * The hairline: a plane tilted back from the brow. Everything in FRONT of it - the forehead,
     * the eyes, the whole face - is cut away, so the hair sits where hair sits and the face stays
     * clear. The tilt follows the brow ridge, which is already an anatomical anchor.
     */
    const float tilt = 0.30f + 0.20f * (m_.sex == AF_SEX_FEMALE ? 1.0f : 0.0f);
    const Vec3 hp(0.0f, browRidge_.y + headH * 0.05f, browRidge_.z + m_.head.rz * 0.10f);
    const float front = (p - hp).x * 0.0f + (p - hp).y * std::cos(tilt) - (p - hp).z * std::sin(tilt);
    d = smax(d, -front, 0.004f);
    /* Below the ears the head is bare; the mass must not wrap the jaw. */
    const float jawCut = eyeCentre_[0].y - headH * 0.14f - p.y;
    if (length <= 0.0f) d = smax(d, jawCut, 0.006f);

    /* Bulk: a volume that follows the skull outwards, for volume without thickness. */
    if (volume > 0.0f) {
        const Vec3 c(0.0f, (crown + m_.y.eyeY) * 0.5f, m_.head.centreZ + m_.head.rz * 0.12f);
        const float v = sdEllipsoid(p - c, Vec3(m_.head.rx * 1.02f + volume, headH * 0.52f + volume,
                                               m_.head.rz * 1.02f + volume * 1.2f));
        d = smin(d, smax(v, -front, 0.004f), 0.010f);
    }
    /* A crest keeps only a strip along the midline; used by the mohawk. */
    if (crest > 0.0f) d = smax(d, std::fabs(p.x) - crest, 0.008f);

    /* Styles that hang: a curtain of hair at the back, widening as it falls. */
    if (length > 0.0f) {
        const float topY = crown - headH * 0.05f;
        const float botY = crown - length;
        const float t = saturate((topY - p.y) / maxf(1e-3f, topY - botY));
        const float backZ = m_.head.centreZ + m_.head.rz * (0.35f + 0.55f * t);
        const float halfW = m_.head.rx * (0.95f + 0.45f * t);
        /* Only behind the head, and only from the crown down. */
        const float curtain = maxf(maxf(std::fabs(p.x) - halfW, backZ - p.z),
                                   maxf(p.y - topY, botY - p.y));
        d = smin(d, smax(curtain, -(0.0f), 0.001f), 0.012f);
    }
    /* Ponytail: a tapered rope from the crown, sweeping down and back. */
    if (tail > 0.0f) {
        const Vec3 a(0.0f, crown - headH * 0.12f, m_.head.centreZ + m_.head.rz * 1.02f);
        const Vec3 b(0.0f, crown - tail, m_.head.centreZ + m_.head.rz * 1.45f);
        d = smin(d, sdTaperedCapsule(p, a, b, m_.head.rx * 0.34f, m_.head.rx * 0.10f), 0.010f);
    }
    /* Bun: a flattened ball at the back of the crown. */
    if (bun > 0.0f) {
        const Vec3 c(0.0f, crown - headH * 0.16f, m_.head.centreZ + m_.head.rz * 0.98f);
        d = smin(d, sdEllipsoid(p - c, Vec3(bun * 0.95f, bun * 0.80f, bun * 0.70f)), 0.010f);
    }
    /* Braids: two ropes falling in front of the shoulders. */
    if (braids > 0.0f) {
        for (int side = -1; side <= 1; side += 2) {
            const Vec3 a(side * m_.head.rx * 0.85f, crown - headH * 0.35f, m_.head.centreZ + m_.head.rz * 0.55f);
            const Vec3 b(side * m_.head.rx * 1.15f, crown - braids, m_.head.centreZ + m_.head.rz * 0.30f);
            d = smin(d, sdTaperedCapsule(p, a, b, m_.head.rx * 0.20f, m_.head.rx * 0.13f), 0.008f);
        }
    }
    return d;
}

float BodyField::apparelField(const Vec3& p) const {
    if (!hasApparel()) return 1e9f;
    float d = minf(clothUpper(p), clothLower(p));
    d = minf(d, shoeShape(p));
    d = minf(d, gearShape(p));
    d = minf(d, hairShape(p));
    return d;
}

bool BodyField::boundsApparel(Vec3* lo, Vec3* hi) const {
    if (!hasApparel()) return false;
    /*
     * The apparel's extent, stated generously rather than derived: a coat hem, a long ponytail and
     * a boot all add a few centimetres to the body, and the only cost of a generous bound is a
     * slightly larger grid. The alternative - clipping a hem because the grid ended - is a
     * silhouette defect that no amount of downstream care can fix.
     */
    Vec3 blo, bhi, tlo, thi;
    boundsBody(&blo, &bhi);
    boundsHead(&tlo, &thi);
    blo = blo.minv(tlo); bhi = bhi.maxv(thi);
    for (int side = -1; side <= 1; side += 2) {
        boundsHand(side, &tlo, &thi);
        blo = blo.minv(tlo); bhi = bhi.maxv(thi);
        boundsFoot(side, &tlo, &thi);
        blo = blo.minv(tlo); bhi = bhi.maxv(thi);
    }
    const float padXZ = 0.16f;
    const float padY = 0.10f;
    *lo = Vec3(blo.x - padXZ, blo.y - padY, blo.z - padXZ);
    *hi = Vec3(bhi.x + padXZ, bhi.y + padY, bhi.z + padXZ);
    return true;
}

int BodyField::classifyRegion(const Vec3& p) const {
    /*
     * Material selection, in priority order. Each test asks the SAME shape function that built the
     * geometry, so a region label can never disagree with the surface it labels - the previous
     * revision re-derived the eyes from a radius test that disagreed with the field by a factor of
     * two at the socket rim, and the lips were missed entirely except for a single vertex.
     */
    /*
     * The tolerance on each test is not arbitrary: mesh vertices are projected onto the isosurface
     * to within about 0.2 mm but are not exactly on it, so a strict `d < 0` test classifies a
     * surface vertex as skin about half the time and the feature loses its material entirely (the
     * previous revision produced ZERO eye vertices that way). 1.5 mm is larger than the projection
     * error and smaller than the distance from these shapes to the surrounding skin.
     */
    const float kOnSurface = 0.0015f;
    /*
     * Apparel is tested first. Cloth, shoes, gear and hair all sit outside the skin, so a point on
     * one of their surfaces would otherwise be labelled as skin - the region labels drive the
     * material assignment in the texture bake, and a shirt baked as skin is exactly the kind of
     * silent failure this ordering prevents. Precedence runs outermost layer first: a pauldron
     * covers a jacket sleeve, and a boot covers a trouser hem.
     */
    if (apparel_.footwear != 0 && shoeShape(p) < kOnSurface) return REGION_SHOE;
    if (apparel_.hair != 0 && hairShape(p) < kOnSurface) return REGION_HAIR;
    if ((apparel_.outfit == 5 || apparel_.outfit == 6 || apparel_.outfit == 7) &&
        gearShape(p) < kOnSurface) return REGION_GEAR;
    if (apparel_.outfit != 0 && clothUpper(p) < kOnSurface) return REGION_CLOTH_UPPER;
    if (apparel_.outfit != 0 && clothLower(p) < kOnSurface) return REGION_CLOTH_LOWER;

    if (teethShape(p) < kOnSurface) return REGION_TEETH;
    if (tongueShape(p) < kOnSurface) return REGION_TONGUE;
    if (lipsShape(p) < kOnSurface) return REGION_LIPS;
    if (browShape(p) < kOnSurface) return REGION_LASH;
    for (int side = 0; side < 2; ++side) {
        if (eyeballShape(p, side) < kOnSurface) return (side == 0) ? REGION_EYE_L : REGION_EYE_R;
    }
    /* Nails: dorsal side of each distal phalanx. */
    for (int side = -1; side <= 1; side += 2) {
        const Chain& c = chain(side);
        const Vec3 palmNormal(-static_cast<float>(side), 0.0f, 0.0f);
        for (int i = 0; i < 5; ++i) {
            const float r = m_.fingerRadius[i];
            const Vec3 tip = c.fingerTip[i];
            if ((p - tip).length() < r * 1.6f && (p - tip).dot(palmNormal) > r * 0.25f) return REGION_NAIL;
        }
        for (int i = 0; i < 5; ++i) {
            const float r = m_.toeRadius[i];
            const Vec3 tip = c.ball + Vec3(0, 0, -(m_.toeLength[i] + r));
            if ((p - tip).length() < r * 1.8f && (p.y - tip.y) > 0.0f) return REGION_NAIL;
        }
    }
    return REGION_SKIN;
}

/* =======================================================================================
 * Bounds
 * ======================================================================================= */
/*
 * Bounds philosophy: a domain is extracted on a dense grid, so its bounding box MUST fully
 * contain the surface. If it does not, the mesh is clipped by the grid wall and the extractor
 * legitimately reports boundary loops there - which the stitcher then cannot match against
 * the intended cut plane, leaving holes. The margins below are therefore derived from the
 * largest soft-tissue bulge and the widest rounding radius in each domain, not guessed.
 */
void BodyField::boundsBody(Vec3* mn, Vec3* mx) const {
    const float H = m_.height;
    const float armReach = m_.biacromialHalf + m_.deltoidR * 1.6f + m_.upperArmR * 1.5f;
    const float footExtent = m_.footLength + m_.footWidth;
    const float margin = 0.030f + 0.02f * H;
    mn->x = -armReach - margin;
    mx->x = armReach + margin;
    mn->y = -0.010f;
    mx->y = neckCutY();
    mn->z = -maxf(footExtent, m_.bustSeg.rz * 1.9f + m_.gluteBulge) - margin;
    mx->z = maxf(m_.footLength * 0.8f, m_.hip.rz * 1.8f + m_.gluteBulge) + margin;
}

void BodyField::boundsHead(Vec3* mn, Vec3* mx) const {
    /* Ears add ~0.6 head radii laterally, the nose ~1.35 forward, the occiput ~1.2 back. */
    const float pad = 0.030f;
    mn->x = -m_.head.rx * 1.75f - pad;
    mx->x = m_.head.rx * 1.75f + pad;
    mn->y = neckCutY() - 0.005f;
    mx->y = m_.y.crownY + 0.006f;
    mn->z = -m_.head.rz * 1.55f - pad;
    mx->z = m_.head.rz * 1.35f + pad;
}

void BodyField::boundsHand(int side, Vec3* mn, Vec3* mx) const {
    const Chain& c = chain(side);
    const float pad = 0.020f;
    const float widest = m_.fingerRadius[0] * 1.35f + m_.palmThickness * 0.5f;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    auto include = [&](const Vec3& v, float r) {
        lo = lo.minv(v - Vec3(r));
        hi = hi.maxv(v + Vec3(r));
    };
    include(c.wrist, m_.wristR * 1.2f);
    include(c.palmCentre, m_.palmWidth * 0.75f);
    for (int i = 0; i < 5; ++i) {
        include(c.fingerTip[i], m_.fingerRadius[i] * 1.6f);
        include(c.fingerKnuckle[i], m_.fingerRadius[i] * 1.6f);
        include(c.fingerJoints[i][0], widest);
        include(c.fingerJoints[i][1], widest);
    }
    *mn = lo - Vec3(pad);
    *mx = hi + Vec3(pad);
    /* The domain must straddle its cut plane so the extracted mesh has a ring exactly there. */
    mx->y = maxf(mx->y, wristCutY(side) + 0.006f);
    mn->y = minf(mn->y, wristCutY(side) - 0.05f);
}

void BodyField::boundsFoot(int side, Vec3* mn, Vec3* mx) const {
    const Chain& c = chain(side);
    const float pad = 0.020f;
    mn->x = c.ankle.x - (m_.footWidth * 1.10f + m_.ankleRx * 1.5f) - pad;
    mx->x = c.ankle.x + (m_.footWidth * 1.10f + m_.ankleRx * 1.5f) + pad;
    /* The sole sits a fixed gap above the ground plane; nothing may poke below it. */
    mn->y = m_.y.soleY - 0.008f;
    mx->y = ankleCutY(side) + 0.006f;
    mn->z = c.heel.z - m_.footLength * 0.30f - pad;
    mx->z = c.ankle.z + m_.footLength * 0.30f + pad;
}

} // namespace af
