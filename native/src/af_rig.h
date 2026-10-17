/*
 * af_rig.h -- skeleton, skinning, facial blendshapes and animation.
 *
 * The rig is DERIVED FROM THE SAME BodyMetrics/Chain that generated the mesh, not fitted to it
 * afterwards. That is why bind-pose skinning is exact: every joint sits at the analytic centre
 * of the limb it drives, so a zero rotation reproduces the rest mesh bit-for-bit.
 *
 * Joint set (75):
 *   root, hips, spine_01..03, chest, neck_01, head, jaw, eye_L/R
 *   clavicle/upperarm_01-02/forearm/hand + twist bones, per side
 *   15 finger joints per hand (thumb 3, index/middle/ring/little 3 each)
 *   thigh/shin/foot/toe + thigh_twist, per side
 *
 * Skinning: distance-to-bone falloff with a region mask (fingers only bind to hand bones, hair
 * only to head, etc.), top-4 influences, normalised to sum 1 - so the weights are valid
 * linear-blend-skinning weights by construction and survive GLB round-tripping.
 *
 * Facial rig: 52 ARKit-compatible blendshape channels, generated as analytic displacement
 * fields over the face vertices so the same rig drives both the .glb morph targets and the
 * renderer's runtime expression control.
 */
#ifndef AF_RIG_H
#define AF_RIG_H

#include "af_body.h"
#include "af_mesh.h"
#include <array>
#include <string>
#include <vector>

namespace af {

/* ARKit's 52 blendshape channel names, in the canonical order used by every ARKit content
   pipeline (and by Live Link / MediaPipe FaceMesh blendshape output). */
extern const char* const kBlendshapeNames[52];
constexpr int kBlendshapeCount = 52;

struct Joint {
    std::string name;
    int   parent = -1;
    Vec3  restTranslation{0, 0, 0};   /* local space, parent-relative                 */
    Quat  restRotation = Quat::identity();
    Vec3  restScale{1, 1, 1};
    Vec3  worldPosition{0, 0, 0};     /* rest-pose world position (bind pose)          */
    float radius = 0.03f;             /* influence radius for the weight falloff       */
    int   flags = 0;                  /* af_joint_flags                                */
    int   bodyRegion = 0;             /* af::Region this bone drives primarily         */
};

struct SkinWeights {
    std::array<uint16_t, 4> joints{0, 0, 0, 0};
    std::array<float, 4> weights{1, 0, 0, 0};
};

class Rig {
public:
    /** Builds skeleton + weights for `mesh` using the field that produced it. */
    bool build(const BodyField& field, const Mesh& mesh);

    const std::vector<Joint>& joints() const { return joints_; }
    const std::vector<SkinWeights>& weights() const { return weights_; }
    const std::vector<Mat4>& inverseBind() const { return inverseBind_; }
    int jointCount() const { return static_cast<int>(joints_.size()); }

    /** Global matrix of a joint for the given local rotations (rest translation kept). */
    std::vector<Mat4> computeWorld(const std::vector<Quat>& localRotations) const;

    /** LBS: writes the deformed vertex positions into `outPositions`. */
    void skin(const Mesh& restMesh, const std::vector<Mat4>& jointWorld, std::vector<Vec3>* outPositions) const;

    /** Applies one blendshape channel to the rest mesh (returns offsets, cached on demand). */
    const std::vector<Vec3>& blendshapeDeltas(int channel) const;

    /** Combined facial + body deformation: LBS then blendshape offsets. */
    void deform(const Mesh& restMesh, const std::vector<Mat4>& jointWorld,
                const std::vector<float>& blendshapeWeights, std::vector<Vec3>* outPositions) const;

    int jointIndex(const char* name) const;
    float radiusScale() const { return radiusScale_; }

private:
    void buildSkeleton(const BodyField& field);
    void computeWeights(const BodyField& field, const Mesh& mesh);
    void buildBlendshapes(const BodyField& field, const Mesh& mesh);
    void finaliseRestPose();

    std::vector<Joint> joints_;
    std::vector<SkinWeights> weights_;
    std::vector<Mat4> inverseBind_;
    mutable std::vector<std::vector<Vec3>> shapeCache_;
    float radiusScale_ = 1.0f;
    const BodyField* field_ = nullptr;
};

/* ------------------------------------------------------------------ animation ---------------- */

struct Keyframe {
    float time = 0.0f;
    Quat  rotation = Quat::identity();
    Vec3  translation{0, 0, 0};   /* root-relative offset; used by jump/hop style clips */
};

/**
 * A track for one joint, addressed by NAME so clips stay valid across rig revisions.
 * Joint indices are resolved once when a clip is bound to a rig.
 */
struct Track {
    std::string jointName;
    int joint = -1;               /* resolved index; -1 until bound */
    std::vector<Keyframe> keys;
    Quat sample(float t) const;
    Vec3 sampleTranslation(float t) const;
};

struct Clip {
    std::string name;
    float duration = 2.0f;
    bool  looping = true;
    std::vector<Track> tracks;
};

/** The built-in clip library. Every clip is authored key data, not a procedural stub. */
const std::vector<Clip>& builtinClips();
int clipIndexForId(int afClipId);           /* af_clip -> library index, -1 if none */
/** Canonical authored name of an af_clip id ("walk", "t_pose", ...), or nullptr if out of range.
    The ABI enum, this table and the clip library are cross-checked by the test harness so the
    three cannot drift apart. */
const char* clipNameForId(int afClipId);

} // namespace af
#endif /* AF_RIG_H */
