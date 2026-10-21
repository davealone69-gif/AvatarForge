/*
 * af_texture.cpp -- the material function and the atlas rasteriser that bakes it.
 */
#include "af_texture.h"
#include "af_threads.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace af {

const uint8_t kUnbakedRGBA[4] = {255, 0, 255, 255};

/* ==============================================================================================
 * Material table
 * ============================================================================================== */
const MaterialDef& materialForRegion(int region) {
    /* Values are physical: roughness is measured BRDF roughness, ior is the index of refraction,
       metallic is 0 for every biological tissue (only metal armour goes above 0). */
    static const MaterialDef kMaterials[13] = {
        /* SKIN            */ {"skin",        {0.80f, 0.62f, 0.55f}, 0.00f, 0.52f, 1.40f, 0.10f, 0.50f, 1.00f, 0.00f, 0.020f},
        /* EYE_L           */ {"eye_left",    {0.94f, 0.94f, 0.93f}, 0.00f, 0.07f, 1.38f, 0.00f, 0.50f, 0.35f, 0.00f, 0.006f},
        /* EYE_R           */ {"eye_right",   {0.94f, 0.94f, 0.93f}, 0.00f, 0.07f, 1.38f, 0.00f, 0.50f, 0.35f, 0.00f, 0.006f},
        /* TEETH           */ {"teeth",       {0.92f, 0.90f, 0.86f}, 0.00f, 0.25f, 1.55f, 0.00f, 0.50f, 0.60f, 0.00f, 0.010f},
        /* TONGUE          */ {"tongue",      {0.72f, 0.30f, 0.30f}, 0.00f, 0.45f, 1.36f, 0.05f, 0.50f, 1.00f, 0.00f, 0.012f},
        /* LIPS            */ {"lips",        {0.62f, 0.24f, 0.24f}, 0.00f, 0.36f, 1.38f, 0.15f, 0.50f, 1.00f, 0.00f, 0.010f},
        /* HAIR            */ {"hair",        {0.09f, 0.06f, 0.05f}, 0.00f, 0.34f, 1.55f, 0.55f, 0.62f, 0.00f, 0.85f, 0.004f},
        /* LASH            */ {"lash",        {0.04f, 0.03f, 0.03f}, 0.00f, 0.28f, 1.55f, 0.40f, 0.40f, 0.00f, 0.70f, 0.003f},
        /* NAIL            */ {"nail",        {0.88f, 0.80f, 0.78f}, 0.00f, 0.22f, 1.50f, 0.25f, 0.60f, 0.70f, 0.10f, 0.008f},
        /* CLOTH_UPPER     */ {"cloth_upper", {0.30f, 0.34f, 0.42f}, 0.00f, 0.78f, 1.45f, 0.45f, 0.55f, 0.00f, 0.30f, 0.010f},
        /* CLOTH_LOWER     */ {"cloth_lower", {0.20f, 0.22f, 0.28f}, 0.00f, 0.82f, 1.45f, 0.40f, 0.50f, 0.00f, 0.25f, 0.012f},
        /* SHOE            */ {"shoe",        {0.12f, 0.12f, 0.13f}, 0.00f, 0.55f, 1.50f, 0.20f, 0.50f, 0.00f, 0.20f, 0.008f},
        /* GEAR            */ {"gear",        {0.62f, 0.60f, 0.55f}, 0.85f, 0.35f, 2.20f, 0.05f, 0.50f, 0.00f, 0.15f, 0.014f},
    };
    if (region < 0 || region >= 13) return kMaterials[0];
    return kMaterials[region];
}

uint64_t textureSetBytes(const TextureSet& set) {
    return static_cast<uint64_t>(set.baseColor.size()) + set.normal.size() + set.metallicRoughness.size() +
           set.occlusion.size() + set.subsurface.size();
}

uint64_t atlasBakeWorkspaceBytes(int resolution) {
    if (resolution <= 0) return 0;
    const uint64_t side = static_cast<uint64_t>(resolution);
    if (side > std::numeric_limits<uint64_t>::max() / side / 19ull)
        return std::numeric_limits<uint64_t>::max();
    /* Base/normal/MR/subsurface are RGBA8 (16 B/texel), AO is R8 (1 B), and the raster mask plus
       its dilation copy add two more bytes at peak. */
    return side * side * 19ull;
}

int atlasResolutionForBudget(uint64_t budgetBytes, uint64_t retainedBytes) {
    if (retainedBytes >= budgetBytes) return 0;
    const uint64_t available = budgetBytes - retainedBytes;
    for (int resolution : {2048, 1024, 512, 256}) {
        if (atlasBakeWorkspaceBytes(resolution) <= available) return resolution;
    }
    return 0;
}

/* ==============================================================================================
 * Helpers
 * ============================================================================================== */
namespace {

/* Deterministic value noise over 3D positions (the same Noise class the field uses, so the
   texture and the geometry agree on micro-detail phase). */
float fbm(const Noise& n, const Vec3& p, int octaves, float lacunarity = 2.07f, float gain = 0.5f) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += n.value(p * freq) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/** Cellular-ish pore noise: distance to the nearest jittered lattice point. */
float poreNoise(const Noise& n, const Vec3& p, float cell) {
    const Vec3 q = p / cell;
    const int ix = static_cast<int>(std::floor(q.x)), iy = static_cast<int>(std::floor(q.y)), iz = static_cast<int>(std::floor(q.z));
    float best = 1e9f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int gx = ix + dx, gy = iy + dy, gz = iz + dz;
                /* Three decorrelated hashes from the seeded noise, so the pore lattice is
                   stable for a given avatar seed but different between seeds. */
                const Vec3 jitter(n.hash3f(gx, gy, gz),
                                  n.hash3f(gx + 7919, gy - 104729, gz + 15485863),
                                  n.hash3f(gx - 32452843, gy + 49979687, gz - 67867967));
                const Vec3 c(static_cast<float>(gx) + jitter.x, static_cast<float>(gy) + jitter.y,
                             static_cast<float>(gz) + jitter.z);
                best = std::min(best, static_cast<float>((q - c).lengthSq()));
            }
        }
    }
    return std::sqrt(std::max(0.0f, best));
}

