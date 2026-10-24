/*
 * af_engine.cpp -- implementation of the public C ABI declared in afcore.h.
 *
 * This is the only translation unit that touches the pubic ABI, and it is deliberately thin: it
 * owns the handle, the stage machine, the dirty mask, the conversion between kernel types and
 * plain-C structs, and nothing else. All geometry, rigging, texturing and export work happens in
 * the kernel files (af_body, af_rig, af_uv, af_texture, af_glb) so that the host test harness can
 * exercise every stage without an Android device in the loop.
 *
 * Ownership rules implemented here, because they are what a caller in Kotlin or Rust relies on:
 *   - every pointer handed out (vertices, indices, submeshes, materials, textures, joints) points
 *     into storage owned by the engine handle and stays valid until the next mutation call on that
 *     handle, or af_engine_destroy();
 *   - no exception ever crosses the ABI: every entry point catches, converts to an af_result and
 *     records a message the caller can read through af_engine_last_error();
 *   - the handle is thread-affine: one thread at a time. Heavy work fans out onto the engine's own
 *     thread pool *inside* a call.
 */
#include "../include/afcore.h"

#include "af_anatomy.h"
#include "af_body.h"
#include "af_field.h"
#include "af_glb.h"
#include "af_glb_read.h"
#include "af_image.h"
#include "af_png.h"
#include "af_prompt.h"
#include "af_rig.h"
#include "af_texture.h"
#include "af_threads.h"
#include "af_uv.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace af;

namespace {

/* ---------------------------------------------------------------------------------------------
 * Dirty mask: which outputs are up to date. A stage clears the bits its inputs invalidate.
 * ------------------------------------------------------------------------------------------- */
enum : uint32_t {
    DIRTY_METRICS     = 1u << 0,
    DIRTY_GEOMETRY    = 1u << 1,
    DIRTY_UV          = 1u << 2,
    DIRTY_TEXTURES    = 1u << 3,
    DIRTY_RIG         = 1u << 4,
    DIRTY_BLENDSHAPES = 1u << 5,
    DIRTY_ANIMATION   = 1u << 6,
    DIRTY_ALL         = 0x7Fu,
};

const char* regionLabel(int region) {
    static const char* const kNames[13] = {
        "skin", "eye_left", "eye_right", "teeth", "tongue", "lips", "hair", "lash",
        "nail", "cloth_upper", "cloth_lower", "shoe", "gear"
    };
    return (region >= 0 && region < 13) ? kNames[region] : "skin";
}

/* Resolve ABI-1 legacy values before any stage can observe the spec. Height stays in metres here;
   centimetre inputs are converted by their parser/UI boundary, never guessed from magnitude. */
void resolveSpecDefaults(af_spec* spec) {
    if (!spec) return;
    if (!std::isfinite(spec->height_m) || spec->height_m <= 0.0f) {
        spec->height_m = AF_DEFAULT_HEIGHT_M;
    }
    spec->height_m = std::max(1.30f, std::min(2.25f, spec->height_m));
}

bool hasMinorPromptMarker(const af_spec& spec) {
    static const char* const kMarkers[] = {
        "boy", "child", "girl", "kid", "minor", "schoolboy", "schoolgirl", "teen",
        "teenage", "underage"
    };
    const size_t length = strnlen(spec.prompt, sizeof(spec.prompt));
    std::string token;
    token.reserve(16);
    for (size_t i = 0; i <= length; ++i) {
        const unsigned char c = (i < length) ? static_cast<unsigned char>(spec.prompt[i]) : 0;
        if (std::isalnum(c)) {
            token.push_back(static_cast<char>(std::tolower(c)));
            continue;
        }
        if (!token.empty()) {
            for (const char* marker : kMarkers) {
                if (token == marker) return true;
            }
            token.clear();
        }
    }
    return false;
}

/* Outfit NONE is the real unclothed body surface. It is therefore adult-only even before detailed
   sexual anatomy exists. Keeping this rule at the C ABI means JNI, Rust and direct native callers
   cannot bypass the UI. */
const char* adultSafetyError(const af_spec& spec) {
    if (spec.outfit != AF_OUTFIT_NONE) return nullptr;
    if (hasMinorPromptMarker(spec)) {
        return "nude/anatomical generation rejects minor or teen prompt terms";
    }
    if (!std::isfinite(spec.age_years) || spec.age_years < 18.0f) {
        return "nude/anatomical generation requires age_years >= 18";
    }
    return nullptr;
}

} // namespace

/* The handle itself. Opaque to C, a normal C++ object here. */
struct af_engine {
    af_spec spec{};
    af_callbacks cb{};
    af_stage stage = AF_STAGE_IDLE;

    /* analysis */
    af_image_analysis analyses[4]{};
    int32_t analysisCount = 0;
    float analysisConfidence = 0.0f;
    int32_t promptMatched = 0;

    /* kernel state */
    BodyMetrics metrics{};
    std::unique_ptr<BodyField> field;
    Mesh mesh;                 /* current (possibly posed) geometry */
    Mesh restMesh;             /* bind-pose geometry                */
    UVLayout layout;
    TextureSet textures;
    Rig rig;

    /* ABI-facing buffers */
    std::vector<af_vertex> verts;
    std::vector<af_vertex> vertsRest;
    std::vector<uint32_t> indices;
    std::vector<af_submesh> submeshes;
    std::vector<af_material> materials;
    af_texture texViews[AF_TEX_COUNT]{};
    bool texValid[AF_TEX_COUNT] = {false, false, false, false, false};
    std::vector<af_joint_info> joints;
    std::vector<float> jointMatrices;
    std::vector<float> poseQuats;          /* current local rotations */
    std::vector<float> blendWeights;
    std::vector<std::vector<float>> clipCache;   /* per clip: frames * joints * 4 */
    std::vector<int> clipFrameCount;
    std::vector<Vec3> deformed;
    std::vector<uint8_t> glbBytes;

    uint64_t budgetBytes = 1536ull * 1024ull * 1024ull;
    uint64_t peakFieldBytes = 0;
    int atlasResolution = 2048;
    uint32_t dirty = DIRTY_ALL;
    bool geometryReady = false;
    std::string lastError;
    af_stats stats{};
    double tFit = 0, tGeometry = 0, tUv = 0, tTex = 0, tRig = 0, tExport = 0;

    void log(int severity, const std::string& msg) {
        if (cb.on_log) cb.on_log(cb.user, severity, msg.c_str());
    }
    void progress(int stageId, float f, const std::string& msg) {
        if (cb.on_progress) cb.on_progress(cb.user, stageId, f, msg.c_str());
    }
    bool cancelled() const { return cb.should_cancel && cb.should_cancel(cb.user) != 0; }
};

