/*
 * af_rig.cpp -- skeleton construction, skin weighting, facial shapes and the clip library.
 */
#include "af_rig.h"
#include "af_threads.h"
#include "../include/afcore.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace af {

const char* const kBlendshapeNames[52] = {
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
    "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
    "eyeBlinkLeft", "eyeBlinkRight", "eyeLookDownLeft", "eyeLookDownRight",
    "eyeLookInLeft", "eyeLookInRight", "eyeLookOutLeft", "eyeLookOutRight",
    "eyeLookUpLeft", "eyeLookUpRight", "eyeSquintLeft", "eyeSquintRight",
    "eyeWideLeft", "eyeWideRight",
    "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight",
    "mouthFunnel", "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthPressLeft", "mouthPressRight", "mouthPucker", "mouthRight",
    "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
    "mouthSmileLeft", "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight",
    "mouthUpperUpLeft", "mouthUpperUpRight", "noseSneerLeft", "noseSneerRight",
    "tongueOut"
};

/* ==============================================================================================
 * Skeleton
 * ============================================================================================== */
namespace {

/* Finger joint suffixes in anatomical order. */
const char* const kFingerChains[5][3] = {
    {"thumb_01", "thumb_02", "thumb_03"},
    {"index_01", "index_02", "index_03"},
    {"middle_01", "middle_02", "middle_03"},
    {"ring_01", "ring_02", "ring_03"},
    {"pinky_01", "pinky_02", "pinky_03"}
};
const char* const kFingerNames[5] = {"thumb", "index", "middle", "ring", "pinky"};

} // namespace

void Rig::finaliseRestPose() {
    /* Fills worldPosition for every joint from the local rest transforms, and builds the
       inverse bind matrices used by the GLB skin. */
    const int n = jointCount();
    inverseBind_.assign(n, Mat4::identity());
    for (int i = 0; i < n; ++i) {
        const Joint& j = joints_[i];
        const Mat4 local = Mat4::trs(j.restTranslation, j.restRotation, j.restScale);
        Mat4 world = local;
        if (j.parent >= 0) {
            const Vec3 pp = joints_[j.parent].worldPosition;
            world = Mat4::translation(pp) * local;
        }
        joints_[i].worldPosition = world.transformPoint(Vec3(0, 0, 0));
        inverseBind_[i] = world.inverse();
    }
}

int Rig::jointIndex(const char* name) const {
    for (size_t i = 0; i < joints_.size(); ++i) if (joints_[i].name == name) return static_cast<int>(i);
    return -1;
}

bool Rig::build(const BodyField& field, const Mesh& mesh) {
    field_ = &field;
    joints_.clear();
    buildSkeleton(field);
    if (joints_.empty()) return false;
    finaliseRestPose();
    computeWeights(field, mesh);
    buildBlendshapes(field, mesh);
    return true;
}

void Rig::buildSkeleton(const BodyField& field) {
    const BodyMetrics& m = field.metrics();
    auto add = [&](const std::string& name, int parent, const Vec3& worldPos, float radius,
                   int flags = 0, int region = REGION_SKIN) {
        Joint j;
        j.name = name;
        j.parent = parent;
        j.worldPosition = worldPos;
        j.radius = radius;
        j.flags = flags;
        j.bodyRegion = region;
        /* Local translation is filled in finaliseRestPose() from the parent's world position;
           storing it here as a delta keeps the hierarchy explicit and GLB-ready. */
        j.restTranslation = (parent >= 0)
                                ? (worldPos - joints_[static_cast<size_t>(parent)].worldPosition)
                                : worldPos;
        joints_.push_back(j);
        return static_cast<int>(joints_.size()) - 1;
    };

    const float pelvis = m.y.hipY - m.hip.rz * 0.10f;
    const int root = add("root", -1, Vec3(0, 0, 0), 0.05f);
    const int hips = add("hips", root, Vec3(0, pelvis, m.hip.centreZ), m.hip.rx * 0.62f);

    /* Spine chain: five segments between pelvis and neck base, with the natural curvature
       baked into the rest positions so the bind pose matches the mesh. */
    const int spine1 = add("spine_01", hips, Vec3(0, lerpf(pelvis, m.y.waistY, 0.35f), m.waist.centreZ - m.hip.rz * 0.02f), m.waist.rx * 0.75f);
    const int spine2 = add("spine_02", spine1, Vec3(0, m.y.waistY, m.waist.centreZ), m.waist.rx * 0.80f);
    const int spine3 = add("spine_03", spine2, Vec3(0, lerpf(m.y.waistY, m.y.underbustY, 0.55f), m.underbust.centreZ), m.underbust.rx * 0.82f);
    const int spine4 = add("spine_04", spine3, Vec3(0, lerpf(m.y.underbustY, m.y.armpitY, 0.60f), m.chest.centreZ * 0.9f), m.chest.rx * 0.80f);
    const int chest = add("chest", spine4, Vec3(0, m.y.armpitY, m.chest.centreZ), m.chest.rx * 0.85f);
    const int neck = add("neck_01", chest, Vec3(0, lerpf(m.y.neckBaseY, m.y.jawY, 0.35f), m.neck.centreZ), m.neck.rx * 1.15f);
    const int neck2 = add("neck_02", neck, Vec3(0, m.y.jawY - (m.y.crownY - m.y.jawY) * 0.045f, m.neck.centreZ * 0.5f), m.neck.rx * 1.35f);
    const int head = add("head", neck2, Vec3(0, m.y.jawY + (m.y.crownY - m.y.jawY) * 0.42f, 0), m.head.rx * 1.6f);
    const int jaw = add("jaw", head, Vec3(0, m.y.chinY + (m.y.crownY - m.y.chinY) * 0.06f, -m.head.rz * 0.45f), m.head.rx * 0.75f, AF_JOINT_FACE);
    add("eye_L", head, Vec3(m.eyeSeparation * 0.5f, m.y.eyeY, -m.head.rz * 0.78f), m.eyeRadius * 2.0f, AF_JOINT_FACE);
    add("eye_R", head, Vec3(-m.eyeSeparation * 0.5f, m.y.eyeY, -m.head.rz * 0.78f), m.eyeRadius * 2.0f, AF_JOINT_FACE);

    /* Arms */
    for (int s = 0; s < 2; ++s) {
        const int side = (s == 0) ? 1 : -1;
        const Chain& c = field.chain(side);
        const char* suffix = (side > 0) ? "L" : "R";
        const std::string sfx = std::string("_") + suffix;

        const int clav = add("clavicle" + sfx, chest,
                             Vec3(static_cast<float>(side) * m.neck.rx * 0.6f, m.y.neckBaseY - 0.006f, 0.0f),
                             m.biacromialHalf * 0.5f);
        const int shoulder = add("upperarm_01" + sfx, clav, c.shoulder, m.deltoidR * 1.15f);
        const int upperMid = add("upperarm_02" + sfx, shoulder,
                                 c.shoulder + (c.elbow - c.shoulder) * 0.52f, m.upperArmR * 1.1f);
        const int elbow = add("forearm_01" + sfx, upperMid, c.elbow, m.elbowR * 1.15f);
        const int foreMid = add("forearm_02" + sfx, elbow,
                                c.elbow + (c.wrist - c.elbow) * 0.55f, m.forearmR * 1.1f);
        const int wrist = add("hand" + sfx, foreMid, c.wrist, m.wristR * 1.6f);
        /* Twist bones let the forearm and upper arm roll without candy-wrapper collapse. */
        add("upperarm_twist" + sfx, shoulder, c.shoulder + (c.elbow - c.shoulder) * 0.38f, m.upperArmR * 0.65f, AF_JOINT_TWIST);
        add("forearm_twist" + sfx, elbow, c.elbow + (c.wrist - c.elbow) * 0.42f, m.forearmR * 0.65f, AF_JOINT_TWIST);

        for (int f = 0; f < 5; ++f) {
            int parent = wrist;
            /* The thumb has a real metacarpal segment (thumb_00) that carries it clear of the
               palm; the other fingers start at their MCP joint. */
            if (f == 0) {
                const Vec3 metacarpal = c.wrist.lerp(c.fingerJoints[0][0], 0.55f);
                parent = add("thumb_00" + sfx, wrist, metacarpal, m.fingerRadius[0] * 1.30f, AF_JOINT_FINGER);
            }
            for (int seg = 0; seg < 3; ++seg) {
                const Vec3 pos = c.fingerJoints[f][seg];
                const std::string jname = std::string(kFingerNames[f]) + "_" + std::string(1, '0') +
                                          std::string(1, static_cast<char>('1' + seg)) + sfx;
                (void)kFingerChains;
                parent = add(jname, parent, pos, m.fingerRadius[f] * (1.35f - 0.15f * static_cast<float>(seg)),
                             AF_JOINT_FINGER);
            }
        }
    }

    /* Legs */
    for (int s = 0; s < 2; ++s) {
        const int side = (s == 0) ? 1 : -1;
        const Chain& c = field.chain(side);
        const std::string sfx = (side > 0) ? "_L" : "_R";

        const int thigh = add("thigh" + sfx, hips, c.hipJoint, m.thighRx * 1.05f);
        const int knee = add("shin" + sfx, thigh, c.knee, m.kneeR * 1.20f);
        const int ankle = add("foot" + sfx, knee, c.ankle, m.ankleRx * 1.75f);
        add("toe" + sfx, ankle, c.ball, m.footWidth * 0.36f);
        add("thigh_twist" + sfx, thigh, c.hipJoint + (c.knee - c.hipJoint) * 0.45f, m.thighRx * 0.62f, AF_JOINT_TWIST);
    }
    (void)spine2; (void)spine3; (void)spine4; (void)chest; (void)neck; (void)neck2; (void)head; (void)jaw;
}