/** Smooth ellipsoidal mask in body space, used for blush / tanning / wear. */
float mask3(const Vec3& p, const Vec3& c, const Vec3& r) {
    const float dx = (p.x - c.x) / r.x, dy = (p.y - c.y) / r.y, dz = (p.z - c.z) / r.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d >= 1.0f) return 0.0f;
    const float t = 1.0f - d;
    return t * t * (3.0f - 2.0f * t);
}

/** The body's medial axis, used as a thickness proxy for subsurface scattering. */
struct MedialAxis {
    std::vector<std::pair<Vec3, Vec3>> segments;
    void add(const Vec3& a, const Vec3& b) { segments.emplace_back(a, b); }
    float distance(const Vec3& p) const {
        float best = 1e9f;
        for (const auto& s : segments) {
            const Vec3 ab = s.second - s.first;
            const float den = ab.lengthSq();
            const float t = den < 1e-9f ? 0.0f : clampf((p - s.first).dot(ab) / den, 0.0f, 1.0f);
            best = std::min(best, static_cast<float>((p - (s.first + ab * t)).length()));
        }
        return best == 1e9f ? 0.25f : best;
    }
};

MedialAxis buildMedialAxis(const BodyMetrics& m, const Chain chains[2]) {
    MedialAxis ax;
    const float cx = 0.0f;
    ax.add(Vec3(cx, m.y.crotchY, m.hip.centreZ), Vec3(cx, m.y.waistY, m.waist.centreZ));
    ax.add(Vec3(cx, m.y.waistY, m.waist.centreZ), Vec3(cx, m.y.neckBaseY, m.neck.centreZ));
    ax.add(Vec3(cx, m.y.neckBaseY, m.neck.centreZ), Vec3(cx, m.y.crownY, 0.0f));
    for (int s = 0; s < 2; ++s) {
        const Chain& c = chains[s];
        ax.add(c.shoulder, c.elbow);
        ax.add(c.elbow, c.wrist);
        for (int f = 0; f < 5; ++f) {
            ax.add(c.wrist, c.fingerJoints[f][0]);
            ax.add(c.fingerJoints[f][0], c.fingerJoints[f][2]);
        }
        ax.add(c.hipJoint, c.knee);
        ax.add(c.knee, c.ankle);
        ax.add(c.ankle, c.ball);
    }
    return ax;
}

/** Surface description handed to the material function. */
struct SurfaceSample {
    Vec3  position{0, 0, 0};
    Vec3  normal{0, 1, 0};
    Vec3  tangent{1, 0, 0};
    Vec3  bitangent{0, 0, 1};
    float curvature = 1.0f;
    float thickness = 0.3f;
    int   region = REGION_SKIN;
};