namespace {

/* Every entry point funnels through these two so that no exception can escape. */
int32_t guard(af_engine* e, const char* what, const std::function<int32_t()>& body) {
    if (!e) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    if (e->cancelled()) return static_cast<int32_t>(AF_ERR_CANCELLED);
    try {
        const int32_t rc = body();
        /* Error text describes the most recent call, not an old failure that a later successful
           operation recovered from. Positive values are successful record/match counts. */
        if (rc >= 0) e->lastError.clear();
        return rc;
    } catch (const std::bad_alloc&) {
        e->lastError = std::string(what) + ": out of memory";
        return static_cast<int32_t>(AF_ERR_OUT_OF_MEMORY);
    } catch (const std::exception& ex) {
        e->lastError = std::string(what) + ": " + ex.what();
        return static_cast<int32_t>(AF_ERR_INTERNAL);
    } catch (...) {
        e->lastError = std::string(what) + ": unknown failure";
        return static_cast<int32_t>(AF_ERR_INTERNAL);
    }
}

float nowMs() {
    return static_cast<float>(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/* Estimated peak bytes for the geometry stage: one float per lattice sample plus the mesh. */
uint64_t estimateFieldBytes(const BodyField& field, const BodyBuildOptions& opt) {
    Vec3 lo, hi;
    field.boundsBody(&lo, &hi);
    Vec3 hlo, hhi;
    field.boundsHead(&hlo, &hhi);
    lo = lo.minv(hlo);
    hi = hi.maxv(hhi);
    for (int side = -1; side <= 1; side += 2) {
        Vec3 tlo, thi;
        field.boundsHand(side, &tlo, &thi);
        lo = lo.minv(tlo);
        hi = hi.maxv(thi);
        field.boundsFoot(side, &tlo, &thi);
        lo = lo.minv(tlo);
        hi = hi.maxv(thi);
    }
    if (field.hasApparel()) {
        Vec3 alo, ahi;
        field.boundsApparel(&alo, &ahi);
        lo = lo.minv(alo);
        hi = hi.maxv(ahi);
    }
    const double pad = 2.0 * opt.bodyCell;
    const double nx = std::ceil((hi.x - lo.x + 2.0 * pad) / opt.bodyCell) + 2.0;
    const double ny = std::ceil((hi.y - lo.y + 2.0 * pad) / opt.bodyCell) + 2.0;
    const double nz = std::ceil((hi.z - lo.z + 2.0 * pad) / opt.bodyCell) + 2.0;
    const double samples = nx * ny * nz;
    /* grid values (4 B/sample) + the extraction's cell list + the output mesh (upper bound). */
    return static_cast<uint64_t>(samples * 4.0 * 2.2);
}

int32_t ensureBudget(af_engine* e, uint64_t extraBytes, const char* what) {
    const uint64_t used = e->peakFieldBytes + extraBytes;
    if (used > e->budgetBytes) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s would need %llu MB, budget is %llu MB",
                      what,
                      static_cast<unsigned long long>(used / (1024 * 1024)),
                      static_cast<unsigned long long>(e->budgetBytes / (1024 * 1024)));
        e->lastError = buf;
        return static_cast<int32_t>(AF_ERR_BUDGET_EXCEEDED);
    }
    return static_cast<int32_t>(AF_OK);
}

/* Rebuilds the ABI vertex/index buffers from the kernel mesh + skin weights. */
void rebuildVertexBuffers(af_engine* e, bool bindPose) {
    const Mesh& m = bindPose ? e->restMesh : e->mesh;
    const size_t n = m.vertexCount();
    std::vector<af_vertex>& out = bindPose ? e->vertsRest : e->verts;
    out.assign(n, af_vertex{});
    const int jointCount = e->rig.jointCount();
    const bool haveWeights = e->rig.weights().size() == n;
    for (size_t i = 0; i < n; ++i) {
        af_vertex& v = out[i];
        const Vec3& p = m.positions[i];
        v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
        const Vec3 nrm = (i < m.normals.size()) ? m.normals[i] : Vec3(0, 1, 0);
        v.normal[0] = nrm.x; v.normal[1] = nrm.y; v.normal[2] = nrm.z;
        if (i < m.tangents.size()) {
            v.tangent[0] = m.tangents[i].x; v.tangent[1] = m.tangents[i].y;
            v.tangent[2] = m.tangents[i].z; v.tangent[3] = m.tangents[i].w;
        } else {
            v.tangent[0] = 1.0f; v.tangent[3] = 1.0f;
        }
        if (m.uvs.size() >= (i + 1) * 2) {
            v.uv[0] = m.uvs[i * 2]; v.uv[1] = m.uvs[i * 2 + 1];
        }
        /* Colour multiplier: white. All colour lives in the baked base colour map, so the vertex
           colour is a neutral multiplier - which is also what lets the renderer tint a material at
           runtime (team colours, character customisation) without re-baking. */
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
        if (haveWeights && jointCount > 0) {
            const SkinWeights& sw = e->rig.weights()[i];
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += sw.weights[k];
            for (int k = 0; k < 4; ++k) {
                const int j = (sw.joints[k] < jointCount) ? static_cast<int>(sw.joints[k]) : 0;
                v.joints[k] = static_cast<uint16_t>(j);
                v.weights[k] = (sum > 1e-6f) ? (sw.weights[k] / sum) : (k == 0 ? 1.0f : 0.0f);
            }
        } else {
            v.weights[0] = 1.0f;
        }
    }
    if (bindPose) {
        e->indices = m.indices;
    }
}

void rebuildSubmeshesAndMaterials(af_engine* e) {
    e->materials.clear();
    e->submeshes.clear();
    const Mesh& m = e->mesh;
    if (m.triangleCount() == 0) return;

    /* Materials: one per distinct material NAME, in first-seen region order. */
    std::vector<int> regionOfMaterial;
    for (int r = 0; r < 13; ++r) {
        const std::string name = materialForRegion(r).name;
        int found = -1;
        for (size_t i = 0; i < e->materials.size(); ++i) {
            if (name == e->materials[i].name) { found = static_cast<int>(i); break; }
        }
        if (found < 0) {
            const MaterialDef& md = materialForRegion(r);
            af_material am{};
            std::snprintf(am.name, sizeof(am.name), "%s", md.name.c_str());
            /* Spec overrides: the user's colour choices ride on the base colour factor, which the
               renderer multiplies with the baked texture (glTF semantics). */
            float base[3] = {md.baseColor.x, md.baseColor.y, md.baseColor.z};
            if (r == REGION_CLOTH_UPPER || r == REGION_CLOTH_LOWER) {
                const float* ct = e->spec.cloth_tone;
                if (ct[0] > 0.0f || ct[1] > 0.0f || ct[2] > 0.0f) {
                    base[0] = ct[0]; base[1] = ct[1]; base[2] = ct[2];
                }
            } else if (r == REGION_GEAR || r == REGION_SHOE) {
                const float* gt = e->spec.gear_tone;
                if (gt[0] > 0.0f || gt[1] > 0.0f || gt[2] > 0.0f) {
                    base[0] = gt[0]; base[1] = gt[1]; base[2] = gt[2];
                }
            } else if (r == REGION_HAIR) {
                const float* ht = e->spec.hair_tone;
                if (ht[0] > 0.0f || ht[1] > 0.0f || ht[2] > 0.0f) {
                    base[0] = ht[0]; base[1] = ht[1]; base[2] = ht[2];
                }
            }
            am.base_color_factor[0] = base[0];
            am.base_color_factor[1] = base[1];
            am.base_color_factor[2] = base[2];
            am.base_color_factor[3] = 1.0f;
            am.metallic = (e->spec.gear_metalness >= 0.0f && (r == REGION_GEAR || r == REGION_SHOE))
                              ? e->spec.gear_metalness : md.metallic;
            am.roughness = (e->spec.cloth_roughness >= 0.0f && (r == REGION_CLOTH_UPPER || r == REGION_CLOTH_LOWER))
                               ? e->spec.cloth_roughness : md.roughness;
            am.ior = md.ior;
            am.sheen = md.sheen;
            am.sheen_tint = md.sheenTint;
            am.subsurface = md.subsurface;
            am.anisotropic = md.anisotropic;
            am.has_base_color_tex = 1;
            am.has_normal_tex = 1;
            am.has_mr_tex = 1;
            am.has_ao_tex = 1;
            e->materials.push_back(am);
            regionOfMaterial.push_back(r);
        }
    }

    /* Submeshes: contiguous index ranges per material, sharing the whole vertex buffer (the
       renderer binds one VBO and draws each range; a per-submesh vertex copy would duplicate
       14 MB of vertex data for nothing). */
    std::vector<std::vector<uint32_t>> groupIndices(e->materials.size());
    for (size_t t = 0; t < m.triangleCount(); ++t) {
        const int32_t r = m.regions.empty() ? 0 : m.regions[m.indices[t * 3]];
        const std::string name = materialForRegion((r >= 0 && r < 13) ? r : 0).name;
        int mat = 0;
        for (size_t i = 0; i < e->materials.size(); ++i) {
            if (name == e->materials[i].name) { mat = static_cast<int>(i); break; }
        }
        std::vector<uint32_t>& g = groupIndices[static_cast<size_t>(mat)];
        g.push_back(m.indices[t * 3]);
        g.push_back(m.indices[t * 3 + 1]);
        g.push_back(m.indices[t * 3 + 2]);
    }
    uint32_t indexOffset = 0;
    for (size_t i = 0; i < e->materials.size(); ++i) {
        if (groupIndices[i].empty()) continue;
        af_submesh sm{};
        sm.region = regionOfMaterial[i];
        sm.vertex_offset = 0;
        sm.vertex_count = static_cast<int32_t>(m.vertexCount());
        sm.index_offset = static_cast<int32_t>(indexOffset);
        sm.index_count = static_cast<int32_t>(groupIndices[i].size());
        sm.material_index = static_cast<int32_t>(i);
        Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (uint32_t idx : groupIndices[i]) {
            const Vec3& p = m.positions[idx];
            mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
            mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
        }
        sm.bounds_min[0] = mn.x; sm.bounds_min[1] = mn.y; sm.bounds_min[2] = mn.z;
        sm.bounds_max[0] = mx.x; sm.bounds_max[1] = mx.y; sm.bounds_max[2] = mx.z;
        e->submeshes.push_back(sm);
        indexOffset += static_cast<uint32_t>(groupIndices[i].size());
    }
    /* The shared index buffer must be grouped the same way, or the ranges above point at the
       wrong triangles. */
    e->indices.clear();
    e->indices.reserve(m.indices.size());
    for (size_t i = 0; i < e->materials.size(); ++i) {
        e->indices.insert(e->indices.end(), groupIndices[i].begin(), groupIndices[i].end());
    }
}

void rebuildJointInfo(af_engine* e) {
    const std::vector<Joint>& src = e->rig.joints();
    e->joints.assign(src.size(), af_joint_info{});
    for (size_t i = 0; i < src.size(); ++i) {
        const Joint& j = src[i];
        std::snprintf(e->joints[i].name, sizeof(e->joints[i].name), "%s", j.name.c_str());
        e->joints[i].parent = j.parent;
        e->joints[i].flags = j.flags;
        e->joints[i].rest_translation[0] = j.restTranslation.x;
        e->joints[i].rest_translation[1] = j.restTranslation.y;
        e->joints[i].rest_translation[2] = j.restTranslation.z;
        e->joints[i].rest_rotation[0] = j.restRotation.x;
        e->joints[i].rest_rotation[1] = j.restRotation.y;
        e->joints[i].rest_rotation[2] = j.restRotation.z;
        e->joints[i].rest_rotation[3] = j.restRotation.w;
    }
}

void refreshTextures(af_engine* e) {
    const std::vector<uint8_t>* maps[AF_TEX_COUNT] = {
        &e->textures.baseColor, &e->textures.normal, &e->textures.metallicRoughness,
        &e->textures.occlusion, &e->textures.subsurface
    };
    const int channels[AF_TEX_COUNT] = {4, 4, 4, 1, 4};
    for (int i = 0; i < AF_TEX_COUNT; ++i) {
        af_texture& t = e->texViews[i];
        t.width = e->textures.width;
        t.height = e->textures.height;
        t.channels = channels[i];
        t.mip_levels = 1;      /* the renderer generates mips on the GPU */
        t.byte_size = static_cast<uint64_t>(maps[i]->size());
        t.data = maps[i]->empty() ? nullptr : maps[i]->data();
        e->texValid[i] = !maps[i]->empty();
    }
}

void updateStats(af_engine* e) {
    af_stats& s = e->stats;
    s.vertex_count = static_cast<int32_t>(e->verts.size());
    s.index_count = static_cast<int32_t>(e->indices.size());
    s.triangle_count = static_cast<int32_t>(e->indices.size() / 3);
    s.submesh_count = static_cast<int32_t>(e->submeshes.size());
    s.joint_count = e->rig.jointCount();
    s.blendshape_count = e->stats.blendshape_count;
    s.geometry_bytes = static_cast<uint64_t>(e->verts.size()) * sizeof(af_vertex) +
                       static_cast<uint64_t>(e->indices.size()) * sizeof(uint32_t);
    s.texture_bytes = e->textures.width > 0 ? textureSetBytes(e->textures) : 0;
    s.peak_field_bytes = e->peakFieldBytes;
    s.fit_anatomy_ms = static_cast<float>(e->tFit);
    s.field_ms = static_cast<float>(e->tGeometry);
    s.isosurface_ms = 0.0f;      /* folded into field_ms: one unified pass, reported honestly */
    s.garment_ms = 0.0f;
    s.hair_ms = 0.0f;
    s.uv_ms = static_cast<float>(e->tUv);
    s.texture_ms = static_cast<float>(e->tTex);
    s.rig_ms = static_cast<float>(e->tRig);
    s.export_ms = static_cast<float>(e->tExport);
}

} // namespace