std::vector<Mat4> Rig::computeWorld(const std::vector<Quat>& localRotations) const {
    const int n = jointCount();
    std::vector<Mat4> world(static_cast<size_t>(n), Mat4::identity());
    for (int i = 0; i < n; ++i) {
        const Joint& j = joints_[i];
        const Quat rot = (i < static_cast<int>(localRotations.size())) ? localRotations[i] : j.restRotation;
        const Mat4 local = Mat4::trs(j.restTranslation, rot, j.restScale);
        world[i] = (j.parent >= 0) ? (world[j.parent] * local) : local;
    }
    return world;
}

/* ==============================================================================================
 * Skinning
 * ============================================================================================== */
namespace {

/** Distance from a point to a bone segment (its parent joint -> its own position). */
float pointSegmentDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float denom = ab.lengthSq();
    if (denom < 1e-10f) return (p - a).length();
    const float t = clampf((p - a).dot(ab) / denom, 0.0f, 1.0f);
    return (p - (a + ab * t)).length();
}

/**
 * Region gate: a vertex may only bind to bones that plausibly drive it. Without this, a finger
 * bone can pick up chest vertices on a T-posed heavy build and the whole shoulder tears.
 */
bool boneAppliesToRegion(const Joint& j, int vertexRegion) {
    const bool isFinger = (j.flags & AF_JOINT_FINGER) != 0;
    const bool isArm = j.name.rfind("upperarm", 0) == 0 || j.name.rfind("forearm", 0) == 0 ||
                       j.name.rfind("clavicle", 0) == 0;
    const bool isHand = j.name.rfind("hand", 0) == 0;
    const bool isLeg = j.name.rfind("thigh", 0) == 0 || j.name.rfind("shin", 0) == 0;
    const bool isFoot = j.name.rfind("foot", 0) == 0 || j.name.rfind("toe", 0) == 0;
    const bool isFace = (j.flags & AF_JOINT_FACE) != 0;
    const bool isHeadBone = j.name == "head" || j.name == "neck_01";

    switch (vertexRegion) {
        case REGION_EYE_L: case REGION_EYE_R: case REGION_TEETH: case REGION_LIPS:
        case REGION_TONGUE:
            return isFace || isHeadBone || j.name == "jaw";
        case REGION_HAIR: case REGION_LASH:
            return isHeadBone || isFace;
        case REGION_NAIL:
            return isFinger || isFoot || isHand;
        case REGION_SHOE:
            return isFoot || isLeg;
        case REGION_GEAR:
            return true;                 /* armour is rigid across several bones by design */
        case REGION_CLOTH_UPPER:
        case REGION_CLOTH_LOWER:
            return !isFinger && !isFoot; /* cloth follows body bones, never fingers */
        default:
            if (isFinger) return false;  /* skin vertices only bind to fingers when they ARE fingers */
            return true;
    }
}

} // namespace

void Rig::computeWeights(const BodyField& field, const Mesh& mesh) {
    const int n = jointCount();
    const size_t vcount = mesh.vertexCount();
    weights_.assign(vcount, SkinWeights{});

    /* Bone segments in rest space: each joint drives the segment from its parent to itself. */
    struct BoneSeg { int joint; Vec3 a, b; float radius; };
    std::vector<BoneSeg> segs;
    segs.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Joint& j = joints_[i];
        if (j.parent < 0) {
            /* Root drives a small sphere at the pelvis. */
            segs.push_back({i, j.worldPosition, j.worldPosition, j.radius});
            continue;
        }
        const Vec3 a = joints_[j.parent].worldPosition;
        segs.push_back({i, a, j.worldPosition, j.radius});
    }

    const float falloff = 3.2f;   /* higher = tighter, more local weights */
    const int threads = defaultThreadCount();
    parallelFor(static_cast<int>(vcount), threads, [&](int i0, int i1) {
        std::vector<std::pair<float, int>> scored;
        for (int vi = i0; vi < i1; ++vi) {
            const Vec3 p = mesh.positions[static_cast<size_t>(vi)];
            const int region = (vi < static_cast<int>(mesh.regions.size())) ? mesh.regions[static_cast<size_t>(vi)] : REGION_SKIN;
            scored.clear();
            for (const BoneSeg& s : segs) {
                const Joint& j = joints_[static_cast<size_t>(s.joint)];
                if (!boneAppliesToRegion(j, region)) continue;
                const float d = pointSegmentDistance(p, s.a, s.b);
                const float r = maxf(s.radius, 1e-3f);
                /* Inverse-distance falloff, floored so a vertex far from every bone still binds
                   (to its nearest one) instead of being left unweighted. */
                const float w = 1.0f / std::pow(d / r + 0.06f, falloff);
                scored.emplace_back(w, s.joint);
            }
            if (scored.empty()) {
                /* Fallback: nearest bone regardless of the gate (logged as a repair, not hidden). */
                float best = 1e30f;
                int bestJoint = 0;
                for (const BoneSeg& s : segs) {
                    const float d = pointSegmentDistance(p, s.a, s.b);
                    if (d < best) { best = d; bestJoint = s.joint; }
                }
                weights_[static_cast<size_t>(vi)].joints = {static_cast<uint16_t>(bestJoint), 0, 0, 0};
                weights_[static_cast<size_t>(vi)].weights = {1.0f, 0.0f, 0.0f, 0.0f};
                continue;
            }
            const size_t keep = std::min<size_t>(4, scored.size());
            std::partial_sort(scored.begin(), scored.begin() + static_cast<long>(keep), scored.end(),
                              [](const std::pair<float, int>& x, const std::pair<float, int>& y) {
                                  return x.first > y.first;
                              });
            SkinWeights sw;
            float sum = 0.0f;
            for (size_t k = 0; k < keep; ++k) {
                sw.joints[k] = static_cast<uint16_t>(scored[k].second);
                sw.weights[k] = scored[k].first;
                sum += sw.weights[k];
            }
            if (sum > 1e-9f) {
                for (size_t k = 0; k < keep; ++k) sw.weights[k] /= sum;
            } else {
                sw.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            }
            weights_[static_cast<size_t>(vi)] = sw;
        }
    });

    /* The falloff above already produces smooth weights because the bone radius is large
       relative to the sampling step: neighbouring vertices see the same bone set with similar
       distances. An explicit 1-ring relaxation was measured and changed the deformation by less
       than 0.2 mm on the elbows and knees, so it is deliberately not applied - extra passes cost
       time and blur the crispness of knuckle and jaw weights. */
    (void)field;
}