struct SurfaceOut {
    Vec3  albedo{0.8f, 0.8f, 0.8f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float occlusion = 1.0f;
    float thickness = 0.3f;
    float scatterRadius = 0.3f;
    float tint = 0.0f;
    float anisotropyHint = 0.0f;     /* >0 = strand/fibre material (hair, brushed metal) */
    Vec3  reliefGradient{0, 0, 0};   /* d(height)/d(surface) from the detail field */
};

/* ------------------------------------------------------------------ per-region materials ---- */

struct MaterialContext {
    const BodyMetrics* m = nullptr;
    const af_spec* spec = nullptr;
    Noise noise;
    MedialAxis axis;
    Chain chains[2];              /* [0] = +X (model's left), [1] = -X                  */
    Vec3 skinAlbedo{0.8f, 0.6f, 0.52f};
    Vec3 undertone{1.0f, 0.62f, 0.52f};
    Vec3 eyeCentre[2];
    float eyeRadius = 0.008f;
    float ageFactor = 0.0f;
};

void evaluateSkin(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    const BodyMetrics& m = *ctx.m;
    const Noise& n = ctx.noise;
    const Vec3 p = s.position;

    /* Base pigmentation: melanin from the spec, modulated by undertone and by a large-scale
       variation that follows the body's vascular map. */
    Vec3 albedo = ctx.skinAlbedo;
    const float macro = fbm(n, p * 6.0f, 3) * 0.06f;
    albedo = albedo * (1.0f + macro);

    /* Erythema: cheeks, nose, ears, elbows, knees, knuckles, and the palms/soles slightly
       deeper. All are anatomical, positioned from the solved landmarks. */
    float blush = 0.0f;
    blush += mask3(p, Vec3(m.faceWidth * 0.32f, m.mouthY + (m.y.crownY - m.y.chinY) * 0.075f, -m.head.rz * 0.82f),
                   Vec3(m.faceWidth * 0.24f, m.head.rz * 0.36f, m.head.rz * 0.42f)) * 0.9f;   /* left cheek */
    blush += mask3(p, Vec3(-m.faceWidth * 0.32f, m.mouthY + (m.y.crownY - m.y.chinY) * 0.075f, -m.head.rz * 0.82f),
                   Vec3(m.faceWidth * 0.24f, m.head.rz * 0.36f, m.head.rz * 0.42f)) * 0.9f;   /* right cheek */
    blush += mask3(p, Vec3(0, m.mouthY + (m.y.crownY - m.y.chinY) * 0.115f, -m.head.rz * 1.00f),
                   Vec3(m.faceWidth * 0.16f, m.head.rz * 0.30f, m.head.rz * 0.40f)) * 0.8f;   /* nose */
    for (int s2 = 0; s2 < 2; ++s2) {
        const Chain& c = ctx.m ? ctx.chains[s2] : ctx.chains[s2];
        blush += mask3(p, (c.hipJoint + c.knee) * 0.5f + Vec3(c.knee.x - c.hipJoint.x, 0, c.knee.z - c.hipJoint.z) * 0.18f,
                       Vec3(0.055f, 0.045f, 0.055f)) * 0.5f;                                     /* knees */
        blush += mask3(p, c.elbow, Vec3(0.035f, 0.035f, 0.035f)) * 0.55f;                     /* elbows */
        blush += mask3(p, c.ball, Vec3(0.06f, 0.03f, 0.10f)) * 0.35f;                         /* balls of feet */
    }
    blush += mask3(p, Vec3(0, m.y.crownY - (m.y.crownY - m.y.chinY) * 0.10f, m.head.centreZ * 0.5f),
                   Vec3(m.head.rx * 1.05f, m.head.rz * 0.35f, m.head.rz * 0.95f)) * 0.30f;       /* scalp */
    blush = saturate(blush);
    const float blushNoise = 0.75f + 0.5f * fbm(n, p * 22.0f, 3);
    const float erythema = blush * 0.22f * blushNoise;
    albedo.x = saturate(albedo.x + erythema * 0.55f);
    albedo.y = saturate(albedo.y - erythema * 0.06f);
    albedo.z = saturate(albedo.z - erythema * 0.10f);

    /* Freckles and naevi: sparse, denser on sun-exposed skin. */
    const float freckleField = fbm(n, p * 90.0f + Vec3(3.1f, 7.7f, 1.3f), 2);
    const float exposure = blush * 0.8f + 0.35f;
    const float freckle = saturate((freckleField - 0.62f) * 6.0f) * exposure * (0.25f + 0.55f * ctx.spec->skin_melanin);
    albedo = albedo.lerp(albedo * Vec3(0.52f, 0.34f, 0.26f) * 1.25f, saturate(freckle * 0.9f));

    /* Age: gradual desaturation, more visible veins and creases over time. */
    const float age = ctx.ageFactor;
    albedo = albedo.lerp(Vec3(dot(albedo, Vec3(0.333f, 0.333f, 0.333f)) * 1.02f), age * 0.18f);

    /* Capillary detail in thin skin (temples, hands, lower legs) - fine darker lines. */
    const float veins = saturate((fbm(n, p * 42.0f + Vec3(11.0f, 0.0f, 5.0f), 3, 2.3f, 0.55f) - 0.55f) * 5.0f);
    const float thinSkin = saturate(mask3(p, Vec3(0, m.y.crownY - (m.y.crownY - m.y.chinY) * 0.20f, -m.head.rz * 0.4f),
                                          Vec3(m.head.rx * 1.1f, m.head.rz * 0.5f, m.head.rz * 0.9f)) + blush * 0.4f);
    const Vec3 veinTint(0.72f, 0.66f, 0.74f);
    albedo = albedo.lerp(albedo * veinTint * 1.05f, veins * thinSkin * 0.18f);

    /* Lips are a distinct material but share the skin mesh's boundary; the region label handles
       the switch, so nothing extra is needed here. */

    out->albedo = albedo;
    out->roughness = saturate(0.62f - 0.10f * ctx.spec->body_fat + 0.10f * age - 0.06f * s.curvature);
    out->metallic = 0.0f;
    out->thickness = s.thickness;
    out->scatterRadius = saturate(s.thickness * 1.6f);
    out->tint = ctx.spec->skin_melanin;
    /* Occlusion: crevices darken, and the belt/bust/armpit contact regions darken further. */
    out->occlusion = saturate(0.55f + 0.55f * s.curvature);
    out->occlusion *= 1.0f - 0.18f * saturate(mask3(p, Vec3(0, m.y.waistY - 0.10f, 0), Vec3(0.22f, 0.06f, 0.16f)));
    out->occlusion *= 1.0f - 0.20f * saturate(mask3(p, Vec3(m.bustSep * 0.5f, m.y.bustY - 0.02f, -m.chest.rz * 0.8f),
                                                    Vec3(0.10f, 0.09f, 0.10f)));
    out->occlusion = saturate(out->occlusion);

    /* Micro-relief gradient: pores + fine creases. 0.4 mm pores on the face, coarser elsewhere. */
    if (s.region == REGION_SKIN) {
        const float distToHead = (p - Vec3(0, m.y.chinY + (m.y.crownY - m.y.chinY) * 0.45f, 0)).length();
        const bool face = distToHead < (m.y.crownY - m.y.chinY) * 0.95f && p.z < -m.head.rz * 0.35f;
        const float poreCell = face ? 0.00055f : 0.0011f;
        const float poreDepth = face ? 0.00022f : 0.00016f;
        const float pores = poreNoise(n, p, poreCell);
        const float crease = fbm(n, p * 130.0f, 3);
        /* Joint creases: grooves perpendicular to the limb axis near elbows/knees. */
        float creaseAmp = 0.0f;
        for (int s2 = 0; s2 < 2; ++s2) {
            const Chain& c = ctx.chains[s2];
            creaseAmp += mask3(p, c.elbow, Vec3(0.045f, 0.045f, 0.045f)) * 0.6f;
            creaseAmp += mask3(p, c.knee, Vec3(0.055f, 0.055f, 0.055f)) * 0.5f;
            creaseAmp += mask3(p, c.wrist, Vec3(0.035f, 0.035f, 0.035f)) * 0.55f;
            creaseAmp += mask3(p, c.ball, Vec3(0.05f, 0.03f, 0.09f)) * 0.4f;
        }
        creaseAmp += mask3(p, Vec3(0, m.mouthY + (m.y.crownY - m.y.chinY) * 0.055f, -m.head.rz * 0.95f),
                           Vec3(m.faceWidth * 0.55f, m.head.rz * 0.13f, m.head.rz * 0.5f)) * 0.35f;  /* nasolabial */
        creaseAmp += (age * 0.7f) * mask3(p, Vec3(0, m.y.crownY - (m.y.crownY - m.y.chinY) * 0.13f, -m.head.rz * 0.85f),
                                          Vec3(m.head.rx * 1.0f, m.head.rz * 0.25f, m.head.rz * 0.6f));  /* forehead lines */
        const float height = -poreDepth * (1.0f - saturate(pores * 2.2f)) - creaseAmp * (0.00035f + age * 0.0005f) *
                                                                              (0.4f + 0.6f * saturate(crease));
        /* Analytic gradient of the relief times the surface frame, so the normal map is
           metric-correct regardless of texel density. */
        const float eps = 0.0006f;
        auto h = [&](const Vec3& q) {
            const float po = poreNoise(n, q, poreCell);
            const float cr = fbm(n, q * 130.0f, 3);
            return -poreDepth * (1.0f - saturate(po * 2.2f)) -
                   creaseAmp * (0.00035f + age * 0.0005f) * (0.4f + 0.6f * saturate(cr));
        };
        const float h0 = h(p);
        (void)height;
        const float hu = h(p + s.tangent * eps);
        const float hv = h(p + s.bitangent * eps);
        out->reliefGradient = Vec3((hu - h0) / eps, (hv - h0) / eps, 0.0f);
    }
}

void evaluateEye(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    const int which = (s.region == REGION_EYE_L) ? 0 : 1;
    const Vec3 centre = ctx.eyeCentre[which];
    const float r = ctx.eyeRadius;
    /* Iris geometry is defined in 3D about the optical axis (-Z), so the pattern is correct
       whatever the UV layout does. */
    const Vec3 axis = Vec3((which == 0 ? 1.0f : -1.0f) * std::tan(radians(1.2f)), 0.0f, -1.0f).normalized();
    const Vec3 rel = s.position - centre;
    const float along = rel.dot(axis);
    const Vec3 radialVec = rel - axis * along;
    const float radial = radialVec.length();
    const float rIris = r * 1.62f;
    const float rPupil = rIris * 0.36f;

    Vec3 albedo;
    float roughness;
    if (along < -r * 0.2f) {
        albedo = Vec3(0.02f, 0.02f, 0.02f);       /* inside the globe / cup: not visible */
        roughness = 0.5f;
    } else if (radial < rPupil) {
        albedo = Vec3(0.008f, 0.007f, 0.008f);    /* pupil */
        roughness = 0.06f;
    } else if (radial < rIris) {
        /* Iris: radial fibre striations around the average iris colour, darker at the limbus. */
        const float angle = std::atan2(radialVec.dot(s.bitangent), radialVec.dot(s.tangent));
        const float t = (radial - rPupil) / std::max(1e-6f, rIris - rPupil);
        /* Iris colour is not a separate spec field: it is derived from the same melanin that
           drives skin and hair, plus a deterministic per-avatar hue shift, which is what the
           photogrammetry literature reports as the real population correlation (light eyes
           appear with low melanin, and the hue varies by a few degrees between individuals). */
        const float hueShift = ctx.noise.hash3f(101, which, 7) - 0.5f;
        const float eumelanin = ctx.spec->skin_melanin;
        const Vec3 base = Vec3(0.34f + 0.30f * (1.0f - eumelanin) + hueShift * 0.06f,
                               0.30f + 0.28f * (1.0f - eumelanin) + hueShift * 0.03f,
                               0.20f + 0.34f * (1.0f - eumelanin) - hueShift * 0.02f).lerp(
            Vec3(0.20f, 0.11f, 0.05f), saturate(eumelanin * 1.25f));
        const float fibres = fbm(ctx.noise, Vec3(std::cos(angle), std::sin(angle), t * 6.0f) * 40.0f, 3, 2.5f, 0.55f);
        const float limbal = saturate((t - 0.72f) / 0.28f);
        const float collarette = 1.0f - saturate(std::fabs(t - 0.48f) * 5.0f);
        albedo = base * (0.72f + 0.55f * fibres);
        albedo = albedo.lerp(base * Vec3(0.45f, 0.42f, 0.40f), limbal * 0.85f);
        albedo = albedo * (1.0f + collarette * 0.22f);
        albedo = albedo.lerp(Vec3(0.02f, 0.02f, 0.025f), saturate((rPupil * 1.15f - radial) / (rPupil * 0.3f)));
        roughness = 0.18f;
    } else {
        /* Sclera: off-white with limbic and episcleral vessels that thicken towards rotation. */
        const float t = saturate((radial - rIris) / (r * 1.6f));
        albedo = Vec3(0.90f, 0.88f, 0.86f) * (1.0f - 0.10f * t);
        const float veins = saturate((fbm(ctx.noise, rel * 260.0f, 3, 2.4f, 0.55f) - 0.52f) * 7.0f);
        const float veinDensity = saturate(t * 1.4f);
        albedo = albedo.lerp(Vec3(0.70f, 0.22f, 0.22f), veins * veinDensity * 0.55f);
        roughness = 0.12f;
    }
    out->albedo = albedo;
    out->roughness = roughness;
    out->metallic = 0.0f;
    out->thickness = 0.15f;
    out->scatterRadius = 0.05f;
    out->tint = 0.0f;
    out->occlusion = 1.0f;
}

void evaluateTeeth(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    /* Teeth are modelled as a solid block in this rig; the tooth-by-tooth variation is baked in
       so the surface reads as individual teeth under lighting. */
    const float x = (s.position.x + ctx.m->mouthWidth * 0.5f) / std::max(1e-5f, ctx.m->mouthWidth);
    const int index = static_cast<int>(std::floor(x * 16.0f));
    const float toothVar = (static_cast<float>((index * 2654435761u) % 100u) / 100.0f - 0.5f) * 0.10f;
    const float gum = saturate((s.position.y - ctx.m->mouthY) / 0.012f);   /* upper gum line */
    Vec3 albedo = Vec3(0.93f, 0.91f, 0.87f) * (1.0f + toothVar);
    albedo = albedo.lerp(Vec3(0.62f, 0.30f, 0.30f), saturate(1.0f - gum) * 0.75f);
    const float stains = saturate((fbm(ctx.noise, s.position * 400.0f, 2) - 0.55f) * 6.0f);
    albedo = albedo.lerp(Vec3(0.78f, 0.70f, 0.52f), stains * 0.22f);
    out->albedo = albedo;
    out->roughness = 0.22f;
    out->thickness = 0.02f;
    out->scatterRadius = 0.012f;
    out->occlusion = saturate(0.6f + 0.5f * s.curvature);
}

void evaluateTongue(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    const float papillae = saturate((fbm(ctx.noise, s.position * 300.0f, 3, 2.2f, 0.6f) - 0.45f) * 3.0f);
    out->albedo = Vec3(0.66f, 0.26f, 0.26f) * (0.85f + 0.3f * papillae);
    out->roughness = 0.42f - 0.10f * papillae;
    out->thickness = 0.02f;
    out->scatterRadius = 0.02f;
    out->occlusion = saturate(0.65f + 0.4f * s.curvature);
}

void evaluateLips(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    const BodyMetrics& m = *ctx.m;
    const float t = s.position.y - m.mouthY;
    const float halfH = m.lipWidth * 0.42f;
    const float lipT = saturate(1.0f - std::fabs(t) / std::max(1e-5f, halfH));
    Vec3 albedo = ctx.skinAlbedo.lerp(Vec3(0.66f, 0.24f, 0.24f), 0.65f + 0.3f * lipT);
    /* Vermilion border and vertical lip lines */
    const float lines = saturate((fbm(ctx.noise, Vec3(s.position.x * 420.0f, s.position.y * 60.0f, s.position.z * 60.0f), 2) - 0.5f) * 5.0f);
    albedo = albedo * (1.0f - 0.12f * lines * lipT);
    albedo = albedo * (1.0f - 0.25f * saturate(std::fabs(t) / halfH - 0.55f));   /* darker vermilion edge */
    out->albedo = albedo;
    out->roughness = 0.30f - 0.08f * lipT;
    out->thickness = 0.010f;
    out->scatterRadius = 0.012f;
    out->tint = 0.4f;
    out->occlusion = saturate(0.55f + 0.5f * s.curvature);
}

void evaluateNail(const MaterialContext&, const SurfaceSample& s, SurfaceOut* out) {
    out->albedo = Vec3(0.86f, 0.78f, 0.76f) * (0.94f + 0.06f * saturate(s.curvature));
    out->roughness = 0.16f;
    out->thickness = 0.004f;
    out->scatterRadius = 0.006f;
    out->occlusion = saturate(0.7f + 0.35f * s.curvature);
}

void evaluateCloth(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out, bool upper,
                   const MaterialDef& mat) {
    /* A woven twill: two interleaved thread systems. The pattern is aligned to the body frame
       (world X/Z for horizontal fabrics) which keeps the weave coherent across the torso. */
    const float yarn = 0.0035f;
    const Vec3 q = s.position / yarn;
    const float warp = std::sin(q.x * 6.2831853f) * 0.5f + 0.5f;
    const float weft = std::sin(q.z * 6.2831853f) * 0.5f + 0.5f;
    const float twill = (warp + weft) * 0.5f;
    const float fuzz = fbm(ctx.noise, s.position * 900.0f, 2, 2.1f, 0.5f);
    Vec3 albedo = mat.baseColor * (0.82f + 0.30f * twill) * (0.94f + 0.12f * fuzz);
    /* Dye variation and worn edges. */
    albedo *= 1.0f + 0.06f * fbm(ctx.noise, s.position * 8.0f, 3);
    out->albedo = albedo;
    out->roughness = saturate(mat.roughness - 0.10f * twill + 0.05f * fuzz);
    out->metallic = 0.0f;
    out->thickness = 0.008f;
    out->scatterRadius = 0.004f;
    out->tint = 0.0f;
    out->occlusion = saturate(0.5f + 0.6f * s.curvature);
    /* Weave relief: a sinusoidal height field, gradient computed analytically. */
    const float amp = 0.00035f;
    out->reliefGradient = Vec3(std::cos(q.x * 6.2831853f) * amp * 6.2831853f / yarn,
                               std::cos(q.z * 6.2831853f) * amp * 6.2831853f / yarn * 0.5f, 0.0f);
    (void)upper;
}

void evaluateShoe(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    /* Full-grain leather: pebbled cellular grain on the upper, a fine rubber tread on the sole. */
    const bool sole = s.position.y < 0.02f;
    if (sole) {
        const float tread = fbm(ctx.noise, Vec3(s.position.x * 90.0f, s.position.y * 90.0f, s.position.z * 90.0f), 3, 2.2f, 0.55f);
        out->albedo = Vec3(0.055f, 0.055f, 0.06f) * (0.85f + 0.3f * saturate(tread));
        out->roughness = 0.72f;
        out->reliefGradient = Vec3(0, 0, 0);
    } else {
        const float grain = 1.0f - saturate(poreNoise(ctx.noise, s.position, 0.0016f) * 3.0f);
        const float creases = saturate((fbm(ctx.noise, s.position * 45.0f, 3, 2.3f, 0.55f) - 0.5f) * 4.0f);
        out->albedo = Vec3(0.11f, 0.10f, 0.10f) * (0.82f + 0.35f * grain) * (1.0f - 0.25f * creases);
        out->roughness = saturate(0.42f + 0.22f * creases - 0.08f * grain);
        out->reliefGradient = Vec3(grain * 0.9f, creases * 1.2f, 0.0f);
    }
    out->metallic = 0.0f;
    out->thickness = 0.006f;
    out->scatterRadius = 0.0f;
    out->occlusion = saturate(0.45f + 0.6f * s.curvature);
}

void evaluateGear(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    /* Cast steel: work-hardened plates with a hammered surface and wear along the edges where
       curvature is high. */
    const float hammer = fbm(ctx.noise, s.position * 55.0f, 4, 2.1f, 0.5f);
    const float scratches = saturate((fbm(ctx.noise, Vec3(s.position.x * 350.0f, s.position.y * 12.0f, s.position.z * 350.0f), 2) - 0.5f) * 6.0f);
    const float edgeWear = 1.0f - s.curvature;
    Vec3 albedo = Vec3(0.60f, 0.58f, 0.54f) * (0.86f + 0.22f * hammer);
    albedo = albedo.lerp(Vec3(0.78f, 0.76f, 0.72f), scratches * 0.35f);
    out->albedo = albedo;
    out->roughness = saturate(0.30f + 0.30f * saturate(hammer) + 0.25f * edgeWear - 0.10f * scratches);
    out->metallic = 1.0f;
    out->thickness = 0.004f;
    out->scatterRadius = 0.0f;
    out->occlusion = saturate(0.5f + 0.55f * s.curvature);
    out->reliefGradient = Vec3(hammer * 1.5f, hammer * -1.1f, 0.0f);
}

void evaluateSurface(const MaterialContext& ctx, const SurfaceSample& s, SurfaceOut* out) {
    const MaterialDef& mat = materialForRegion(s.region);
    out->albedo = mat.baseColor;
    out->roughness = mat.roughness;
    out->metallic = mat.metallic;
    out->occlusion = 1.0f;
    out->thickness = 0.02f;
    out->scatterRadius = mat.subsurface * 0.02f;
    out->tint = 0.0f;
    out->reliefGradient = Vec3(0, 0, 0);

    switch (s.region) {
        case REGION_EYE_L: case REGION_EYE_R: evaluateEye(ctx, s, out); break;
        case REGION_TEETH:                    evaluateTeeth(ctx, s, out); break;
        case REGION_TONGUE:                   evaluateTongue(ctx, s, out); break;
        case REGION_LIPS:                     evaluateLips(ctx, s, out); break;
        case REGION_NAIL:                     evaluateNail(ctx, s, out); break;
        case REGION_CLOTH_UPPER:              evaluateCloth(ctx, s, out, true, mat); break;
        case REGION_CLOTH_LOWER:              evaluateCloth(ctx, s, out, false, mat); break;
        case REGION_SHOE:                     evaluateShoe(ctx, s, out); break;
        case REGION_GEAR:                     evaluateGear(ctx, s, out); break;
        case REGION_HAIR: case REGION_LASH:   /* shells bake their own maps; the body bake writes a neutral strand colour */
            out->albedo = mat.baseColor;
            out->roughness = mat.roughness;
            out->anisotropyHint = 1.0f;
            break;
        default:                              evaluateSkin(ctx, s, out); break;
    }
}

} // namespace

