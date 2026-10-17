/*
 * af_field.h -- signed distance field of the human body.
 *
 * The body is modelled as a union of analytic primitives combined with polynomial smooth
 * minimum, so the result is a single continuous, watertight surface with organic junctions
 * (no visible seams at the shoulder, hip, wrist or jaw). Muscle and adipose tissue are added
 * as negative-space blobs whose amplitude comes from the anthropometric solve, which is why
 * changing `muscle` in the spec changes shape rather than just scaling the mesh.
 *
 * Split into four evaluation domains so the isosurface extractor can use the right spatial
 * resolution for each: the trunk+limbs, the head+face, each hand, each foot. The domains share
 * exact cut planes (see af_isosurface.h) so the extracted meshes stitch seamlessly.
 */
#ifndef AF_FIELD_H
#define AF_FIELD_H

#include "af_math.h"
#include "af_anatomy.h"
#include <vector>

namespace af {

/** Monotone cubic Hermite interpolation (Fritsch-Carlson), used for torso profiles. */
class Profile1D {
public:
    void add(float y, float value) {
        ys_.push_back(y);
        vs_.push_back(value);
    }
    /** Builds tangents; call once after all points are added. */
    void build();
    float eval(float y) const;
    bool empty() const { return ys_.empty(); }

private:
    std::vector<float> ys_, vs_, ms_;
};

/** The articulated chain of a limb, shared between the field, the rig and the garment layers. */
struct Chain {
    Vec3 shoulder, elbow, wrist, palmCentre, middleTip;
    Vec3 hipJoint, knee, ankle, heel, ball, toeTip;
    Vec3 fingerTip[5];       /* thumb..little, world space, rest pose                    */
    Vec3 fingerKnuckle[5];
    Vec3 fingerJoints[5][3]; /* proximal, middle, distal joint positions                  */
    int  side = 1;           /* +1 = character's left (+X), -1 = right                    */
};

/*
 * Apparel selection. Mirrors the af_spec enums but lives here so the geometry kernel can evaluate a
 * dressed body without dragging the whole ABI struct into it.
 */
struct ApparelOptions {
    int outfit = 0;      /* af_outfit      */
    int footwear = 0;    /* af_footwear    */
    int hair = 0;        /* af_hair_style  */
    /*
     * Extraction lattice the garment will be sampled on. A garment is a SOLID whose thickness is
     * the gap between the skin and the cloth, so a 4 mm layer sampled on an 11.5 mm lattice is a
     * membrane the extractor cannot represent - it produces a shredded collar of boundary edges
     * instead of a garment. The field therefore refuses to generate anything thinner than one
     * lattice cell: the caller states the lattice and gets clothing that can actually be meshed.
     */
    float latticeCell = 0.008f;
};

class BodyField {
public:
    explicit BodyField(const BodyMetrics& metrics);

    /** Trunk, arms up to the wrist, legs down to the ankle, glutes. */
    float body(const Vec3& p) const;
    /** Head, face and the top of the neck (cut plane sits below the jaw). */
    float head(const Vec3& p) const;
    /** One hand including all five fingers, cut at the wrist. */
    float hand(const Vec3& p, int side) const;
    /** One foot including toes, cut at the ankle. */
    float foot(const Vec3& p, int side) const;

    /** Dominant primitive region at a point; REGION_SKIN when nothing more specific matches. */
    int classifyRegion(const Vec3& p) const;

    const BodyMetrics& metrics() const { return m_; }
    const Chain& chain(int side) const { return side > 0 ? left_ : right_; }
    Chain& chainMutable(int side) { return side > 0 ? left_ : right_; }

    /** Y of the trunk/head cut plane and the wrist/ankle cut planes, metres. */
    /**
     * Neck cut height. It must sit ABOVE the shoulder/trapezius (so the torso's cross-section
     * at that height is only the neck) and BELOW the chin (so the jaw and chin belong to the
     * head domain). Cutting through the chin makes the torso's section there a two-component
     * shape - neck plus jaw - which fragments the seam ring; cutting through the shoulders does
     * the same at the trapezius. 42% up the neck satisfies both constraints for every build.
     */
    float neckCutY() const { return m_.y.neckBaseY + 0.42f * (m_.y.chinY - m_.y.neckBaseY); }
    float wristCutY(int side) const { return chain(side).wrist.y + 0.010f; }
    float ankleCutY(int side) const { return chain(side).ankle.y + 0.012f; }

    /** Exact bounding boxes reused by the extractor and by the renderer's frustum culling. */
    void boundsBody(Vec3* mn, Vec3* mx) const;
    void boundsHead(Vec3* mn, Vec3* mx) const;
    void boundsHand(int side, Vec3* mn, Vec3* mx) const;
    void boundsFoot(int side, Vec3* mn, Vec3* mx) const;

private:
    float torso(const Vec3& p) const;
    float torsoProfileRx(float y) const;
    float torsoProfileRz(float y) const;
    float torsoProfileCz(float y) const;
    float armChain(const Vec3& p, const Chain& c) const;
    float legChain(const Vec3& p, const Chain& c) const;
    float footShape(const Vec3& p, const Chain& c) const;
    float handShape(const Vec3& p, const Chain& c) const;
    float skull(const Vec3& p) const;
    float faceFeatures(const Vec3& p) const;