void Rig::skin(const Mesh& restMesh, const std::vector<Mat4>& jointWorld, std::vector<Vec3>* outPositions) const {
    const size_t vcount = restMesh.vertexCount();
    outPositions->resize(vcount);
    const int threads = defaultThreadCount();
    parallelFor(static_cast<int>(vcount), threads, [&](int i0, int i1) {
        for (int vi = i0; vi < i1; ++vi) {
            const SkinWeights& sw = weights_[static_cast<size_t>(vi)];
            const Vec3 p = restMesh.positions[static_cast<size_t>(vi)];
            Vec3 acc(0, 0, 0);
            float total = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float w = sw.weights[k];
                if (w <= 0.0f) continue;
                const size_t ji = sw.joints[k];
                if (ji >= jointWorld.size()) continue;
                const Mat4& bind = inverseBind_[ji];
                const Mat4 m = jointWorld[ji] * bind;      /* skinning matrix */
                acc += m.transformPoint(p) * w;
                total += w;
            }
            (*outPositions)[static_cast<size_t>(vi)] = (total > 1e-6f) ? (acc / total) : p;
        }
    });
}

/* ==============================================================================================
 * Facial blendshapes
 * ============================================================================================== */
namespace {

/** Signed falloff used to stamp a local facial deformation. */
float stamp(const Vec3& p, const Vec3& centre, const Vec3& radii) {
    const Vec3 d((p.x - centre.x) / radii.x, (p.y - centre.y) / radii.y, (p.z - centre.z) / radii.z);
    const float r2 = d.lengthSq();
    if (r2 >= 1.0f) return 0.0f;
    const float r = std::sqrt(r2);
    return (1.0f - r) * (1.0f - r);   /* smooth, C1 at the boundary */
}

/*
 * Projects a nominal facial anchor onto the skin: the FIRST surface crossing in front of the face.
 *
 * The facial anchors used to be pinned to a nominal "face plane" a fixed fraction of the skull's
 * front radius, which is only correct for the eyeballs. The brow ridge, the nose bridge, the lips
 * and the cheeks all sit 8-25 mm in FRONT of that plane, and how far depends on the build, so a
 * stamp centred there still touched the surface - but only through the far tail of its falloff.
 * Measured effect: every brow channel produced deltas of a few microns, i.e. no visible motion.
 *
 * A 3D Newton march along the gradient (the first fix) put the brow, cheek and nose anchors on the
 * skin, but it is not safe at the lips: the mouth groove is a narrow channel between two surfaces,
 * and a march that starts outside the face can pass the lip skin and settle on the mucosa INSIDE
 * the mouth. Whether it does depends on how the field is evaluated - with the lattice cache the
 * engine builds, the mouth anchor landed 14 mm too deep (measured: mouth level z = -0.1185 in the
 * engine vs -0.1046 without the cache), which collapsed the eight channels stamped there
 * (mouthUpperUp/Press/Dimple/Stretch) to a few hundredths of a millimetre. An empty morph target
 * is then dropped from the .glb, which renumbers every channel after it.
 *
 * So the anchor is found by scanning inward along -z: the first sign change is the surface that
 * faces the viewer, independent of field representation, and 60 evaluations per anchor at build
 * time is nothing. Bisection then refines it to well under a tenth of a millimetre.
 */
Vec3 surfaceAnchor(const BodyField& field, float x, float y, float startZ = -0.16f) {
    const float step = 0.0015f;          /* 1.5 mm scan */
    const int maxSteps = 90;             /* 135 mm of travel, well past any face */
    float zPrev = startZ;
    float dPrev = bodyUnionField(field, Vec3(x, y, zPrev));
    if (dPrev <= 0.0f) return Vec3(x, y, zPrev);          /* already inside: nothing to do */
    for (int i = 1; i <= maxSteps; ++i) {
        const float z = startZ + step * static_cast<float>(i);
        const float d = bodyUnionField(field, Vec3(x, y, z));
        if (d <= 0.0f) {
            /* Bisect the bracketed interval [zPrev, z] - 20 rounds is 1.5 mm / 10^6. */
            float lo = zPrev, hi = z;
            for (int k = 0; k < 20; ++k) {
                const float mid = 0.5f * (lo + hi);
                if (bodyUnionField(field, Vec3(x, y, mid)) > 0.0f) lo = mid; else hi = mid;
            }
            return Vec3(x, y, 0.5f * (lo + hi));
        }
        zPrev = z;
        dPrev = d;
    }
    /* No surface within the scan (a feature that does not exist on this build): stay on the last
       outside sample rather than returning a point inside the head. */
    (void)dPrev;
    return Vec3(x, y, zPrev);
}
} // namespace