/* ==============================================================================================
 * Lifetime
 * ============================================================================================ */
extern "C" {

int32_t af_engine_abi_version(void) { return AF_ABI_VERSION; }

int32_t af_engine_create(const af_spec* spec, const af_callbacks* callbacks, af_engine** out_engine) {
    if (!out_engine) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    *out_engine = nullptr;
    af_engine* e = new (std::nothrow) af_engine();
    if (!e) return static_cast<int32_t>(AF_ERR_OUT_OF_MEMORY);
    af_spec_init(&e->spec);
    if (spec) {
        /* Copy field by field: a caller built against ABI 1 may pass a smaller struct, and
           struct_size says how much of it is real. */
        const size_t n = static_cast<size_t>(spec->struct_size > 0)
                             ? std::min(sizeof(af_spec), static_cast<size_t>(spec->struct_size))
                             : sizeof(af_spec);
        std::memcpy(&e->spec, spec, n);
        e->spec.struct_size = static_cast<int32_t>(sizeof(af_spec));
    }
    resolveSpecDefaults(&e->spec);
    if (adultSafetyError(e->spec)) {
        delete e;
        return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    }
    if (callbacks) e->cb = *callbacks;
    e->blendWeights.assign(kBlendshapeCount, 0.0f);
    e->stats.blendshape_count = 0;
    *out_engine = e;
    return static_cast<int32_t>(AF_OK);
}

void af_engine_destroy(af_engine* engine) {
    delete engine;
}

int32_t af_engine_reset(af_engine* engine, const af_spec* spec) {
    return guard(engine, "reset", [&]() {
        af_spec fresh;
        af_spec_init(&fresh);
        if (spec) {
            const size_t n = static_cast<size_t>(spec->struct_size > 0)
                                 ? std::min(sizeof(af_spec), static_cast<size_t>(spec->struct_size))
                                 : sizeof(af_spec);
            std::memcpy(&fresh, spec, n);
            fresh.struct_size = static_cast<int32_t>(sizeof(af_spec));
        }
        resolveSpecDefaults(&fresh);
        if (const char* safety = adultSafetyError(fresh)) {
            engine->lastError = std::string("reset: ") + safety;
            return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        }
        engine->spec = fresh;
        engine->field.reset();
        engine->mesh = Mesh();
        engine->restMesh = Mesh();
        engine->layout = UVLayout();
        engine->textures = TextureSet();
        engine->rig = Rig();
        engine->verts.clear();
        engine->vertsRest.clear();
        engine->indices.clear();
        engine->submeshes.clear();
        engine->materials.clear();
        engine->joints.clear();
        engine->jointMatrices.clear();
        engine->poseQuats.clear();
        engine->clipCache.clear();
        engine->clipFrameCount.clear();
        engine->deformed.clear();
        engine->glbBytes.clear();
        engine->geometryReady = false;
        engine->analysisCount = 0;
        engine->promptMatched = 0;
        engine->dirty = DIRTY_ALL;
        engine->stage = AF_STAGE_IDLE;
        engine->peakFieldBytes = 0;
        engine->atlasResolution = 2048;
        for (int i = 0; i < AF_TEX_COUNT; ++i) {
            engine->texValid[i] = false;
            engine->texViews[i] = af_texture{};
        }
        std::fill(engine->blendWeights.begin(), engine->blendWeights.end(), 0.0f);
        engine->stats = af_stats{};
        engine->log(AF_LOG_INFO, "engine reset");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_set_memory_budget(af_engine* engine, uint64_t bytes) {
    if (!engine) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    if (bytes < 64ull * 1024ull * 1024ull) return static_cast<int32_t>(AF_ERR_INVALID_ARG);   /* below the floor */
    if (engine->budgetBytes != bytes) {
        engine->budgetBytes = bytes;
        if (engine->geometryReady) {
            /* Rebuild from anatomy rather than re-unwrapping an already seam-split mesh. */
            engine->dirty = DIRTY_ALL;
            engine->geometryReady = false;
        }
    }
    return static_cast<int32_t>(AF_OK);
}

int32_t af_engine_set_gpu_hint(af_engine* engine, const char* hint) {
    if (!engine || !hint) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    std::snprintf(engine->stats.gpu_hint, sizeof(engine->stats.gpu_hint), "%s", hint);
    return static_cast<int32_t>(AF_OK);
}

/* ==============================================================================================
 * Inputs
 * ============================================================================================ */
int32_t af_engine_analyze_prompt(af_engine* engine, const char* prompt, af_spec* out_spec) {
    return guard(engine, "analyze_prompt", [&]() {
        if (!prompt) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        engine->stage = AF_STAGE_ANALYZE_INPUTS;
        engine->progress(AF_STAGE_ANALYZE_INPUTS, 0.0f, "reading prompt");
        const af_spec previous = engine->spec;
        PromptParse parse;
        const int matched = parsePrompt(prompt, &engine->spec, &parse);
        if (const char* safety = adultSafetyError(engine->spec)) {
            engine->spec = previous;
            engine->lastError = std::string("analyze_prompt: ") + safety;
            if (out_spec) *out_spec = previous;
            return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        }
        engine->promptMatched = matched;
        for (int i = 0; i < parse.tokenCount; ++i) {
            engine->log(AF_LOG_INFO, std::string("understood \"") + parse.tokens[i].text +
                                         "\" -> " + parse.tokens[i].field);
        }
        if (matched == 0) {
            engine->log(AF_LOG_WARN, "prompt recognised no attributes; using the default spec");
        } else {
            /* Any spec change invalidates everything downstream of the anatomy. */
            engine->dirty |= DIRTY_ALL;
            engine->geometryReady = false;
        }
        if (out_spec) *out_spec = engine->spec;
        engine->progress(AF_STAGE_ANALYZE_INPUTS, 1.0f, "prompt interpreted");
        return static_cast<int32_t>(matched);
    });
}

int32_t af_engine_analyze_images(af_engine* engine, const af_image* images, int32_t image_count,
                                 af_image_analysis* out_analyses, int32_t max_analyses) {
    return guard(engine, "analyze_images", [&]() {
        if (!images || image_count <= 0) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        engine->stage = AF_STAGE_ANALYZE_INPUTS;
        engine->analysisCount = 0;
        /* Failed images are skipped, so original input indices cannot index the compact analysis
           array. Retain this map for later role selection. */
        int32_t analysisIndex[4] = {-1, -1, -1, -1};
        int32_t produced = 0;
        for (int32_t i = 0; i < image_count && i < 4; ++i) {
            if (engine->cancelled()) return static_cast<int32_t>(AF_ERR_CANCELLED);
            engine->progress(AF_STAGE_ANALYZE_INPUTS, static_cast<float>(i) / image_count,
                             "analysing reference image");
            af_image_analysis a{};
            if (!analyseImage(images[i], &a)) {
                engine->log(AF_LOG_WARN, "reference image could not be analysed (empty or unreadable)");
                continue;
            }
            analysisIndex[i] = engine->analysisCount;
            engine->analyses[engine->analysisCount++] = a;
            if (out_analyses && produced < max_analyses) out_analyses[produced] = a;
            ++produced;
        }
        if (engine->analysisCount == 0) return static_cast<int32_t>(AF_ERR_INVALID_ARG);

        /* Identity reference is the first image unless one is explicitly marked. Clothing wins if
           present, because that is the image the user pointed at for the outfit. */
        const af_image_analysis* identity = &engine->analyses[0];
        const af_image_analysis* clothing = nullptr;
        for (int32_t i = 0; i < image_count && i < 4; ++i) {
            if (analysisIndex[i] < 0) continue;
            if (images[i].role == AF_IMAGE_CLOTHING) {
                clothing = &engine->analyses[analysisIndex[i]];
            }
            if (images[i].role == AF_IMAGE_IDENTITY) {
                identity = &engine->analyses[analysisIndex[i]];
            }
        }
        float confidence = 0.0f;
        specFromAnalysis(*identity, clothing, &engine->spec, &confidence);
        engine->analysisConfidence = confidence;
        engine->dirty |= DIRTY_ALL;
        engine->geometryReady = false;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "identity: full body %s, coverage %.2f, melanin %.2f, confidence %.2f",
                      identity->is_full_body ? "yes" : "no", identity->subject_coverage,
                      engine->spec.skin_melanin, confidence);
        engine->log(AF_LOG_INFO, buf);
        engine->progress(AF_STAGE_ANALYZE_INPUTS, 1.0f, "reference images analysed");
        return produced;
    });
}

/* ==============================================================================================
 * Stages
 * ============================================================================================ */
int32_t af_engine_fit_anatomy(af_engine* engine) {
    return guard(engine, "fit_anatomy", [&]() {
        if (const char* safety = adultSafetyError(engine->spec)) {
            engine->lastError = std::string("fit_anatomy: ") + safety;
            return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        }
        engine->stage = AF_STAGE_FIT_ANATOMY;
        engine->progress(AF_STAGE_FIT_ANATOMY, 0.0f, "solving anthropometry");
        const float t0 = nowMs();
        engine->metrics = Anatomy::solve(engine->spec);
        /* Publish the exact resolved stature back into the canonical spec used by texture/export.
           This keeps metadata, renderer bounds and anatomy on one non-zero value. */
        engine->spec.height_m = engine->metrics.height;
        engine->field.reset(new BodyField(engine->metrics));
        ApparelOptions ap;
        ap.outfit = engine->spec.outfit;
        ap.footwear = engine->spec.footwear;
        ap.hair = engine->spec.hair_style;
        /* The lattice the caller will extract on; see BodyBuildOptions::bodyCell. */
        BodyBuildOptions opt;
        ap.latticeCell = opt.bodyCell;
        engine->field->setApparel(ap);
        engine->tFit = nowMs() - t0;
        engine->dirty = DIRTY_ALL & ~DIRTY_METRICS;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "stature %.3f m, crown %.3f m, chin %.3f m, shoulder %.3f m, hip %.3f m",
                      engine->metrics.height, engine->metrics.y.crownY, engine->metrics.y.chinY,
                      engine->metrics.y.shoulderY, engine->metrics.y.hipY);
        engine->log(AF_LOG_INFO, buf);
        engine->progress(AF_STAGE_FIT_ANATOMY, 1.0f, "anatomy solved");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_build_geometry(af_engine* engine) {
    return guard(engine, "build_geometry", [&]() {
        if (!engine->field) return static_cast<int32_t>(AF_ERR_NOT_READY);
        engine->stage = AF_STAGE_GEOMETRY;
        BodyBuildOptions opt;
        opt.threads = defaultThreadCount();
        opt.cancelled = [engine]() { return engine->cancelled(); };
        opt.progress = [engine](int stage, float f) {
            engine->progress(AF_STAGE_GEOMETRY, f, stage == 0 ? "sampling the signed distance field"
                                                : stage == 1 ? "extracting the isosurface"
                                                             : "refining and repairing topology");
        };
        const uint64_t need = estimateFieldBytes(*engine->field, opt);
        const int32_t budget = ensureBudget(engine, need, "geometry");
        if (budget != AF_OK) return budget;

        const float t0 = nowMs();
        BodyBuildReport rep;
        Mesh built;
        if (!buildBodyMesh(*engine->field, opt, &built, &rep)) {
            engine->lastError = "geometry stage failed";
            return static_cast<int32_t>(AF_ERR_INTERNAL);
        }
        engine->peakFieldBytes = need;
        engine->mesh = std::move(built);
        engine->restMesh = engine->mesh;
        engine->tGeometry = nowMs() - t0;
        engine->geometryReady = true;
        engine->dirty = DIRTY_ALL & ~(DIRTY_METRICS | DIRTY_GEOMETRY);
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "%zu triangles, %zu vertices, open edges %zu, non-manifold %zu, %s, %.0f ms",
                      engine->mesh.triangleCount(), engine->mesh.vertexCount(),
                      rep.openEdgesAfterRepair, rep.nonManifoldEdges,
                      rep.quality == TopologyQuality::WATERTIGHT ? "watertight"
                          : (rep.quality == TopologyQuality::REPAIRED ? "repaired" : "degraded"),
                      rep.millis);
        engine->log(rep.quality == TopologyQuality::DEGRADED ? AF_LOG_WARN : AF_LOG_INFO, buf);
        engine->progress(AF_STAGE_GEOMETRY, 1.0f, "geometry complete");

        /* The ABI's buffers are refreshed only when everything downstream exists, so a caller
           cannot observe a half-built mesh. */
        rebuildVertexBuffers(engine, false);
        rebuildVertexBuffers(engine, true);
        /* Statistics are part of the contract too: a caller driving the stages by hand sees
           vertex_count/joint_count/peak_field_bytes after each one, not only after export. (They
           used to stay zero until the export stage, which made a staged build look like it had
           produced nothing.) */
        updateStats(engine);
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_unwrap_uv(af_engine* engine) {
    return guard(engine, "unwrap_uv", [&]() {
        if (!engine->geometryReady) return static_cast<int32_t>(AF_ERR_NOT_READY);
        engine->stage = AF_STAGE_UV_UNWRAP;
        engine->progress(AF_STAGE_UV_UNWRAP, 0.2f, "charting and packing the atlas");
        /* A previous bake is stale once UVs change. Release it before selecting the next atlas
           so the budget calculation reflects peak, not additive, texture memory. */
        engine->textures = TextureSet();
        engine->layout = UVLayout();
        refreshTextures(engine);
        const int resolution = atlasResolutionForBudget(engine->budgetBytes, engine->peakFieldBytes);
        if (resolution == 0) {
            engine->lastError = "UV unwrap: memory budget cannot fit even a 256x256 texture atlas";
            return static_cast<int32_t>(AF_ERR_BUDGET_EXCEEDED);
        }
        engine->atlasResolution = resolution;
        const float t0 = nowMs();
        UVLayout layout;
        if (!unwrapAtlas(&engine->mesh, resolution, resolution, &layout)) {
            engine->lastError = "UV unwrap failed";
            return static_cast<int32_t>(AF_ERR_INTERNAL);
        }
        engine->layout = layout;
        engine->restMesh = engine->mesh;      /* keep the bind pose in sync with the split mesh */
        engine->tUv = nowMs() - t0;
        engine->dirty = DIRTY_ALL & ~(DIRTY_METRICS | DIRTY_GEOMETRY | DIRTY_UV);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%zu charts, %dx%d, coverage %.2f, %.0f texels/m, %.0f ms",
                      layout.charts.size(), layout.texWidth, layout.texHeight, layout.coverage,
                      layout.texelsPerMeter, engine->tUv);
        engine->log(AF_LOG_INFO, buf);
        rebuildVertexBuffers(engine, false);
        rebuildVertexBuffers(engine, true);
        engine->progress(AF_STAGE_UV_UNWRAP, 1.0f, "atlas ready");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_bake_textures(af_engine* engine) {
    return guard(engine, "bake_textures", [&]() {
        if (!engine->geometryReady || engine->layout.charts.empty()) return static_cast<int32_t>(AF_ERR_NOT_READY);
        engine->stage = AF_STAGE_BAKE_TEXTURES;
        engine->progress(AF_STAGE_BAKE_TEXTURES, 0.1f, "baking PBR maps");
        BakeOptions bo;
        bo.width = engine->atlasResolution;
        bo.height = engine->atlasResolution;
        const uint64_t need = atlasBakeWorkspaceBytes(bo.width);
        const int32_t budget = ensureBudget(engine, need, "texture bake");
        if (budget != AF_OK) return budget;
        const float t0 = nowMs();
        TextureSet ts;
        if (!bakeTextures(engine->mesh, engine->layout, engine->metrics, engine->spec, bo, &ts)) {
            engine->lastError = "texture bake failed";
            return static_cast<int32_t>(AF_ERR_INTERNAL);
        }
        engine->textures = std::move(ts);
        engine->tTex = nowMs() - t0;
        engine->dirty &= ~DIRTY_TEXTURES;
        refreshTextures(engine);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%dx%d, %.1f MB, %.0f ms", engine->textures.width,
                      engine->textures.height, textureSetBytes(engine->textures) / 1048576.0, engine->tTex);
        engine->log(AF_LOG_INFO, buf);
        engine->progress(AF_STAGE_BAKE_TEXTURES, 1.0f, "textures baked");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_rig(af_engine* engine) {
    return guard(engine, "rig", [&]() {
        if (!engine->geometryReady) return static_cast<int32_t>(AF_ERR_NOT_READY);
        engine->stage = AF_STAGE_RIG;
        engine->progress(AF_STAGE_RIG, 0.2f, "building the skeleton");
        const float t0 = nowMs();
        Rig rig;
        if (!rig.build(*engine->field, engine->mesh)) {
            engine->lastError = "rig build failed";
            return static_cast<int32_t>(AF_ERR_INTERNAL);
        }
        engine->rig = std::move(rig);
        engine->tRig = nowMs() - t0;
        engine->poseQuats.assign(static_cast<size_t>(engine->rig.jointCount()) * 4, 0.0f);
        for (int j = 0; j < engine->rig.jointCount(); ++j) {
            engine->poseQuats[static_cast<size_t>(j) * 4 + 3] = 1.0f;   /* identity */
        }
        engine->jointMatrices.assign(static_cast<size_t>(engine->rig.jointCount()) * 16, 0.0f);
        /* Publish the bind-pose matrices immediately: a caller that reads joint matrices before
           posing must see the rest pose, not zeros. */
        {
            std::vector<Quat> identity(static_cast<size_t>(engine->rig.jointCount()));
            const std::vector<Mat4> world = engine->rig.computeWorld(identity);
            for (size_t j = 0; j < world.size() && j * 16 + 16 <= engine->jointMatrices.size(); ++j) {
                std::memcpy(&engine->jointMatrices[j * 16], world[j].m, 16 * sizeof(float));
            }
        }
        engine->dirty &= ~DIRTY_RIG;
        rebuildJointInfo(engine);
        rebuildVertexBuffers(engine, false);
        rebuildVertexBuffers(engine, true);
        rebuildSubmeshesAndMaterials(engine);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%d joints, %zu weighted vertices, %.0f ms",
                      engine->rig.jointCount(), engine->rig.weights().size(), engine->tRig);
        engine->log(AF_LOG_INFO, buf);
        engine->progress(AF_STAGE_RIG, 1.0f, "skeleton and skin weights ready");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_bake_blendshapes(af_engine* engine) {
    return guard(engine, "bake_blendshapes", [&]() {
        if (engine->rig.jointCount() == 0) return static_cast<int32_t>(AF_ERR_NOT_READY);
        engine->stage = AF_STAGE_BLENDSHAPES;
        engine->progress(AF_STAGE_BLENDSHAPES, 0.2f, "computing facial targets");
        int produced = 0;
        for (int c = 0; c < kBlendshapeCount; ++c) {
            if (engine->cancelled()) return static_cast<int32_t>(AF_ERR_CANCELLED);
            const std::vector<Vec3>& deltas = engine->rig.blendshapeDeltas(c);
            if (deltas.size() == engine->mesh.vertexCount()) ++produced;
            if (c % 8 == 0) {
                engine->progress(AF_STAGE_BLENDSHAPES, 0.2f + 0.7f * static_cast<float>(c) / kBlendshapeCount,
                                 "computing facial targets");
            }
        }
        engine->stats.blendshape_count = produced;
        engine->dirty &= ~DIRTY_BLENDSHAPES;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d ARKit-compatible blendshape targets", produced);
        engine->log(produced == kBlendshapeCount ? AF_LOG_INFO : AF_LOG_WARN, buf);
        engine->progress(AF_STAGE_BLENDSHAPES, 1.0f, "facial rig ready");
        return static_cast<int32_t>((produced == kBlendshapeCount) ? AF_OK : AF_ERR_INTERNAL);
    });
}

int32_t af_engine_bake_animation(af_engine* engine) {
    return guard(engine, "bake_animation", [&]() {
        if (engine->rig.jointCount() == 0) return static_cast<int32_t>(AF_ERR_NOT_READY);
        engine->stage = AF_STAGE_ANIMATION;
        const std::vector<Clip>& clips = builtinClips();
        if (clips.empty()) return static_cast<int32_t>(AF_ERR_INTERNAL);
        const int jointCount = engine->rig.jointCount();
        const float hz = 30.0f;
        engine->clipCache.assign(clips.size(), {});
        engine->clipFrameCount.assign(clips.size(), 0);
        int boundTotal = 0;
        for (size_t ci = 0; ci < clips.size(); ++ci) {
            const Clip& clip = clips[ci];
            const int frames = std::max(2, static_cast<int>(std::ceil(clip.duration * hz)) + 1);
            std::vector<float>& cache = engine->clipCache[ci];
            cache.assign(static_cast<size_t>(frames) * jointCount * 4, 0.0f);
            /* Bind the clip's tracks once, by name, then evaluate. The bind table is also what the
               ABI's sample/apply calls use, so playback never re-resolves a joint. */
            std::vector<std::pair<int, const Track*>> bound;
            for (const Track& tr : clip.tracks) {
                const int j = engine->rig.jointIndex(tr.jointName.c_str());
                if (j >= 0) bound.emplace_back(j, &tr);
            }
            boundTotal += static_cast<int>(bound.size());
            for (int f = 0; f < frames; ++f) {
                const float t = clip.duration * static_cast<float>(f) / static_cast<float>(frames - 1);
                for (int j = 0; j < jointCount; ++j) {
                    cache[(static_cast<size_t>(f) * jointCount + j) * 4 + 3] = 1.0f;   /* identity */
                }
                for (const auto& bt : bound) {
                    const Quat q = bt.second->sample(t);
                    float* dst = &cache[(static_cast<size_t>(f) * jointCount + bt.first) * 4];
                    dst[0] = q.x; dst[1] = q.y; dst[2] = q.z; dst[3] = q.w;
                }
            }
            engine->clipFrameCount[ci] = frames;
            engine->progress(AF_STAGE_ANIMATION, static_cast<float>(ci + 1) / clips.size(),
                             "sampling the clip library");
        }
        engine->dirty &= ~DIRTY_ANIMATION;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%zu clips baked at %.0f Hz (%d bound tracks)",
                      clips.size(), hz, boundTotal);
        engine->log(AF_LOG_INFO, buf);
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_run_all(af_engine* engine) {
    return guard(engine, "run_all", [&]() {
        const uint32_t todo = engine->dirty;
        if (todo & DIRTY_METRICS) {
            const int32_t rc = af_engine_fit_anatomy(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->dirty & DIRTY_GEOMETRY) {
            const int32_t rc = af_engine_build_geometry(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->dirty & DIRTY_UV) {
            const int32_t rc = af_engine_unwrap_uv(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->dirty & DIRTY_TEXTURES) {
            const int32_t rc = af_engine_bake_textures(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->dirty & DIRTY_RIG) {
            const int32_t rc = af_engine_rig(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->dirty & DIRTY_BLENDSHAPES) {
            const int32_t rc = af_engine_bake_blendshapes(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->dirty & DIRTY_ANIMATION) {
            const int32_t rc = af_engine_bake_animation(engine);
            if (rc != AF_OK) return rc;
        }
        if (engine->cancelled()) return static_cast<int32_t>(AF_ERR_CANCELLED);
        engine->stage = AF_STAGE_DONE;
        updateStats(engine);
        engine->progress(AF_STAGE_DONE, 1.0f, "avatar ready");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_dirty_mask(af_engine* engine) {
    if (!engine) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    return static_cast<int32_t>(engine->dirty);
}

/* ==============================================================================================
 * Posing and animation
 * ============================================================================================ */
namespace {

/* Applies the current pose + blendshape weights and refreshes the renderable vertex buffer. */
int32_t applyCurrentPose(af_engine* e) {
    if (e->rig.jointCount() == 0) return static_cast<int32_t>(AF_ERR_NOT_READY);
    const int J = e->rig.jointCount();
    std::vector<Quat> locals(static_cast<size_t>(J));
    for (int j = 0; j < J; ++j) {
        locals[static_cast<size_t>(j)] = Quat(e->poseQuats[static_cast<size_t>(j) * 4 + 0],
                                              e->poseQuats[static_cast<size_t>(j) * 4 + 1],
                                              e->poseQuats[static_cast<size_t>(j) * 4 + 2],
                                              e->poseQuats[static_cast<size_t>(j) * 4 + 3]).normalized();
    }
    const std::vector<Mat4> world = e->rig.computeWorld(locals);
    for (int j = 0; j < J; ++j) {
        std::memcpy(&e->jointMatrices[static_cast<size_t>(j) * 16], world[static_cast<size_t>(j)].m,
                    16 * sizeof(float));
    }
    e->deformed.assign(e->restMesh.vertexCount(), Vec3(0, 0, 0));
    /*
     * LBS followed by the blendshape offsets - the same order the GLB morph targets imply, so the
     * in-app preview and an exported file agree.
     *
     * The deformation is always evaluated from the BIND POSE, never from the currently displayed
     * geometry: `mesh` holds the last posed result, so deforming it would compound the previous
     * pose with the new one. That mistake is invisible while a clip plays (every frame is close to
     * the last) and obvious the moment pose changes are composed - a walk followed by
     * AF_CLIP_REST left 261 777 of 268 591 vertices displaced from the bind pose instead of 0.
     */
    e->rig.deform(e->restMesh, world, e->blendWeights, &e->deformed);
    for (size_t i = 0; i < e->deformed.size(); ++i) {
        e->mesh.positions[i] = e->deformed[i];
        if (i < e->verts.size()) {
            e->verts[i].pos[0] = e->deformed[i].x;
            e->verts[i].pos[1] = e->deformed[i].y;
            e->verts[i].pos[2] = e->deformed[i].z;
        }
    }
    /* Normals are refreshed from the deformed surface: a posed avatar whose normals stay in the
       bind pose shades as if the limbs were still straight. Area-weighted face normals are the
       standard approximation for linear-blend skinning and cost one pass. */
    e->mesh.areaWeightedNormals();
    for (size_t i = 0; i < e->mesh.normals.size() && i < e->verts.size(); ++i) {
        e->verts[i].normal[0] = e->mesh.normals[i].x;
        e->verts[i].normal[1] = e->mesh.normals[i].y;
        e->verts[i].normal[2] = e->mesh.normals[i].z;
    }
    updateStats(e);
    return static_cast<int32_t>(AF_OK);
}

int32_t sampleClipInto(af_engine* e, int32_t clip, float t, float* quats, int32_t jointCount) {
    if (jointCount != e->rig.jointCount()) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    /*
     * AF_CLIP_REST is the bind pose, and the bind pose is the rig's own rest pose - there are no
     * keys to interpolate, which is exactly why the clip library deliberately has no "rest" entry
     * (the harness asserts that library size == AF_CLIP_COUNT - 1). Applying it therefore means
     * writing identity rotations, not failing.
     */
    if (clip == AF_CLIP_REST) {
        for (int j = 0; j < jointCount; ++j) {
            quats[static_cast<size_t>(j) * 4 + 0] = 0.0f;
            quats[static_cast<size_t>(j) * 4 + 1] = 0.0f;
            quats[static_cast<size_t>(j) * 4 + 2] = 0.0f;
            quats[static_cast<size_t>(j) * 4 + 3] = 1.0f;
        }
        return static_cast<int32_t>(AF_OK);
    }
    const int idx = clipIndexForId(clip);
    if (idx < 0 || static_cast<size_t>(idx) >= e->clipCache.size()) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    const std::vector<float>& cache = e->clipCache[static_cast<size_t>(idx)];
    const int frames = e->clipFrameCount[static_cast<size_t>(idx)];
    const std::vector<Clip>& clips = builtinClips();
    const float duration = clips[static_cast<size_t>(idx)].duration;
    float phase = t;
    if (clips[static_cast<size_t>(idx)].looping) {
        phase = std::fmod(t, duration);
        if (phase < 0.0f) phase += duration;
    } else {
        phase = std::max(0.0f, std::min(duration, t));
    }
    const float fpos = phase / duration * static_cast<float>(frames - 1);
    const int f0 = std::max(0, std::min(frames - 1, static_cast<int>(fpos)));
    const int f1 = std::min(frames - 1, f0 + 1);
    const float frac = fpos - static_cast<float>(f0);
    for (int j = 0; j < jointCount; ++j) {
        const float* a = &cache[(static_cast<size_t>(f0) * jointCount + j) * 4];
        const float* b = &cache[(static_cast<size_t>(f1) * jointCount + j) * 4];
        Quat qa(a[0], a[1], a[2], a[3]);
        Quat qb(b[0], b[1], b[2], b[3]);
        /* Shortest-path: without this fix a quaternion pair on opposite hemispheres spins the
           joint the long way round mid-interpolation. */
        if (qa.dot(qb) < 0.0f) qb = qb * -1.0f;
        const Quat q = Quat(qa.x + (qb.x - qa.x) * frac, qa.y + (qb.y - qa.y) * frac,
                            qa.z + (qb.z - qa.z) * frac, qa.w + (qb.w - qa.w) * frac).normalized();
        quats[static_cast<size_t>(j) * 4 + 0] = q.x;
        quats[static_cast<size_t>(j) * 4 + 1] = q.y;
        quats[static_cast<size_t>(j) * 4 + 2] = q.z;
        quats[static_cast<size_t>(j) * 4 + 3] = q.w;
    }
    return static_cast<int32_t>(AF_OK);
}

} // namespace

int32_t af_engine_apply_pose(af_engine* engine, const af_pose* pose) {
    return guard(engine, "apply_pose", [&]() {
        if (!pose || !pose->quats) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        if (pose->joint_count != engine->rig.jointCount()) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        for (int j = 0; j < pose->joint_count; ++j) {
            for (int k = 0; k < 4; ++k) {
                engine->poseQuats[static_cast<size_t>(j) * 4 + k] = pose->quats[static_cast<size_t>(j) * 4 + k];
            }
        }
        return applyCurrentPose(engine);
    });
}

float af_engine_clip_duration(const af_engine* engine, int32_t clip) {
    /* Duration is immutable clip-library metadata; it does not read engine state. Accept a null
       handle so callers can populate animation controls while a renderer owns the engine lease. */
    (void)engine;
    if (clip == AF_CLIP_REST) return 0.0f;   /* the bind pose has no timeline */
    const int idx = clipIndexForId(clip);
    if (idx < 0) return 0.0f;
    const std::vector<Clip>& clips = builtinClips();
    if (static_cast<size_t>(idx) >= clips.size()) return 0.0f;
    return clips[static_cast<size_t>(idx)].duration;
}

int32_t af_engine_clip_name(int32_t clip, char* out_name, int32_t max_bytes) {
    if (clip == AF_CLIP_REST) {
        if (!out_name || max_bytes < 5) return out_name ? static_cast<int32_t>(AF_ERR_INVALID_ARG) : 0;
        std::snprintf(out_name, static_cast<size_t>(max_bytes), "rest");
        return 4;
    }
    const char* name = clipNameForId(clip);
    if (!name) return 0;
    const int len = static_cast<int>(std::strlen(name));
    if (!out_name) return len;
    if (max_bytes <= len) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    std::memcpy(out_name, name, static_cast<size_t>(len) + 1);
    return len;
}

int32_t af_engine_sample_clip(af_engine* engine, int32_t clip, float time_seconds, af_pose* out_pose) {
    return guard(engine, "sample_clip", [&]() {
        if (!out_pose || !out_pose->quats) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        /* af_pose carries a const pointer because it is an INPUT type for apply_pose; sampling
           writes through it. The buffer belongs to the caller either way. */
        float* dst = const_cast<float*>(out_pose->quats);
        return sampleClipInto(engine, clip, time_seconds, dst, out_pose->joint_count);
    });
}

int32_t af_engine_apply_clip(af_engine* engine, int32_t clip, float time_seconds) {
    return guard(engine, "apply_clip", [&]() {
        const int J = engine->rig.jointCount();
        if (J == 0) return static_cast<int32_t>(AF_ERR_NOT_READY);
        const int32_t rc = sampleClipInto(engine, clip, time_seconds, engine->poseQuats.data(), J);
        if (rc != AF_OK) return rc;
        return applyCurrentPose(engine);
    });
}

int32_t af_engine_apply_blendshapes(af_engine* engine, const float* weights, int32_t count) {
    return guard(engine, "apply_blendshapes", [&]() {
        if (!weights || count < 0) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        const int n = std::min(count, kBlendshapeCount);
        std::fill(engine->blendWeights.begin(), engine->blendWeights.end(), 0.0f);
        for (int i = 0; i < n; ++i) {
            engine->blendWeights[static_cast<size_t>(i)] = std::max(0.0f, std::min(1.0f, weights[i]));
        }
        /* Re-evaluate with the current pose so the deformation the caller sees includes both. */
        if (engine->rig.jointCount() > 0) return applyCurrentPose(engine);
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_blendshape_names(char names_out[][64], int32_t max_names) {
    if (!names_out || max_names <= 0) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    const int n = std::min(max_names, kBlendshapeCount);
    for (int i = 0; i < n; ++i) {
        std::snprintf(names_out[i], 64, "%s", kBlendshapeNames[i]);
    }
    return n;
}

/* ==============================================================================================
 * Accessors
 * ============================================================================================ */
int32_t af_engine_vertex_count(const af_engine* engine) {
    return engine ? static_cast<int32_t>(engine->verts.size()) : 0;
}
int32_t af_engine_index_count(const af_engine* engine) {
    return engine ? static_cast<int32_t>(engine->indices.size()) : 0;
}
const af_vertex* af_engine_vertices(const af_engine* engine) {
    return (engine && !engine->verts.empty()) ? engine->verts.data() : nullptr;
}
const af_vertex* af_engine_vertices_rest(const af_engine* engine) {
    return (engine && !engine->vertsRest.empty()) ? engine->vertsRest.data() : nullptr;
}
const uint32_t* af_engine_indices(const af_engine* engine) {
    return (engine && !engine->indices.empty()) ? engine->indices.data() : nullptr;
}
int32_t af_engine_submesh_count(const af_engine* engine) {
    return engine ? static_cast<int32_t>(engine->submeshes.size()) : 0;
}
const af_submesh* af_engine_submeshes(const af_engine* engine) {
    return (engine && !engine->submeshes.empty()) ? engine->submeshes.data() : nullptr;
}
int32_t af_engine_material_count(const af_engine* engine) {
    return engine ? static_cast<int32_t>(engine->materials.size()) : 0;
}
const af_material* af_engine_materials(const af_engine* engine) {
    return (engine && !engine->materials.empty()) ? engine->materials.data() : nullptr;
}
int32_t af_engine_joint_count(const af_engine* engine) {
    return engine ? engine->rig.jointCount() : 0;
}
int32_t af_engine_joint_info(const af_engine* engine, int32_t joint, af_joint_info* out) {
    if (!engine || !out) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    if (joint < 0 || static_cast<size_t>(joint) >= engine->joints.size()) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    *out = engine->joints[static_cast<size_t>(joint)];
    return static_cast<int32_t>(AF_OK);
}
int32_t af_engine_joint_matrices(const af_engine* engine, float* out_matrices4x4, int32_t max_joints) {
    if (!engine || !out_matrices4x4) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    const int J = engine->rig.jointCount();
    if (max_joints < J) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    const int n = std::min(J, static_cast<int>(engine->jointMatrices.size() / 16));
    std::memcpy(out_matrices4x4, engine->jointMatrices.data(), static_cast<size_t>(n) * 16 * sizeof(float));
    return n;
}
int32_t af_engine_texture(const af_engine* engine, int32_t slot, af_texture* out) {
    if (!engine || !out) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    if (slot < 0 || slot >= AF_TEX_COUNT) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    if (!engine->texValid[slot]) return static_cast<int32_t>(AF_ERR_NOT_READY);
    *out = engine->texViews[slot];
    return static_cast<int32_t>(AF_OK);
}
int32_t af_engine_stats(const af_engine* engine, af_stats* out) {
    if (!engine || !out) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    *out = engine->stats;
    return static_cast<int32_t>(AF_OK);
}

/* ==============================================================================================
 * Export
 * ============================================================================================ */
namespace {

int32_t exportToVector(af_engine* e, std::vector<uint8_t>* out) {
    if (!e->geometryReady) return static_cast<int32_t>(AF_ERR_NOT_READY);
    GlbOptions go;
    go.embedTextures = e->textures.width > 0;
    go.includeMorphTargets = e->rig.jointCount() > 0;
    go.includeAnimations = !e->clipCache.empty();
    go.groundToZero = true;
    GlbResult res;
    /* Export the BIND pose: the current mesh may be posed, and a glTF skin is defined against the
       bind pose plus joint rotations. Sending a posed mesh with a rest skeleton would double the
       deformation. */
    if (!writeGlb(e->restMesh, &e->layout, e->rig.jointCount() > 0 ? &e->rig : nullptr,
                  e->textures.width > 0 ? &e->textures : nullptr, e->metrics, e->spec, go, out, &res)) {
        e->lastError = "GLB export failed";
        return static_cast<int32_t>(AF_ERR_INTERNAL);
    }
    e->tExport = res.millis;
    /* Sized from the format string rather than guessed: the composed message is up to ~330 bytes
       with large counts, and a 200-byte buffer truncated it in the middle of a sentence (which the
       compiler had been reporting as -Wformat-truncation all along). A truncated log line is not
       a crash, but it is a lie about the export in the exact place a user would look for one. */
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "GLB %.2f MB (json %.0f KB, bin %.2f MB), %d primitives, %d materials, %d images, "
                  "%d animations, %d morph targets (%d with no motion on this build, written as empty "
                  "targets so the channel order stays canonical)",
                  res.byteSize / 1048576.0, res.jsonBytes / 1024.0, res.binBytes / 1048576.0,
                  res.primitives, res.materials, res.images, res.animations, res.morphTargets,
                  res.emptyMorphs);
    e->log(AF_LOG_INFO, buf);
    updateStats(e);
    return static_cast<int32_t>(AF_OK);
}

} // namespace

int32_t af_engine_export_glb(af_engine* engine, const char* absolute_path) {
    return guard(engine, "export_glb", [&]() {
        if (!absolute_path) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        engine->stage = AF_STAGE_EXPORT;
        engine->progress(AF_STAGE_EXPORT, 0.1f, "writing GLB");
        std::vector<uint8_t> bytes;
        const int32_t rc = exportToVector(engine, &bytes);
        if (rc != AF_OK) return rc;
        std::ofstream f(absolute_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            engine->lastError = std::string("cannot open ") + absolute_path + " for writing";
            return static_cast<int32_t>(AF_ERR_IO);
        }
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.close();
        if (!f) {
            engine->lastError = std::string("short write to ") + absolute_path;
            return static_cast<int32_t>(AF_ERR_IO);
        }
        engine->progress(AF_STAGE_EXPORT, 1.0f, "GLB written");
        return static_cast<int32_t>(AF_OK);
    });
}

int32_t af_engine_export_glb_to_memory(af_engine* engine, uint8_t** out_bytes, size_t* out_size) {
    return guard(engine, "export_glb_to_memory", [&]() {
        if (!out_bytes || !out_size) return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        *out_bytes = nullptr;
        *out_size = 0;
        std::vector<uint8_t> bytes;
        const int32_t rc = exportToVector(engine, &bytes);
        if (rc != AF_OK) return rc;
        uint8_t* block = static_cast<uint8_t*>(std::malloc(bytes.size()));
        if (!block) return static_cast<int32_t>(AF_ERR_OUT_OF_MEMORY);
        std::memcpy(block, bytes.data(), bytes.size());
        *out_bytes = block;
        *out_size = bytes.size();
        return static_cast<int32_t>(AF_OK);
    });
}


/* ==============================================================================================
 * Import
 * ----------------------------------------------------------------------------------------------
 *  Builds beside the live state and swaps at the end, so a rejected file cannot leave the engine
 *  half-replaced. See the header for what is applied and what is deliberately not.
 * ============================================================================================ */
namespace {

/* Region for an imported submesh: the exporter names materials after regions, so an engine-made
   file round-trips its regions exactly. Anything else (a foreign file, or a renamed material) is
   skin, which is the honest default - the region drives picking and the region inspector, and a
   wrong-but-plausible guess would be worse than a neutral one. */
int32_t regionForMaterialName(const char* name) {
    if (!name || !*name) return REGION_SKIN;
    for (int r = 0; r < 13; ++r) {
        if (std::strcmp(name, materialForRegion(r).name.c_str()) == 0) return r;
    }
    return REGION_SKIN;
}

int32_t importIntoEngine(af_engine* e, const uint8_t* bytes, size_t size) {
    if (!bytes || size == 0) {
        e->lastError = "import: no data";
        return static_cast<int32_t>(AF_ERR_INVALID_ARG);
    }
    GlbAsset asset;
    std::string error;
    const int rc = importGlb(bytes, size, &asset, &error);
    if (rc != AF_OK) {
        e->lastError = std::string("import: ") + (error.empty() ? "unsupported file" : error);
        return static_cast<int32_t>(rc);
    }
    if (asset.pools.empty() || asset.primitives.empty()) {
        e->lastError = "import: the file contains no drawable geometry";
        return static_cast<int32_t>(AF_ERR_UNSUPPORTED);
    }

    /* ---- vertices: the pools concatenated, one VBO the way the generated path uses one ---- */
    std::vector<af_vertex> verts;
    std::vector<uint32_t> poolBase(asset.pools.size(), 0);
    size_t total = 0;
    for (size_t i = 0; i < asset.pools.size(); ++i) {
        if (total > static_cast<size_t>(INT32_MAX) ||
            asset.pools[i].size() > static_cast<size_t>(INT32_MAX) - total) {
            e->lastError = "import: total vertex count exceeds the engine's signed 32-bit range";
            return static_cast<int32_t>(AF_ERR_UNSUPPORTED);
        }
        poolBase[i] = static_cast<uint32_t>(total);
        total += asset.pools[i].size();
    }
    verts.reserve(total);
    for (const auto& pool : asset.pools) {
        verts.insert(verts.end(), pool.begin(), pool.end());
    }

    /* ---- indices, offset into the shared buffer, grouped per primitive ---- */
    std::vector<uint32_t> indices;
    struct Range {
        int32_t region;
        int32_t material;
        int32_t vertexOffset;
        int32_t vertexCount;
        size_t indexBegin;
        size_t indexCount;
    };
    std::vector<Range> ranges;
    ranges.reserve(asset.primitives.size());
    for (const auto& prim : asset.primitives) {
        if (indices.size() > static_cast<size_t>(INT32_MAX) ||
            prim.indices.size() > static_cast<size_t>(INT32_MAX) - indices.size()) {
            e->lastError = "import: total index count exceeds the engine's signed 32-bit range";
            return static_cast<int32_t>(AF_ERR_UNSUPPORTED);
        }
        if (prim.pool < 0 || static_cast<size_t>(prim.pool) >= asset.pools.size()) {
            e->lastError = "import: primitive references a missing vertex pool";
            return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        }
        Range r{};
        r.material = prim.material;
        r.region = REGION_SKIN;
        if (prim.material >= 0 && static_cast<size_t>(prim.material) < asset.materials.size()) {
            r.region = regionForMaterialName(asset.materials[static_cast<size_t>(prim.material)].name.c_str());
        }
        r.vertexOffset = static_cast<int32_t>(poolBase[static_cast<size_t>(prim.pool)]);
        r.vertexCount = static_cast<int32_t>(asset.pools[static_cast<size_t>(prim.pool)].size());
        r.indexBegin = indices.size();
        r.indexCount = prim.indices.size();
        const uint32_t base = poolBase[static_cast<size_t>(prim.pool)];
        for (uint32_t idx : prim.indices) {
            indices.push_back(base + idx);
        }
        if (!prim.indices.empty()) ranges.push_back(r);
    }
    if (indices.empty()) {
        e->lastError = "import: the file contains no triangles";
        return static_cast<int32_t>(AF_ERR_UNSUPPORTED);
    }

    /* ---- materials ---- */
    std::vector<af_material> materials;
    for (const auto& m : asset.materials) {
        af_material am{};
        std::snprintf(am.name, sizeof(am.name), "%s", m.name.c_str());
        for (int c = 0; c < 4; ++c) am.base_color_factor[c] = m.baseColor[c];
        am.metallic = m.metallic;
        am.roughness = m.roughness;
        am.ior = 1.4f;
        am.has_base_color_tex = m.baseColorTexture >= 0 ? 1 : 0;
        am.has_normal_tex = m.normalTexture >= 0 ? 1 : 0;
        am.has_mr_tex = m.metallicRoughnessTexture >= 0 ? 1 : 0;
        am.has_ao_tex = 0;
        materials.push_back(am);
    }
    if (materials.empty()) {
        af_material am{};
        std::snprintf(am.name, sizeof(am.name), "skin");
        am.base_color_factor[0] = am.base_color_factor[1] = am.base_color_factor[2] = 0.8f;
        am.base_color_factor[3] = 1.0f;
        am.metallic = 0.0f;
        am.roughness = 0.5f;
        am.ior = 1.4f;
        materials.push_back(am);
    }

    /* ---- submeshes ---- */
    std::vector<af_submesh> submeshes;
    for (const Range& r : ranges) {
        af_submesh sm{};
        sm.region = r.region;
        sm.vertex_offset = r.vertexOffset;
        sm.vertex_count = r.vertexCount;
        sm.index_offset = static_cast<int32_t>(r.indexBegin);
        sm.index_count = static_cast<int32_t>(r.indexCount);
        sm.material_index = (r.material >= 0 && static_cast<size_t>(r.material) < materials.size())
                                ? r.material
                                : 0;
        /* Bounds from the vertices this submesh actually draws. */
        Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (size_t k = r.indexBegin; k < r.indexBegin + r.indexCount; ++k) {
            const af_vertex& v = verts[indices[k]];
            const float px = v.pos[0], py = v.pos[1], pz = v.pos[2];
            mn.x = px < mn.x ? px : mn.x;
            mn.y = py < mn.y ? py : mn.y;
            mn.z = pz < mn.z ? pz : mn.z;
            mx.x = px > mx.x ? px : mx.x;
            mx.y = py > mx.y ? py : mx.y;
            mx.z = pz > mx.z ? pz : mx.z;
        }
        if (mn.x > mx.x) { mn = Vec3(0.0f, 0.0f, 0.0f); mx = mn; }
        sm.bounds_min[0] = mn.x; sm.bounds_min[1] = mn.y; sm.bounds_min[2] = mn.z;
        sm.bounds_max[0] = mx.x; sm.bounds_max[1] = mx.y; sm.bounds_max[2] = mx.z;
        submeshes.push_back(sm);
    }

    /* Prepare the remaining allocations before mutating the live engine, so a bad_alloc during
       staging cannot leave the previous model partially replaced. */
    std::vector<af_vertex> vertsRest = verts;
    TextureSet importedTextures;
    for (size_t i = 0; i < asset.textures.size(); ++i) {
        const ImportedTexture& t = asset.textures[i];
        if (t.rgba.empty() || t.width <= 0 || t.height <= 0) continue;
        bool used = false;
        for (const auto& m : asset.materials) {
            if (m.baseColorTexture == static_cast<int>(i)) { used = true; break; }
        }
        if (!used) continue;
        importedTextures.width = t.width;
        importedTextures.height = t.height;
        importedTextures.baseColor = t.rgba;
        break;
    }

    /* ---- swap in ---- */
    e->verts.swap(verts);
    e->vertsRest.swap(vertsRest); /* imported geometry is already in its bind pose */
    e->indices.swap(indices);
    e->submeshes.swap(submeshes);
    e->materials.swap(materials);
    e->textures = std::move(importedTextures);
    for (int i = 0; i < AF_TEX_COUNT; ++i) {
        e->texViews[i] = af_texture{};
        e->texValid[i] = false;
    }

    /* The generated state that no longer describes this geometry. Cleared rather than left stale:
       a rig built for a different mesh would skin the new one with nonsense weights. */
    e->mesh = Mesh();
    e->restMesh = Mesh();
    e->layout = UVLayout();
    e->rig = Rig();
    e->joints.clear();
    e->jointMatrices.clear();
    e->poseQuats.clear();
    e->blendWeights.clear();
    e->clipCache.clear();
    e->clipFrameCount.clear();
    e->deformed.clear();
    e->field.reset();
    e->metrics = BodyMetrics();
    e->geometryReady = true;
    e->stage = AF_STAGE_DONE;
    e->dirty = 0;
    e->peakFieldBytes = 0;
    e->tFit = e->tGeometry = e->tUv = e->tTex = e->tRig = 0.0;

    /* Only adopt an embedded base-colour texture that was decoded and referenced by a material;
       every old generated/imported texture slot was invalidated at the swap above. */
    if (e->textures.width > 0 && !e->textures.baseColor.empty()) {
        af_texture tex{};
        tex.width = e->textures.width;
        tex.height = e->textures.height;
        tex.channels = 4;
        tex.mip_levels = 1;
        tex.byte_size = e->textures.baseColor.size();
        tex.data = e->textures.baseColor.data();
        e->texViews[AF_TEX_BASE_COLOR] = tex;
        e->texValid[AF_TEX_BASE_COLOR] = true;
    }

    /* What the file carried that this import does not adopt - said out loud, every time. */
    if (!asset.skins.empty()) {
        e->log(AF_LOG_WARN, "import: file contains a skeleton; it is not applied - the engine poses "
                            "with its own rig, so this model is static until it is re-rigged");
    }
    if (!asset.animations.empty()) {
        e->log(AF_LOG_WARN, "import: file contains animation clips; they are not applied (rig playback "
                            "requires a generated avatar)");
    }
    if (!asset.warnings.empty()) {
        e->log(AF_LOG_WARN, std::string("import: ") + asset.warnings.front().what + " - " +
                                asset.warnings.front().detail);
    }
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "imported %zu vertices, %zu triangles, %zu submeshes, %zu materials",
                      e->verts.size(), e->indices.size() / 3, e->submeshes.size(), e->materials.size());
        e->log(AF_LOG_INFO, msg);
    }
    updateStats(e);
    return static_cast<int32_t>(AF_OK);
}

} // namespace

int32_t af_engine_import_glb_from_memory(af_engine* engine, const uint8_t* bytes, size_t size) {
    return guard(engine, "import_glb_from_memory", [&]() {
        return importIntoEngine(engine, bytes, size);
    });
}

int32_t af_engine_import_glb(af_engine* engine, const char* absolute_path) {
    return guard(engine, "import_glb", [&]() {
        if (!absolute_path || !*absolute_path) {
            engine->lastError = "import: no path";
            return static_cast<int32_t>(AF_ERR_INVALID_ARG);
        }
        std::FILE* f = std::fopen(absolute_path, "rb");
        if (!f) {
            engine->lastError = std::string("import: cannot open ") + absolute_path;
            return static_cast<int32_t>(AF_ERR_IO);
        }
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len <= 0) {
            std::fclose(f);
            engine->lastError = "import: empty file";
            return static_cast<int32_t>(AF_ERR_IO);
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(len));
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (got != bytes.size()) {
            engine->lastError = "import: short read";
            return static_cast<int32_t>(AF_ERR_IO);
        }
        return importIntoEngine(engine, bytes.data(), bytes.size());
    });
}

void af_arena_free(void* ptr) {
    std::free(ptr);
}

/* ==============================================================================================
 * Diagnostics
 * ============================================================================================ */
const char* af_engine_last_error(const af_engine* engine) {
    if (!engine) return "null engine";
    return engine->lastError.empty() ? "" : engine->lastError.c_str();
}

const char* af_engine_version(void) { return AF_ENGINE_VERSION; }

const char* af_engine_region_name(int32_t region) { return regionLabel(region); }

} /* extern "C" */