    /*
     * Facial sub-shapes.
     *
     * These are separate functions because THREE different consumers need the same shape: headShape
     * unions them into the head surface, classifyRegion() answers "which material is this point on",
     * and the texture baker reads them through the region labels. Duplicating the geometry between
     * the union and the classifier is how a face ends up with lips that are tagged as skin, so the
     * shapes live in exactly one place and everything else asks for them.
     */
    Vec3  archCentre() const;                         /* dental arch centre (shared anchor)   */
    float lipsShape(const Vec3& p) const;             /* vermilion envelope                   */
    float mouthAperture(const Vec3& p) const;         /* the opening cut through the lip wall */
    float oralCavity(const Vec3& p) const;            /* carved void behind the lips          */
    float teethShape(const Vec3& p) const;            /* upper + lower dental arches, crowns  */
    float tongueShape(const Vec3& p) const;           /* tongue body + tip, on the floor      */
    float eyeballShape(const Vec3& p, int side) const;/* sclera sphere + corneal bulge        */
    float browShape(const Vec3& p) const;             /* eyebrow band (fibre material)        */

    /*
     * Apparel.
     *
     * Every garment is built as an OFFSET SOLID of the body's own distance field, not as a
     * separately modelled mesh. `body(p) - t` is the field of the body inflated by t, so its zero
     * set is the surface of a 4 mm-thick T-shirt, a 16 mm coat or an 18 mm pauldron - and, being
     * derived from the body itself, it fits any body the generator can produce: an 8 cm offset on a
     * heavy build is the same shape as on a slim one, which no hand-modelled garment can manage.
     *
     * The garment's OUTLINE comes from intersecting the inflated body with a domain: a vertical
     * band for the hem and collar, sleeve volumes added back, and the arms carved out of the band
     * so a sleeveless garment has real armholes. The intersection uses a near-hard maximum, which
     * is what makes a hem a crisp edge rather than a rounded lip.
     */
    float clothUpper(const Vec3& p) const;
    float clothLower(const Vec3& p) const;
    float shoeShape(const Vec3& p) const;
    float gearShape(const Vec3& p) const;
    float hairShape(const Vec3& p) const;
    float sleeveSolid(const Vec3& p, float thickness, float reach) const;
    float armsCut(const Vec3& p, float thickness) const;

public:
    /** Selects the garment set. Must be called before the mesh is built. */
    void setApparel(const ApparelOptions& a) { apparel_ = a; }
    /** The thinnest layer the extraction lattice can resolve. */
    float minLayer() const { return apparel_.latticeCell; }
    const ApparelOptions& apparel() const { return apparel_; }
    /** Union of all active apparel solids (a huge positive value when nothing is worn). */
    float apparelField(const Vec3& p) const;
    /** Bounds of the apparel, for the extraction grid. Returns false when nothing is worn. */
    bool boundsApparel(Vec3* lo, Vec3* hi) const;
    /** True when the apparel changes the body's silhouette at all. */
    bool hasApparel() const { return apparel_.outfit != 0 || apparel_.footwear != 0 || apparel_.hair != 0; }

    /* Diagnostic access to the individual pieces, for probes and the region classifier. */
    float clothUpperPublic(const Vec3& p) const { return clothUpper(p); }
    float clothLowerPublic(const Vec3& p) const { return clothLower(p); }
    float shoePublic(const Vec3& p) const { return shoeShape(p); }
    float gearPublic(const Vec3& p) const { return gearShape(p); }
    float hairPublic(const Vec3& p) const { return hairShape(p); }

public:
    /*
     * Applies the oral shapes (aperture, cavity, teeth, tongue) to a value of the FINAL union
     * field. See the definition for why this cannot live inside head().
     */
    void applyOralShapes(float& d, const Vec3& p) const;

    /* Eye centres in world space. Public because the mesh builder needs to point its finest
       refinement at them; they are anatomical anchors, not internal state. */
    Vec3 eyeCentre(int side) const { return eyeCentre_[static_cast<size_t>((side > 0) ? 0 : 1)]; }
    /* Diagnostic access to the mouth shapes, so probes can verify that what was built is what the
       classifier claims. Not used by the engine path. */
    Vec3  archCentrePublic() const { return archCentre(); }
    float teethPublic(const Vec3& p) const { return teethShape(p); }
    float tonguePublic(const Vec3& p) const { return tongueShape(p); }
    float cavityPublic(const Vec3& p) const { return oralCavity(p); }

    BodyMetrics m_;
    Noise noise_{0u};
    Chain left_{}, right_{};
    Profile1D rxProfile_, rzProfile_, czProfile_;

    /* Face feature anchors (world space, derived from head landmarks in the ctor). */
    Vec3 noseTip_, noseBridge_, chin_, jawCorner_[2], cheek_[2], browRidge_;
    Vec3 eyeCentre_[2], mouthCentre_, earCentre_[2];
    ApparelOptions apparel_;
};

} // namespace af
#endif /* AF_FIELD_H */