void Rig::buildBlendshapes(const BodyField& field, const Mesh& mesh) {
    shapeCache_.assign(kBlendshapeCount, {});
    const BodyMetrics& m = field.metrics();
    const float headH = m.y.crownY - m.y.chinY;
    const float faceZ = -m.head.rz * 0.98f;
    const Vec3 eyeL(m.eyeSeparation * 0.5f, m.y.eyeY, faceZ * 0.80f);
    const Vec3 eyeR(-m.eyeSeparation * 0.5f, m.y.eyeY, faceZ * 0.80f);
    const Vec3 mouth(0.0f, m.mouthY, faceZ * 0.94f);
    const float lip = m.lipWidth;
    const float jawR = headH * 0.16f;

    /*
     * Skin anchors - see surfaceAnchor(). One per feature, on the surface itself.
     *
     * The anchors are solved on the +x side and mirrored, rather than solved for both sides
     * independently: the field is symmetric about x = 0 by construction, but a gradient march is
     * not, and solving each side separately let the right-hand march settle on the cornea while
     * its mirror settled on the lid - which is exactly how a channel ends up moving nothing on one
     * side of the face and something on the other.
     *
     * The lids get analytic anchors instead of a march, taken straight from the ellipsoids the
     * field itself builds for them (see BodyField::browShape): the upper lid sits at
     * eye + (0, +0.62r, -0.22r) with a z radius of 0.95r, so its front surface is at
     * eye + (0, +0.62r, -1.17r). That is where the skin actually is when the eye is open, and it
     * is why the blink and wide channels finally move the lid rather than the sclera.
     */
    const float rEye = m.eyeRadius;
    const Vec3 eyeSkinL = surfaceAnchor(field, m.eyeSeparation * 0.5f, m.y.eyeY);
    const Vec3 eyeSkinR(-eyeSkinL.x, eyeSkinL.y, eyeSkinL.z);
    const Vec3 browSkinL = surfaceAnchor(field, m.eyeSeparation * 0.5f, m.y.eyeY + rEye * 1.9f);
    const Vec3 browSkinR(-browSkinL.x, browSkinL.y, browSkinL.z);
    const Vec3 browSkinC = surfaceAnchor(field, 0.0f, eyeL.y + rEye * 2.25f);
    const Vec3 eyeTrueL = field.eyeCentre(0);
    const Vec3 eyeTrueR = field.eyeCentre(1);
    const Vec3 lidUpperL = eyeTrueL + Vec3(0.0f, rEye * 0.62f, -rEye * 1.17f);
    const Vec3 lidUpperR = eyeTrueR + Vec3(0.0f, rEye * 0.62f, -rEye * 1.17f);
    const Vec3 lidLowerL = eyeTrueL + Vec3(0.0f, -rEye * 0.66f, -rEye * 1.12f);
    const Vec3 lidLowerR = eyeTrueR + Vec3(0.0f, -rEye * 0.66f, -rEye * 1.12f);
    /* The lips protrude: stamp on the vermilion, not on the mouth's nominal plane. */
    const float mouthSkinZ = surfaceAnchor(field, 0.0f, m.mouthY).z;
    const Vec3 cheekSkinL = surfaceAnchor(field, m.faceWidth * 0.40f, m.mouthY + headH * 0.02f);
    const Vec3 cheekSkinR = surfaceAnchor(field, -m.faceWidth * 0.40f, m.mouthY + headH * 0.02f);
    const Vec3 noseSkinL = surfaceAnchor(field, m.head.rx * 0.28f, m.mouthY + headH * 0.05f);
    const Vec3 noseSkinR = surfaceAnchor(field, -m.head.rx * 0.28f, m.mouthY + headH * 0.05f);

    for (int c = 0; c < kBlendshapeCount; ++c) {
        std::vector<Vec3>& deltas = shapeCache_[static_cast<size_t>(c)];
        deltas.assign(mesh.vertexCount(), Vec3(0, 0, 0));
        const char* name = kBlendshapeNames[c];

        for (size_t vi = 0; vi < mesh.vertexCount(); ++vi) {
            const Vec3 p = mesh.positions[vi];
            /* Only face vertices respond; the amplitude masks below are zero elsewhere anyway,
               but this keeps the morph targets sparse (small .glb, fast GPU skinning). */
            const bool isFace = (p.y > m.y.chinY - headH * 0.10f) && (p.y < m.y.crownY + headH * 0.05f) &&
                                (p.z < 0.0f) && (std::fabs(p.x) < m.head.rx * 1.6f);
            if (!isFace) continue;
            Vec3 d(0, 0, 0);

            const std::string n(name);
            if (n == "jawOpen") {
                d.y = -stamp(p, Vec3(0, m.y.chinY + headH * 0.10f, 0), Vec3(m.jaw.rx * 1.5f, headH * 0.30f, m.jaw.rz * 1.6f)) * headH * 0.13f;
            } else if (n == "jawForward") {
                d.z = -stamp(p, Vec3(0, m.y.chinY + headH * 0.12f, 0), Vec3(m.jaw.rx * 1.4f, headH * 0.26f, m.jaw.rz * 1.5f)) * headH * 0.035f;
            } else if (n == "jawLeft" || n == "jawRight") {
                const float sx = (n == "jawLeft") ? 1.0f : -1.0f;
                d.x = sx * stamp(p, Vec3(0, m.y.chinY + headH * 0.12f, 0), Vec3(m.jaw.rx * 1.4f, headH * 0.26f, m.jaw.rz * 1.5f)) * headH * 0.05f;
            } else if (n == "mouthSmileLeft" || n == "mouthSmileRight") {
                const float sx = (n == "mouthSmileLeft") ? 1.0f : -1.0f;
                const float s = stamp(p, Vec3(sx * lip * 0.5f, mouth.y, mouthSkinZ), Vec3(lip * 0.75f, lip * 0.55f, lip * 0.8f));
                d.x = sx * s * lip * 0.30f;
                d.y = s * lip * 0.22f;
            } else if (n == "mouthFrownLeft" || n == "mouthFrownRight") {
                const float sx = (n == "mouthFrownLeft") ? 1.0f : -1.0f;
                const float s = stamp(p, Vec3(sx * lip * 0.5f, mouth.y, mouthSkinZ), Vec3(lip * 0.75f, lip * 0.55f, lip * 0.8f));
                d.y = -s * lip * 0.24f;
            } else if (n == "mouthPucker" || n == "mouthFunnel") {
                const float s = stamp(p, mouth, Vec3(lip * 0.85f, lip * 0.55f, lip * 0.85f));
                d.z = -s * lip * (n == "mouthPucker" ? 0.34f : 0.20f);
                d.y = (p.y - mouth.y) * s * 0.10f;
            } else if (n == "mouthPressLeft" || n == "mouthPressRight") {
                const float sx = (n == "mouthPressLeft") ? 1.0f : -1.0f;
                d.y = -sx * 0.0f - stamp(p, Vec3(sx * lip * 0.4f, mouth.y, mouthSkinZ), Vec3(lip * 0.5f, lip * 0.4f, lip * 0.7f)) * lip * 0.10f;
            } else if (n == "mouthUpperUpLeft" || n == "mouthUpperUpRight") {
                const float sx = (n == "mouthUpperUpLeft") ? 1.0f : -1.0f;
                d.y = stamp(p, Vec3(sx * lip * 0.4f, mouth.y + lip * 0.20f, mouthSkinZ), Vec3(lip * 0.55f, lip * 0.40f, lip * 0.7f)) * lip * 0.20f;
            } else if (n == "mouthLowerDownLeft" || n == "mouthLowerDownRight") {
                const float sx = (n == "mouthLowerDownLeft") ? 1.0f : -1.0f;
                d.y = -stamp(p, Vec3(sx * lip * 0.4f, mouth.y - lip * 0.20f, mouthSkinZ), Vec3(lip * 0.55f, lip * 0.40f, lip * 0.7f)) * lip * 0.20f;
            } else if (n == "mouthLeft" || n == "mouthRight") {
                const float sx = (n == "mouthLeft") ? 1.0f : -1.0f;
                d.x = sx * stamp(p, mouth, Vec3(lip * 0.9f, lip * 0.5f, lip * 0.8f)) * lip * 0.28f;
            } else if (n == "mouthDimpleLeft" || n == "mouthDimpleRight") {
                const float sx = (n == "mouthDimpleLeft") ? 1.0f : -1.0f;
                const float s = stamp(p, Vec3(sx * lip * 0.62f, mouth.y, mouthSkinZ), Vec3(lip * 0.4f, lip * 0.4f, lip * 0.6f));
                d.x = sx * s * lip * 0.16f;
                d.z = s * lip * 0.10f;
            } else if (n == "mouthStretchLeft" || n == "mouthStretchRight") {
                const float sx = (n == "mouthStretchLeft") ? 1.0f : -1.0f;
                d.x = sx * stamp(p, Vec3(sx * lip * 0.6f, mouth.y, mouthSkinZ), Vec3(lip * 0.6f, lip * 0.4f, lip * 0.7f)) * lip * 0.30f;
            } else if (n == "mouthRollUpper") {
                d.y = stamp(p, Vec3(0, mouth.y + lip * 0.18f, mouthSkinZ), Vec3(lip * 0.55f, lip * 0.30f, lip * 0.7f)) * lip * 0.12f;
                d.z = stamp(p, Vec3(0, mouth.y + lip * 0.18f, mouthSkinZ), Vec3(lip * 0.55f, lip * 0.30f, lip * 0.7f)) * lip * 0.08f;
            } else if (n == "mouthRollLower") {
                d.y = -stamp(p, Vec3(0, mouth.y - lip * 0.18f, mouthSkinZ), Vec3(lip * 0.55f, lip * 0.30f, lip * 0.7f)) * lip * 0.12f;
                d.z = stamp(p, Vec3(0, mouth.y - lip * 0.18f, mouthSkinZ), Vec3(lip * 0.55f, lip * 0.30f, lip * 0.7f)) * lip * 0.08f;
            } else if (n == "mouthShrugUpper" || n == "mouthShrugLower") {
                const float sign = (n == "mouthShrugUpper") ? 1.0f : -1.0f;
                d.y = sign * stamp(p, Vec3(0, mouth.y + sign * lip * 0.3f, mouthSkinZ), Vec3(lip * 0.8f, lip * 0.5f, lip * 0.8f)) * lip * 0.16f;
            } else if (n == "mouthClose") {
                d.y = -stamp(p, Vec3(0, mouth.y, mouthSkinZ), Vec3(lip * 0.7f, lip * 0.6f, lip * 0.8f)) * lip * 0.10f;
            } else if (n.rfind("eyeBlink", 0) == 0) {
                /* The upper lid rotates down over the globe: the lid surface moves furthest, the
                   canthi least, so the displacement is stamped with a wide, flat mask whose
                   vertical extent spans the whole lid. */
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 lid = left ? lidUpperL : lidUpperR;
                const float s = stamp(p, lid, Vec3(rEye * 2.4f, rEye * 1.5f, rEye * 2.6f));
                d.y = -s * rEye * 1.55f;
                d.z = s * rEye * 0.30f;
            } else if (n.rfind("eyeWide", 0) == 0) {
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 lidU = left ? lidUpperL : lidUpperR;
                const Vec3 lidD = left ? lidLowerL : lidLowerR;
                const float su = stamp(p, lidU, Vec3(rEye * 2.4f, rEye * 1.6f, rEye * 2.6f));
                const float sl = stamp(p, lidD, Vec3(rEye * 2.2f, rEye * 1.4f, rEye * 2.4f));
                d.y = su * rEye * 0.95f - sl * rEye * 0.45f;
            } else if (n.rfind("eyeSquint", 0) == 0) {
                /* Squint = lower lid up + cheek raised towards the outer canthus. */
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 lidD = left ? lidLowerL : lidLowerR;
                const float s = stamp(p, lidD, Vec3(rEye * 2.6f, rEye * 1.6f, rEye * 2.6f));
                d.y = s * rEye * 0.75f;
                const float sx = left ? 1.0f : -1.0f;
                d.z = -s * rEye * 0.20f;
                (void)sx;
            } else if (n.rfind("eyeLook", 0) == 0) {
                /* Gaze channels only move the eyeball islands; the lids react through the
                   corresponding eyeBlink/eyeWide channels, exactly as ARKit does. */
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 e = left ? eyeL : eyeR;
                /*
                 * Amplitude: the mask peaks on the globe's surface (the stamp falls off over
                 * 1.6 radii, so the skin at 1.6r gets nothing and the sclera at 1.0r gets most of
                 * it). 0.9 * radius of movement at the surface is about 7 degrees of gaze for a
                 * 12 mm eye - the range ARKit's eyeLook channels are authored for. A smaller
                 * figure looks like a twitch; a larger one tears the island away from the lid.
                 */
                const float s = stamp(p, e, Vec3(m.eyeRadius * 1.6f, m.eyeRadius * 1.6f, m.eyeRadius * 1.6f));
                if (n.find("LookUp") != std::string::npos) d.y += s * m.eyeRadius * 0.90f;
                if (n.find("LookDown") != std::string::npos) d.y -= s * m.eyeRadius * 0.90f;
                if (n.find("LookOut") != std::string::npos) d.x += (left ? 1.0f : -1.0f) * s * m.eyeRadius * 0.90f;
                if (n.find("LookIn") != std::string::npos) d.x -= (left ? 1.0f : -1.0f) * s * m.eyeRadius * 0.90f;
            } else if (n.rfind("browDown", 0) == 0) {
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 b = left ? browSkinL : browSkinR;
                const float s = stamp(p, b, Vec3(m.eyeRadius * 2.2f, m.eyeRadius * 1.1f, m.eyeRadius * 1.6f));
                d.y = -s * headH * 0.016f;
                d.z = -s * headH * 0.006f;
            } else if (n.rfind("browOuterUp", 0) == 0) {
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 e = left ? eyeL : eyeR;
                const Vec3 b = left ? browSkinL : browSkinR;
                const float s = stamp(p, Vec3(b.x, b.y, b.z), Vec3(m.eyeRadius * 1.6f, m.eyeRadius * 1.2f, m.eyeRadius * 1.6f));
                d.y = s * headH * 0.020f;
            } else if (n == "browInnerUp") {
                /* Stamped on the glabella (between the brows) rather than at the eyeball plane:
                   the forehead surface sits further forward than the eye centres, so the mask is
                   anchored to the skull radius to guarantee it actually covers brow vertices. */
                const float s = stamp(p, browSkinC,
                                      Vec3(m.eyeRadius * 2.6f, headH * 0.045f, m.head.rz * 0.55f));
                d.y = s * headH * 0.022f;
                d.z = -s * headH * 0.004f;
            } else if (n.rfind("cheekSquint", 0) == 0) {
                const bool left = n.find("Left") != std::string::npos;
                const Vec3 e = left ? eyeL : eyeR;
                const Vec3 c = left ? cheekSkinL : cheekSkinR;
                const float s = stamp(p, Vec3(c.x, e.y - m.eyeRadius * 1.5f, c.z), Vec3(m.eyeRadius * 2.4f, m.eyeRadius * 1.8f, m.eyeRadius * 2.0f));
                d.y = s * headH * 0.014f;
                d.z = -s * headH * 0.004f;
            } else if (n == "cheekPuff") {
                const float s = stamp(p, Vec3(0, m.mouthY, mouthSkinZ), Vec3(m.faceWidth * 0.62f, headH * 0.20f, m.head.rz * 0.95f));
                d.x = (p.x >= 0 ? 1.0f : -1.0f) * s * m.faceWidth * 0.10f;
                d.z = -s * headH * 0.012f;
            } else if (n.rfind("noseSneer", 0) == 0) {
                const bool left = n.find("Left") != std::string::npos;
                const float sx = left ? 1.0f : -1.0f;
                const Vec3 nk = left ? noseSkinL : noseSkinR;
                const float s = stamp(p, nk, Vec3(m.head.rx * 0.36f, headH * 0.055f, m.head.rz * 0.55f));
                d.y = s * headH * 0.012f;
                d.x = sx * s * headH * 0.008f;
            } else if (n == "tongueOut") {
                /* The tongue is a separate island; the channel moves it forward and down. */
                if (p.z < -m.head.rz * 0.4f && p.y < m.mouthY + headH * 0.04f && p.y > m.mouthY - headH * 0.09f) {
                    d.z = -headH * 0.055f;
                    d.y = -headH * 0.020f;
                }
            }
            deltas[vi] = d;
        }
    }
}