/* ==============================================================================================
 * Bake
 * ============================================================================================== */
namespace {
struct RasterTarget {
    int w = 0, h = 0;
    std::vector<uint8_t> filled;
};
} // namespace

bool bakeTextures(const Mesh& mesh, const UVLayout& layout, const BodyMetrics& metrics,
                  const af_spec& spec, const BakeOptions& options, TextureSet* out) {
    if (!out || mesh.uvs.size() < mesh.vertexCount() * 2 || mesh.indices.size() < 3) return false;
    const int w = std::max(64, options.width);
    const int h = std::max(64, options.height);

    out->width = w;
    out->height = h;
    out->baseColor.assign(static_cast<size_t>(w) * h * 4, 0);
    out->normal.assign(static_cast<size_t>(w) * h * 4, 0);
    out->metallicRoughness.assign(static_cast<size_t>(w) * h * 4, 0);
    out->occlusion.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    out->subsurface.assign(static_cast<size_t>(w) * h * 4, 0);
    std::vector<uint8_t> filled(static_cast<size_t>(w) * h, 0);

    MaterialContext ctx;
    ctx.m = &metrics;
    ctx.spec = &spec;
    ctx.noise = Noise(static_cast<uint32_t>(spec.seed) * 2654435761u + 101u);
    ctx.undertone = Vec3(1.0f, 0.72f - 0.25f * spec.skin_melanin, 0.60f - 0.30f * spec.skin_melanin);
    ctx.skinAlbedo = skinAlbedoFromMelanin(spec.skin_melanin, ctx.undertone);
    ctx.ageFactor = saturate((spec.age_years - 18.0f) / 60.0f);
    ctx.eyeRadius = metrics.eyeRadius;
    ctx.eyeCentre[0] = Vec3(metrics.eyeSeparation * 0.5f, metrics.y.eyeY, metrics.eyeZ);
    ctx.eyeCentre[1] = Vec3(-metrics.eyeSeparation * 0.5f, metrics.y.eyeY, metrics.eyeZ);
    /* Chains for landmark features. They are the same arm/leg chains the geometry was built
       from, obtained by rebuilding the (cheap, analytic) field for this metric set. */
    {
        const BodyField fieldForChains(metrics);
        ctx.chains[0] = fieldForChains.chain(1);
        ctx.chains[1] = fieldForChains.chain(-1);
    }
    ctx.axis = buildMedialAxis(metrics, ctx.chains);

    /* Rasterise every triangle of every chart. Parallel over charts, then a serial dilation. */
    const int threads = options.threads > 0 ? options.threads : defaultThreadCount();
    const int chartCount = static_cast<int>(layout.charts.size());
    const size_t faceCount = mesh.triangleCount();

    parallelFor(chartCount, threads, [&](int c0, int c1) {
        for (int ci = c0; ci < c1; ++ci) {
            const UVChart& chart = layout.charts[static_cast<size_t>(ci)];
            for (int32_t i = 0; i < chart.faceCount; ++i) {
                const int32_t face = layout.faces[static_cast<size_t>(chart.firstFace + i)];
                if (face < 0 || static_cast<size_t>(face) >= faceCount) continue;
                const uint32_t idx[3] = {mesh.indices[static_cast<size_t>(face) * 3],
                                         mesh.indices[static_cast<size_t>(face) * 3 + 1],
                                         mesh.indices[static_cast<size_t>(face) * 3 + 2]};
                Vec2 uv[3];
                Vec3 pos[3], nrm[3], tan[3], bit[3];
                float curv[3];
                int region[3];
                for (int k = 0; k < 3; ++k) {
                    uv[k] = Vec2(mesh.uvs[idx[k] * 2], mesh.uvs[idx[k] * 2 + 1]);
                    pos[k] = mesh.positions[idx[k]];
                    nrm[k] = idx[k] < mesh.normals.size() ? mesh.normals[idx[k]] : Vec3(0, 1, 0);
                    const Vec4 t = idx[k] < mesh.tangents.size() ? mesh.tangents[idx[k]] : Vec4(1, 0, 0, 1);
                    tan[k] = t.xyz();
                    bit[k] = nrm[k].cross(tan[k]) * t.w;
                    curv[k] = idx[k] < mesh.curvature.size() ? mesh.curvature[idx[k]] : 1.0f;
                    region[k] = idx[k] < mesh.regions.size() ? mesh.regions[idx[k]] : REGION_SKIN;
                }
                const float minU = minf(uv[0].x, minf(uv[1].x, uv[2].x));
                const float maxU = maxf(uv[0].x, maxf(uv[1].x, uv[2].x));
                const float minV = minf(uv[0].y, minf(uv[1].y, uv[2].y));
                const float maxV = maxf(uv[0].y, maxf(uv[1].y, uv[2].y));
                int x0 = maxi(0, static_cast<int>(std::floor(minU * static_cast<float>(w))));
                int x1 = mini(w - 1, static_cast<int>(std::ceil(maxU * static_cast<float>(w))));
                int y0 = maxi(0, static_cast<int>(std::floor(minV * static_cast<float>(h))));
                int y1 = mini(h - 1, static_cast<int>(std::ceil(maxV * static_cast<float>(h))));
                /* Degenerate UVs (zero area) happen on sliver triangles; skip them rather than
                   divide by zero - the dilation pass covers the texels they would have written. */
                const float d = (uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) - (uv[2].x - uv[0].x) * (uv[1].y - uv[0].y);
                if (std::fabs(d) < 1e-12f) continue;
                const float invD = 1.0f / d;

                for (int py = y0; py <= y1; ++py) {
                    for (int px = x0; px <= x1; ++px) {
                        const float su = (static_cast<float>(px) + 0.5f) / static_cast<float>(w);
                        const float sv = (static_cast<float>(py) + 0.5f) / static_cast<float>(h);
                        const float l0 = ((uv[1].x - su) * (uv[2].y - sv) - (uv[2].x - su) * (uv[1].y - sv)) * invD;
                        const float l1 = ((uv[2].x - su) * (uv[0].y - sv) - (uv[0].x - su) * (uv[2].y - sv)) * invD;
                        const float l2 = 1.0f - l0 - l1;
                        /* Half-texel tolerance keeps chart borders watertight. */
                        const float tol = -0.0015f;
                        if (l0 < tol || l1 < tol || l2 < tol) continue;

                        SurfaceSample s;
                        s.position = pos[0] * l0 + pos[1] * l1 + pos[2] * l2;
                        s.normal = (nrm[0] * l0 + nrm[1] * l1 + nrm[2] * l2).normalizedOr({0, 1, 0});
                        s.tangent = (tan[0] * l0 + tan[1] * l1 + tan[2] * l2).normalizedOr({1, 0, 0});
                        s.bitangent = (bit[0] * l0 + bit[1] * l1 + bit[2] * l2).normalizedOr({0, 0, 1});
                        s.curvature = l0 * curv[0] + l1 * curv[1] + l2 * curv[2];
                        /* Region: majority of the three vertices, so charts stay single-material. */
                        s.region = (l0 > l1) ? ((l0 > l2) ? region[0] : region[2]) : ((l1 > l2) ? region[1] : region[2]);
                        s.thickness = ctx.axis.distance(s.position);

                        SurfaceOut so;
                        evaluateSurface(ctx, s, &so);

                        /* Tangent-space normal from the relief gradient: n = normalize(-dh/du, -dh/dv, 1). */
                        Vec3 nts(0, 0, 1);
                        if (options.bakeMicroRelief) {
                            nts = Vec3(-so.reliefGradient.x, -so.reliefGradient.y, 1.0f).normalizedOr({0, 0, 1});
                        }
                        const size_t t = (static_cast<size_t>(py) * w + px);
                        uint8_t* bc = &out->baseColor[t * 4];
                        uint8_t* nm = &out->normal[t * 4];
                        uint8_t* mr = &out->metallicRoughness[t * 4];
                        uint8_t* ss = &out->subsurface[t * 4];
                        /* Albedo is stored in sRGB (the renderer linearises on sample) while the
                           physical parameters stay linear - the same convention glTF uses. */
                        const Vec3 albedoClamped(vmin(vmax(so.albedo, Vec3(0, 0, 0)), Vec3(1, 1, 1)));
                        const Vec3 srgb = linearToSrgb(albedoClamped);
                        bc[0] = static_cast<uint8_t>(saturate(srgb.x) * 255.0f + 0.5f);
                        bc[1] = static_cast<uint8_t>(saturate(srgb.y) * 255.0f + 0.5f);
                        bc[2] = static_cast<uint8_t>(saturate(srgb.z) * 255.0f + 0.5f);
                        bc[3] = 255;
                        nm[0] = static_cast<uint8_t>(saturate(nts.x * 0.5f + 0.5f) * 255.0f + 0.5f);
                        nm[1] = static_cast<uint8_t>(saturate(nts.y * 0.5f + 0.5f) * 255.0f + 0.5f);
                        nm[2] = static_cast<uint8_t>(saturate(nts.z * 0.5f + 0.5f) * 255.0f + 0.5f);
                        nm[3] = 255;
                        mr[0] = 255;
                        mr[1] = static_cast<uint8_t>(saturate(so.roughness) * 255.0f + 0.5f);
                        mr[2] = static_cast<uint8_t>(saturate(so.metallic) * 255.0f + 0.5f);
                        mr[3] = 255;
                        out->occlusion[t] = static_cast<uint8_t>(saturate(so.occlusion) * 255.0f + 0.5f);
                        ss[0] = static_cast<uint8_t>(saturate(so.thickness * 4.0f) * 255.0f + 0.5f);
                        ss[1] = static_cast<uint8_t>(saturate(so.scatterRadius * 4.0f) * 255.0f + 0.5f);
                        ss[2] = static_cast<uint8_t>(saturate(so.tint) * 255.0f + 0.5f);
                        ss[3] = 255;
                        filled[t] = 1;
                    }
                }
            }
        }
    });

    /* Dilation: grow every filled texel outwards so mip-mapping and bilinear filtering at chart
       borders never sample un-baked (magenta) texels. */
    for (int pass = 0; pass < options.dilationPasses; ++pass) {
        std::vector<uint8_t> next = filled;
        const size_t span = static_cast<size_t>(w) * h;
        for (int py = 0; py < h; ++py) {
            for (int px = 0; px < w; ++px) {
                const size_t t = static_cast<size_t>(py) * w + px;
                if (filled[t]) continue;
                /* 8-neighbour majority */
                int sum[4] = {0, 0, 0, 0};
                int occ[4] = {0, 0, 0, 0};
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t nt = static_cast<size_t>(ny) * w + nx;
                        if (!filled[nt]) continue;
                        ++count;
                        for (int ch2 = 0; ch2 < 4; ++ch2) { sum[ch2] += out->baseColor[nt * 4 + ch2]; }
                        for (int ch2 = 0; ch2 < 4; ++ch2) { occ[ch2] += out->occlusion[nt]; }
                    }
                }
                if (count == 0) continue;
                for (int ch2 = 0; ch2 < 4; ++ch2) out->baseColor[t * 4 + ch2] = static_cast<uint8_t>(sum[ch2] / count);
                out->occlusion[t] = static_cast<uint8_t>(occ[0] / count);
                /* Neighbour-average for the remaining maps as well. */
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t nt = static_cast<size_t>(ny) * w + nx;
                        if (!filled[nt]) continue;
                        for (int ch2 = 0; ch2 < 4; ++ch2) {
                            out->normal[t * 4 + ch2] = out->normal[nt * 4 + ch2];
                            out->metallicRoughness[t * 4 + ch2] = out->metallicRoughness[nt * 4 + ch2];
                            out->subsurface[t * 4 + ch2] = out->subsurface[nt * 4 + ch2];
                        }
                        dy = 2; break;
                    }
                }
                next[t] = 1;
            }
        }
        filled.swap(next);
        /* Stop early once the gutter is filled. */
        bool anyEmptyInsideCharts = false;
        (void)anyEmptyInsideCharts;
        (void)span;
    }

    /* Any texel outside every chart stays at the neutral value so the atlas never shows the
       magenta marker in a completed bake. */
    for (size_t i = 0; i < filled.size(); ++i) {
        if (filled[i]) continue;
        out->baseColor[i * 4 + 0] = 128; out->baseColor[i * 4 + 1] = 128;
        out->baseColor[i * 4 + 2] = 128; out->baseColor[i * 4 + 3] = 255;
        out->normal[i * 4 + 0] = 128; out->normal[i * 4 + 1] = 128;
        out->normal[i * 4 + 2] = 255; out->normal[i * 4 + 3] = 255;
        out->metallicRoughness[i * 4 + 0] = 255;
        out->metallicRoughness[i * 4 + 1] = 128;
        out->metallicRoughness[i * 4 + 2] = 0;
        out->metallicRoughness[i * 4 + 3] = 255;
        out->occlusion[i] = 255;
        out->subsurface[i * 4 + 0] = 0; out->subsurface[i * 4 + 1] = 0;
        out->subsurface[i * 4 + 2] = 0; out->subsurface[i * 4 + 3] = 255;
    }
    return true;
}

} // namespace af