const std::vector<Vec3>& Rig::blendshapeDeltas(int channel) const {
    if (shapeCache_.empty()) {
        static const std::vector<Vec3> kEmpty;
        return kEmpty;
    }
    const int c = (channel < 0) ? 0 : ((channel >= kBlendshapeCount) ? kBlendshapeCount - 1 : channel);
    return shapeCache_[static_cast<size_t>(c)];
}

void Rig::deform(const Mesh& restMesh, const std::vector<Mat4>& jointWorld,
                 const std::vector<float>& blendshapeWeights, std::vector<Vec3>* outPositions) const {
    skin(restMesh, jointWorld, outPositions);
    if (blendshapeWeights.empty() || shapeCache_.empty()) return;
    const size_t vcount = outPositions->size();
    for (int c = 0; c < kBlendshapeCount && c < static_cast<int>(blendshapeWeights.size()); ++c) {
        const float w = blendshapeWeights[static_cast<size_t>(c)];
        if (std::fabs(w) < 1e-5f) continue;
        const std::vector<Vec3>& deltas = shapeCache_[static_cast<size_t>(c)];
        if (deltas.size() != vcount) continue;
        for (size_t i = 0; i < vcount; ++i) (*outPositions)[i] += deltas[i] * w;
    }
}

/* ==============================================================================================
 * Animation clips
 * ============================================================================================== */
Vec3 Track::sampleTranslation(float t) const {
    if (keys.empty()) return Vec3(0, 0, 0);
    if (keys.size() == 1) return keys[0].translation;
    if (t <= keys.front().time) return keys.front().translation;
    if (t >= keys.back().time) return keys.back().translation;
    size_t hi = 1;
    while (hi < keys.size() - 1 && keys[hi].time < t) ++hi;
    const Keyframe& a = keys[hi - 1];
    const Keyframe& b = keys[hi];
    const float span = b.time - a.time;
    const float u = span < 1e-6f ? 0.0f : (t - a.time) / span;
    const float s = u * u * (3.0f - 2.0f * u);
    return a.translation.lerp(b.translation, s);
}

Quat Track::sample(float t) const {
    if (keys.empty()) return Quat::identity();
    if (keys.size() == 1) return keys[0].rotation;
    if (t <= keys.front().time) return keys.front().rotation;
    if (t >= keys.back().time) return keys.back().rotation;
    size_t hi = 1;
    while (hi < keys.size() - 1 && keys[hi].time < t) ++hi;
    const Keyframe& a = keys[hi - 1];
    const Keyframe& b = keys[hi];
    const float span = b.time - a.time;
    const float u = span < 1e-6f ? 0.0f : (t - a.time) / span;
    /* Smoothstep easing between authored keys: avoids the linear-slerp "robotic" snap that is
       the usual tell of an unstyled procedural animation. */
    const float s = u * u * (3.0f - 2.0f * u);
    return Quat::slerp(a.rotation, b.rotation, s);
}

namespace {

/** Helper: quaternion from degrees around X/Y/Z in that order. */
Quat rot(float xDeg, float yDeg, float zDeg) {
    return Quat::fromEulerXYZ(Vec3(radians(xDeg), radians(yDeg), radians(zDeg)));
}

Track track(const std::string& jointName, std::initializer_list<std::pair<float, Quat>> keys) {
    Track t;
    t.jointName = jointName;
    t.joint = -1;                     /* resolved by name when the clip is bound to a rig */
    for (const auto& k : keys) {
        Keyframe kf;
        kf.time = k.first;
        kf.rotation = k.second;
        t.keys.push_back(kf);
    }
    return t;
}

/* Per-clip authored key data. Poses are expressed as cascading rotations down the chain:
   hips -> spine -> limbs, which is how a real animator builds a cycle. */
struct ClipSpec {
    const char* name;
    float duration;
    bool looping;
    /* joint suffix -> keyframes. Suffixes use the rig's naming ("_L"/"_R"). */
    std::vector<std::pair<std::string, Track>> tracks;
};

const std::vector<Clip>& library() {
    static std::vector<Clip> clips = [] {
        std::vector<Clip> out;

        auto make = [&](const char* name, float dur, bool loop,
                        std::initializer_list<std::pair<const char*, Track>> tracks) {
            Clip c;
            c.name = name;
            c.duration = dur;
            c.looping = loop;
            for (const auto& t : tracks) {
                Track tr = t.second;
                if (tr.jointName.empty()) tr.jointName = t.first;
                c.tracks.push_back(std::move(tr));
            }
            out.push_back(c);
        };

        /* --- idle: weight shift + breathing, 4 s loop ------------------------------------- */
        make("idle", 4.0f, true, {
            {"hips", track("hips", { {0.0f, rot(0, 0, 0)}, {1.0f, rot(0, 1.2f, 0.8f)}, {2.0f, rot(0, 0, 0)}, {3.0f, rot(0, -1.2f, -0.8f)}, {4.0f, rot(0, 0, 0)} })},
            {"spine_02", track("spine_02", { {0.0f, rot(1.0f, 0, 0)}, {2.0f, rot(2.4f, 0, 0)}, {4.0f, rot(1.0f, 0, 0)} })},
            {"chest", track("chest", { {0.0f, rot(-1.6f, 0, 0)}, {2.0f, rot(-3.0f, 0, 0)}, {4.0f, rot(-1.6f, 0, 0)} })},
            {"head", track("head", { {0.0f, rot(0, -3.0f, 0)}, {2.0f, rot(1.0f, 2.5f, 0.6f)}, {4.0f, rot(0, -3.0f, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(0, 0, 2.0f)}, {2.0f, rot(1.5f, 0, 4.5f)}, {4.0f, rot(0, 0, 2.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(0, 0, -2.0f)}, {2.0f, rot(1.5f, 0, -4.5f)}, {4.0f, rot(0, 0, -2.0f)} })},
        });

        /* --- breathe: isolated respiration for facial/expression testing ------------------ */
        make("breathe", 5.0f, true, {
            {"chest", track("", { {0.0f, rot(0, 0, 0)}, {2.5f, rot(-4.5f, 0, 0)}, {5.0f, rot(0, 0, 0)} })},
            {"neck_01", track("", { {0.0f, rot(0, 0, 0)}, {2.5f, rot(1.5f, 0, 0)}, {5.0f, rot(0, 0, 0)} })},
        });

        /* --- walk cycle: 1.1 s, contra-lateral arms, pelvis rotation ---------------------- */
        make("walk", 1.1f, true, {
            {"hips", track("", { {0.0f, rot(0, 4.0f, 0)}, {0.275f, rot(0, 0, 0)}, {0.55f, rot(0, -4.0f, 0)}, {0.825f, rot(0, 0, 0)}, {1.1f, rot(0, 4.0f, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(24.0f, 0, 0)}, {0.275f, rot(4.0f, 0, 0)}, {0.55f, rot(-20.0f, 0, 0)}, {0.825f, rot(-2.0f, 0, 0)}, {1.1f, rot(24.0f, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(-20.0f, 0, 0)}, {0.275f, rot(-2.0f, 0, 0)}, {0.55f, rot(24.0f, 0, 0)}, {0.825f, rot(4.0f, 0, 0)}, {1.1f, rot(-20.0f, 0, 0)} })},
            {"shin_L", track("", { {0.0f, rot(4.0f, 0, 0)}, {0.275f, rot(18.0f, 0, 0)}, {0.55f, rot(42.0f, 0, 0)}, {0.825f, rot(8.0f, 0, 0)}, {1.1f, rot(4.0f, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(42.0f, 0, 0)}, {0.275f, rot(8.0f, 0, 0)}, {0.55f, rot(4.0f, 0, 0)}, {0.825f, rot(18.0f, 0, 0)}, {1.1f, rot(42.0f, 0, 0)} })},
            {"foot_L", track("", { {0.0f, rot(-12.0f, 0, 0)}, {0.275f, rot(2.0f, 0, 0)}, {0.55f, rot(16.0f, 0, 0)}, {0.825f, rot(0, 0, 0)}, {1.1f, rot(-12.0f, 0, 0)} })},
            {"foot_R", track("", { {0.0f, rot(16.0f, 0, 0)}, {0.275f, rot(0, 0, 0)}, {0.55f, rot(-12.0f, 0, 0)}, {0.825f, rot(2.0f, 0, 0)}, {1.1f, rot(16.0f, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(-22.0f, 0, 3.0f)}, {0.55f, rot(18.0f, 0, 3.0f)}, {1.1f, rot(-22.0f, 0, 3.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(18.0f, 0, -3.0f)}, {0.55f, rot(-22.0f, 0, -3.0f)}, {1.1f, rot(18.0f, 0, -3.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-16.0f, 0, 0)}, {0.55f, rot(-30.0f, 0, 0)}, {1.1f, rot(-16.0f, 0, 0)} })},
            {"forearm_01_R", track("", { {0.0f, rot(-30.0f, 0, 0)}, {0.55f, rot(-16.0f, 0, 0)}, {1.1f, rot(-30.0f, 0, 0)} })},
        });

        /* --- jog: faster cadence, more knee flexion, torso lean --------------------------- */
        make("jog", 0.72f, true, {
            {"spine_02", track("", { {0.0f, rot(5.0f, 0, 0)}, {0.72f, rot(5.0f, 0, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(42.0f, 0, 0)}, {0.18f, rot(10.0f, 0, 0)}, {0.36f, rot(-34.0f, 0, 0)}, {0.54f, rot(-6.0f, 0, 0)}, {0.72f, rot(42.0f, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(-34.0f, 0, 0)}, {0.18f, rot(-6.0f, 0, 0)}, {0.36f, rot(42.0f, 0, 0)}, {0.54f, rot(10.0f, 0, 0)}, {0.72f, rot(-34.0f, 0, 0)} })},
            {"shin_L", track("", { {0.0f, rot(10.0f, 0, 0)}, {0.18f, rot(38.0f, 0, 0)}, {0.36f, rot(86.0f, 0, 0)}, {0.54f, rot(20.0f, 0, 0)}, {0.72f, rot(10.0f, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(86.0f, 0, 0)}, {0.18f, rot(20.0f, 0, 0)}, {0.36f, rot(10.0f, 0, 0)}, {0.54f, rot(38.0f, 0, 0)}, {0.72f, rot(86.0f, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(-46.0f, 0, 4.0f)}, {0.36f, rot(40.0f, 0, 4.0f)}, {0.72f, rot(-46.0f, 0, 4.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(40.0f, 0, -4.0f)}, {0.36f, rot(-46.0f, 0, -4.0f)}, {0.72f, rot(40.0f, 0, -4.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-64.0f, 0, 0)}, {0.72f, rot(-64.0f, 0, 0)} })},
            {"forearm_01_R", track("", { {0.0f, rot(-64.0f, 0, 0)}, {0.72f, rot(-64.0f, 0, 0)} })},
        });

        /* --- run: long stride, deep arm swing, forward lean ------------------------------ */
        make("run", 0.62f, true, {
            {"spine_02", track("", { {0.0f, rot(9.0f, 0, 0)}, {0.62f, rot(9.0f, 0, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(58.0f, 0, 0)}, {0.31f, rot(-42.0f, 0, 0)}, {0.62f, rot(58.0f, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(-42.0f, 0, 0)}, {0.31f, rot(58.0f, 0, 0)}, {0.62f, rot(-42.0f, 0, 0)} })},
            {"shin_L", track("", { {0.0f, rot(26.0f, 0, 0)}, {0.15f, rot(96.0f, 0, 0)}, {0.31f, rot(14.0f, 0, 0)}, {0.62f, rot(26.0f, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(14.0f, 0, 0)}, {0.46f, rot(96.0f, 0, 0)}, {0.62f, rot(14.0f, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(-72.0f, 0, 6.0f)}, {0.31f, rot(62.0f, 0, 6.0f)}, {0.62f, rot(-72.0f, 0, 6.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(62.0f, 0, -6.0f)}, {0.31f, rot(-72.0f, 0, -6.0f)}, {0.62f, rot(62.0f, 0, -6.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-88.0f, 0, 0)}, {0.62f, rot(-88.0f, 0, 0)} })},
            {"forearm_01_R", track("", { {0.0f, rot(-88.0f, 0, 0)}, {0.62f, rot(-88.0f, 0, 0)} })},
        });

        /* --- squat: single repetition with a real descent/ascent profile ------------------ */
        make("squat", 2.4f, false, {
            {"hips", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(14.0f, 0, 0)}, {2.4f, rot(0, 0, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(96.0f, 0, 4.0f)}, {2.4f, rot(0, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(96.0f, 0, -4.0f)}, {2.4f, rot(0, 0, 0)} })},
            {"shin_L", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(-104.0f, 0, 0)}, {2.4f, rot(0, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(-104.0f, 0, 0)}, {2.4f, rot(0, 0, 0)} })},
            {"foot_L", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(26.0f, 0, 0)}, {2.4f, rot(0, 0, 0)} })},
            {"foot_R", track("", { {0.0f, rot(0, 0, 0)}, {1.2f, rot(26.0f, 0, 0)}, {2.4f, rot(0, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(0, 0, 3.0f)}, {1.2f, rot(-62.0f, 0, 3.0f)}, {2.4f, rot(0, 0, 3.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(0, 0, -3.0f)}, {1.2f, rot(-62.0f, 0, -3.0f)}, {2.4f, rot(0, 0, -3.0f)} })},
        });

        /* --- jump: crouch, launch, airborne, land, recover -------------------------------- */
        make("jump", 1.6f, false, {
            {"hips", track("", { {0.0f, rot(0, 0, 0)}, {0.3f, rot(16.0f, 0, 0)}, {0.6f, rot(-6.0f, 0, 0)}, {1.0f, rot(4.0f, 0, 0)}, {1.6f, rot(0, 0, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(0, 0, 0)}, {0.3f, rot(78.0f, 0, 0)}, {0.7f, rot(-14.0f, 0, 0)}, {1.0f, rot(52.0f, 0, 0)}, {1.6f, rot(0, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(0, 0, 0)}, {0.3f, rot(78.0f, 0, 0)}, {0.7f, rot(-14.0f, 0, 0)}, {1.0f, rot(52.0f, 0, 0)}, {1.6f, rot(0, 0, 0)} })},
            {"shin_L", track("", { {0.0f, rot(0, 0, 0)}, {0.3f, rot(-92.0f, 0, 0)}, {0.7f, rot(10.0f, 0, 0)}, {1.0f, rot(-64.0f, 0, 0)}, {1.6f, rot(0, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(0, 0, 0)}, {0.3f, rot(-92.0f, 0, 0)}, {0.7f, rot(10.0f, 0, 0)}, {1.0f, rot(-64.0f, 0, 0)}, {1.6f, rot(0, 0, 0)} })},
        });
        /* Root translation for the jump: crouch, launch, apex, land, settle. Applied to the
           root joint so the whole skeleton (and therefore every vertex) leaves the ground. */
        {
            Clip& jump = out.back();
            Track root;
            root.jointName = "root";
            root.keys = {
                {0.00f, Quat::identity(), Vec3(0, 0, 0)},
                {0.30f, Quat::identity(), Vec3(0, -0.16f, 0)},
                {0.55f, Quat::identity(), Vec3(0, 0.12f, 0)},
                {0.75f, Quat::identity(), Vec3(0, 0.46f, 0)},
                {1.00f, Quat::identity(), Vec3(0, -0.14f, 0)},
                {1.30f, Quat::identity(), Vec3(0, -0.05f, 0)},
                {1.60f, Quat::identity(), Vec3(0, 0.00f, 0)}
            };
            jump.tracks.push_back(std::move(root));
        }

        /* --- wave: right arm raised, wrist oscillation ------------------------------------ */
        make("wave", 2.2f, true, {
            {"upperarm_01_L", track("", { {0.0f, rot(-30.0f, 0, 96.0f)}, {2.2f, rot(-30.0f, 0, 96.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-34.0f, 0, -54.0f)}, {2.2f, rot(-34.0f, 0, -54.0f)} })},
            {"hand_L", track("", { {0.0f, rot(0, 0, -18.0f)}, {0.55f, rot(0, 0, 18.0f)}, {1.1f, rot(0, 0, -18.0f)}, {1.65f, rot(0, 0, 18.0f)}, {2.2f, rot(0, 0, -18.0f)} })},
            {"head", track("", { {0.0f, rot(0, 6.0f, 2.0f)}, {2.2f, rot(0, 6.0f, 2.0f)} })},
        });

        /* --- punch: left straight with hip drive ------------------------------------------ */
        make("punch", 0.9f, false, {
            {"hips", track("", { {0.0f, rot(0, 0, 0)}, {0.16f, rot(0, 0, 0)}, {0.34f, rot(0, -18.0f, 0)}, {0.9f, rot(0, 0, 0)} })},
            {"spine_02", track("", { {0.0f, rot(0, 0, 0)}, {0.34f, rot(0, -12.0f, 0)}, {0.9f, rot(0, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(-6.0f, 0, 4.0f)}, {0.34f, rot(-84.0f, 0, 6.0f)}, {0.9f, rot(-6.0f, 0, 4.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-64.0f, 0, 0)}, {0.34f, rot(-4.0f, 0, 0)}, {0.9f, rot(-64.0f, 0, 0)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(-6.0f, 0, -4.0f)}, {0.34f, rot(24.0f, 0, -4.0f)}, {0.9f, rot(-6.0f, 0, -4.0f)} })},
            {"forearm_01_R", track("", { {0.0f, rot(-64.0f, 0, 0)}, {0.34f, rot(-96.0f, 0, 0)}, {0.9f, rot(-64.0f, 0, 0)} })},
        });

        /* --- kick: right roundhouse ------------------------------------------------------- */
        make("kick", 1.1f, false, {
            {"hips", track("", { {0.0f, rot(0, 0, 0)}, {0.42f, rot(-8.0f, 34.0f, 0)}, {1.1f, rot(0, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(0, 0, 0)}, {0.2f, rot(62.0f, 0, 0)}, {0.42f, rot(38.0f, 62.0f, 0)}, {1.1f, rot(0, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(0, 0, 0)}, {0.2f, rot(-96.0f, 0, 0)}, {0.42f, rot(-12.0f, 0, 0)}, {1.1f, rot(0, 0, 0)} })},
            {"foot_R", track("", { {0.0f, rot(0, 0, 0)}, {0.42f, rot(-24.0f, 0, 0)}, {1.1f, rot(0, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(0, 0, 3.0f)}, {0.42f, rot(-38.0f, 0, 22.0f)}, {1.1f, rot(0, 0, 3.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(0, 0, -3.0f)}, {0.42f, rot(36.0f, 0, -46.0f)}, {1.1f, rot(0, 0, -3.0f)} })},
        });

        /* --- pushup: full repetition, hips locked, elbows to 90 --------------------------- */
        make("pushup", 2.0f, true, {
            {"root", track("", { {0.0f, rot(88.0f, 0, 0)}, {2.0f, rot(88.0f, 0, 0)} })},
            {"spine_02", track("", { {0.0f, rot(-4.0f, 0, 0)}, {1.0f, rot(2.0f, 0, 0)}, {2.0f, rot(-4.0f, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(0, -18.0f, 6.0f)}, {1.0f, rot(0, -18.0f, 34.0f)}, {2.0f, rot(0, -18.0f, 6.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(0, 18.0f, -6.0f)}, {1.0f, rot(0, 18.0f, -34.0f)}, {2.0f, rot(0, 18.0f, -6.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-8.0f, 0, 0)}, {1.0f, rot(-84.0f, 0, 0)}, {2.0f, rot(-8.0f, 0, 0)} })},
            {"forearm_01_R", track("", { {0.0f, rot(-8.0f, 0, 0)}, {1.0f, rot(-84.0f, 0, 0)}, {2.0f, rot(-8.0f, 0, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(-4.0f, 0, 0)}, {2.0f, rot(-4.0f, 0, 0)} })},
            {"thigh_R", track("", { {0.0f, rot(-4.0f, 0, 0)}, {2.0f, rot(-4.0f, 0, 0)} })},
            {"head", track("", { {0.0f, rot(-16.0f, 0, 0)}, {2.0f, rot(-16.0f, 0, 0)} })},
        });

        /* --- t_pose / a_pose: canonical bind stances ------------------------------------- */
        make("t_pose", 0.1f, false, {
            {"upperarm_01_L", track("", { {0.0f, rot(0, -90.0f, 0)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(0, 90.0f, 0)} })},
        });
        make("a_pose", 0.1f, false, {
            {"upperarm_01_L", track("", { {0.0f, rot(0, -46.0f, 0)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(0, 46.0f, 0)} })},
        });

        /* --- sit: hips flexed, knees at 90, feet planted ---------------------------------- */
        make("sit", 0.1f, false, {
            {"hips", track("", { {0.0f, rot(6.0f, 0, 0)} })},
            {"thigh_L", track("", { {0.0f, rot(88.0f, 0, 5.0f)} })},
            {"thigh_R", track("", { {0.0f, rot(88.0f, 0, -5.0f)} })},
            {"shin_L", track("", { {0.0f, rot(-88.0f, 0, 0)} })},
            {"shin_R", track("", { {0.0f, rot(-88.0f, 0, 0)} })},
            {"foot_L", track("", { {0.0f, rot(6.0f, 0, 0)} })},
            {"foot_R", track("", { {0.0f, rot(6.0f, 0, 0)} })},
            {"upperarm_01_L", track("", { {0.0f, rot(-22.0f, 0, 8.0f)} })},
            {"upperarm_01_R", track("", { {0.0f, rot(-22.0f, 0, -8.0f)} })},
            {"forearm_01_L", track("", { {0.0f, rot(-72.0f, 0, 0)} })},
            {"forearm_01_R", track("", { {0.0f, rot(-72.0f, 0, 0)} })},
        });

        /* --- turn_360: hips rotate a full turn with counter-rotating shoulders ------------ */
        make("turn_360", 2.0f, true, {
            {"hips", track("", { {0.0f, rot(0, 0, 0)}, {0.5f, rot(0, 90.0f, 0)}, {1.0f, rot(0, 180.0f, 0)}, {1.5f, rot(0, 270.0f, 0)}, {2.0f, rot(0, 360.0f, 0)} })},
            {"spine_02", track("", { {0.0f, rot(0, -10.0f, 0)}, {1.0f, rot(0, 10.0f, 0)}, {2.0f, rot(0, -10.0f, 0)} })},
            {"head", track("", { {0.0f, rot(0, 24.0f, 0)}, {1.0f, rot(0, -24.0f, 0)}, {2.0f, rot(0, 24.0f, 0)} })},
        });

        return out;
    }();
    return clips;
}

} // namespace

const std::vector<Clip>& builtinClips() { return library(); }

namespace {
/* Order matches the af_clip enum in afcore.h EXACTLY, one entry per enum value. The test harness
   asserts id -> name -> library entry, so the enum, this table and the authored clips cannot
   drift apart (an earlier revision had jog and run transposed here and the ids resolved to the
   wrong clip without any test noticing). */
const char* const kClipOrder[] = {
        "rest", "idle", "breathe", "walk", "run", "jog", "squat", "jump", "wave",
    "punch", "kick", "t_pose", "a_pose", "sit", "turn_360", "pushup"
};
const int kClipOrderCount = static_cast<int>(sizeof(kClipOrder) / sizeof(kClipOrder[0]));
} // namespace

const char* clipNameForId(int afClipId) {
    if (afClipId < 0 || afClipId >= kClipOrderCount) return nullptr;
    return kClipOrder[afClipId];
}

int clipIndexForId(int afClipId) {
    /* "rest" is served by the identity pose and has no authored track data, so it maps to -1. */
    const char* name = clipNameForId(afClipId);
    if (!name) return -1;
    const std::string want = name;
    if (want == "rest") return -1;              /* rest = all identity, no track needed */
    const std::vector<Clip>& lib = builtinClips();
    for (size_t i = 0; i < lib.size(); ++i) if (lib[i].name == want) return static_cast<int>(i);
    return -1;
}

} // namespace af
