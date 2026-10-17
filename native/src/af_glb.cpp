#include "af_glb.h"

#include "af_png.h"
#include "afcore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace af {
namespace {

/* ---------------------------------------------------------------------------------------------
 * Container bookkeeping.
 *
 * Every binary payload goes in through addView(), which aligns to 4 bytes first and records the
 * range. Accessors then refer to views by index. Nothing tracks offsets by hand: the two rules
 * that break hand-rolled glTF writers are the 4-byte view alignment and the requirement that an
 * accessor's byteStride divide the view's length, and adding a padding step in one place makes the
 * first impossible to get wrong.
 * ------------------------------------------------------------------------------------------- */
struct BufferView {
    size_t offset = 0;
    size_t length = 0;
    int    target = 0;      /* 34962 ARRAY_BUFFER, 34963 ELEMENT_ARRAY_BUFFER, 0 = neither */
};

struct Accessor {
    int         view = -1;
    int         componentType = 5126;   /* FLOAT */
    const char* type = "SCALAR";
    size_t      count = 0;
    bool        hasBounds = false;
    float       mn[3] = {0, 0, 0};
    float       mx[3] = {0, 0, 0};
    bool        sparse = false;
    size_t      sparseCount = 0;
    int         sparseIndexView = -1;
    int         sparseValueView = -1;
};

struct GlbBuilder {
    std::vector<uint8_t>   bin;
    std::vector<BufferView> views;
    std::vector<Accessor>   accs;

    int addView(const void* data, size_t bytes, int target) {
        while ((bin.size() % 4) != 0) bin.push_back(0);
        const size_t off = bin.size();
        if (data && bytes) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            bin.insert(bin.end(), p, p + bytes);
        }
        BufferView v;
        v.offset = off;
        v.length = bytes;
        v.target = target;
        views.push_back(v);
        return static_cast<int>(views.size()) - 1;
    }
    int addAccessor(const Accessor& a) {
        accs.push_back(a);
        return static_cast<int>(accs.size()) - 1;
    }
};

/* glTF is JSON: NaN and Infinity are not numbers, and a float printed with the default precision
   can round-trip to a different value. Both are caught here rather than in a viewer. */
void jsonNum(std::string& out, double v) {
    if (!std::isfinite(v)) v = 0.0;
    char b[48];
    std::snprintf(b, sizeof(b), "%.9g", v);
    out += b;
}

void jsonEscape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
}

inline void pushU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
}

/*
 * Materials are identified by NAME, and two regions that share a name share one material: both eyes
 * are one material, both nails are one material, the ten toes of one foot are one material. The
 * table is built once, in a fixed order, so the same mesh always produces the same material list -
 * a requirement for reproducible builds, and for the export to be byte-comparable between runs.
 */
const std::vector<std::string>& materialNames() {
    static std::vector<std::string> names;
    if (names.empty()) {
        std::map<std::string, int> seen;
        for (int r = 0; r < 13; ++r) {
            const std::string n = materialForRegion(r).name;
            if (!seen.count(n)) {
                seen[n] = static_cast<int>(names.size());
                names.push_back(n);
            }
        }
    }
    return names;
}

int materialIndexForRegion(int region) {
    const std::string n = materialForRegion(region).name;
    const std::vector<std::string>& names = materialNames();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == n) return static_cast<int>(i);
    }
    return 0;
}

/* The first region that maps to a given material, used for the material's own properties. */
int representativeRegion(int material) {
    for (int r = 0; r < 13; ++r) {
        if (materialIndexForRegion(r) == material) return r;
    }
    return 0;
}

} // namespace

bool writeGlb(const Mesh& mesh, const UVLayout* layout, const Rig* rig, const TextureSet* textures,
              const BodyMetrics& metrics, const af_spec& spec, const GlbOptions& options,
              std::vector<uint8_t>* out, GlbResult* result) {
    if (!out) return false;
    const auto tStart = std::chrono::steady_clock::now();
    out->clear();
    GlbResult res;

    const size_t vcount = mesh.vertexCount();
    const size_t triCount = mesh.triangleCount();
    if (vcount == 0 || triCount == 0) return false;

    GlbBuilder b;
    b.bin.reserve(1 << 22);

    /*
     * Grounding. The engine grounds the model itself - the field's floor clamp puts the sole on
     * y = 0 - so on a correctly built avatar dy is zero and every branch below disappears. It is
     * kept because an export is a file that leaves the process: if any upstream stage ever hands
     * over a mesh standing above or below the origin, the file should still be a usable avatar
     * rather than one floating in space, and the offset is reported so a consumer can undo it.
     */
    float dy = 0.0f;
    if (options.groundToZero) {
        float minY = 1e30f;
        for (const Vec3& p : mesh.positions) minY = std::min(minY, p.y);
        if (std::isfinite(minY) && std::fabs(minY) > 1e-5f) dy = -minY;
    }
    res.groundOffsetY = dy;

    /* ==========================================================================================
     * Vertex attributes.
     * ======================================================================================== */
    std::vector<float> pos(vcount * 3);
    std::vector<float> nrm(vcount * 3);
    std::vector<float> tan4;
    std::vector<float> uvs2(vcount * 2, 0.0f);
    for (size_t i = 0; i < vcount; ++i) {
        const Vec3& p = mesh.positions[i];
        pos[i * 3 + 0] = p.x;
        pos[i * 3 + 1] = p.y + dy;
        pos[i * 3 + 2] = p.z;
        const Vec3 n = (i < mesh.normals.size()) ? mesh.normals[i] : Vec3(0, 1, 0);
        nrm[i * 3 + 0] = n.x;
        nrm[i * 3 + 1] = n.y;
        nrm[i * 3 + 2] = n.z;
    }
    if (mesh.uvs.size() == vcount * 2) {
        for (size_t i = 0; i < vcount * 2; ++i) uvs2[i] = mesh.uvs[i];
    }
    const bool haveTangents = options.includeTangents && mesh.tangents.size() == vcount;
    if (haveTangents) {
        tan4.resize(vcount * 4);
        for (size_t i = 0; i < vcount; ++i) {
            tan4[i * 4 + 0] = mesh.tangents[i].x;
            tan4[i * 4 + 1] = mesh.tangents[i].y;
            tan4[i * 4 + 2] = mesh.tangents[i].z;
            tan4[i * 4 + 3] = mesh.tangents[i].w;
        }
    }

    Accessor aPos;
    aPos.componentType = 5126; aPos.type = "VEC3"; aPos.count = vcount; aPos.hasBounds = true;
    {
        Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (size_t i = 0; i < vcount; ++i) {
            mn.x = std::min(mn.x, pos[i * 3 + 0]); mn.y = std::min(mn.y, pos[i * 3 + 1]); mn.z = std::min(mn.z, pos[i * 3 + 2]);
            mx.x = std::max(mx.x, pos[i * 3 + 0]); mx.y = std::max(mx.y, pos[i * 3 + 1]); mx.z = std::max(mx.z, pos[i * 3 + 2]);
        }
        aPos.mn[0] = mn.x; aPos.mn[1] = mn.y; aPos.mn[2] = mn.z;
        aPos.mx[0] = mx.x; aPos.mx[1] = mx.y; aPos.mx[2] = mx.z;
    }
    aPos.view = b.addView(pos.data(), pos.size() * sizeof(float), 34962);
    const int accPos = b.addAccessor(aPos);

    Accessor aNrm;
    aNrm.componentType = 5126; aNrm.type = "VEC3"; aNrm.count = vcount;
    aNrm.view = b.addView(nrm.data(), nrm.size() * sizeof(float), 34962);
    const int accNrm = b.addAccessor(aNrm);

    int accTan = -1;
    if (haveTangents) {
        Accessor a;
        a.componentType = 5126; a.type = "VEC4"; a.count = vcount;
        a.view = b.addView(tan4.data(), tan4.size() * sizeof(float), 34962);
        accTan = b.addAccessor(a);
    }
    Accessor aUv;
    aUv.componentType = 5126; aUv.type = "VEC2"; aUv.count = vcount;
    aUv.view = b.addView(uvs2.data(), uvs2.size() * sizeof(float), 34962);
    const int accUv = b.addAccessor(aUv);

    /* Skin attributes. Weights are normalised on the way out: a viewer that renormalises sees no
       difference, and one that does not still gets an affine combination. */
    int accJoints = -1, accWeights = -1;
    const bool haveSkin = rig && rig->jointCount() > 0 && rig->weights().size() == vcount;
    if (haveSkin) {
        const int jointCount = rig->jointCount();
        std::vector<uint16_t> ji(vcount * 4);
        std::vector<float> wt(vcount * 4);
        for (size_t i = 0; i < vcount; ++i) {
            const SkinWeights& sw = rig->weights()[i];
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const int j = (sw.joints[k] < jointCount) ? static_cast<int>(sw.joints[k]) : 0;
                ji[i * 4 + k] = static_cast<uint16_t>(j);
                const float w = (sw.weights[k] > 0.0f) ? sw.weights[k] : 0.0f;
                wt[i * 4 + k] = w;
                sum += w;
            }
            if (sum > 1e-6f) {
                for (int k = 0; k < 4; ++k) wt[i * 4 + k] /= sum;
            } else {
                wt[i * 4 + 0] = 1.0f;
            }
        }
        Accessor a;
        a.componentType = 5123; a.type = "VEC4"; a.count = vcount;
        a.view = b.addView(ji.data(), ji.size() * sizeof(uint16_t), 34962);
        accJoints = b.addAccessor(a);
        Accessor c;
        c.componentType = 5126; c.type = "VEC4"; c.count = vcount;
        c.view = b.addView(wt.data(), wt.size() * sizeof(float), 34962);
        accWeights = b.addAccessor(c);
    }

    /* ==========================================================================================
     * Primitives: one per material.
     * ======================================================================================== */
    std::map<int, std::vector<uint32_t>> groups;
    for (size_t t = 0; t < triCount; ++t) {
        int region = 0;
        if (!mesh.regions.empty()) {
            const int32_t r = mesh.regions[mesh.indices[t * 3]];
            region = (r >= 0 && r < 13) ? r : 0;
        }
        std::vector<uint32_t>& g = groups[materialIndexForRegion(region)];
        g.push_back(mesh.indices[t * 3 + 0]);
        g.push_back(mesh.indices[t * 3 + 1]);
        g.push_back(mesh.indices[t * 3 + 2]);
    }
    struct Primitive { int material; int accIndex; size_t indexCount; int region; };
    std::vector<Primitive> prims;
    const bool wide = vcount > 65535;
    for (auto& kv : groups) {
        std::vector<uint8_t> bytes;
        const size_t n = kv.second.size();
        if (wide) {
            bytes.resize(n * sizeof(uint32_t));
            std::memcpy(bytes.data(), kv.second.data(), n * sizeof(uint32_t));
        } else {
            bytes.resize(n * sizeof(uint16_t));
            uint16_t* p = reinterpret_cast<uint16_t*>(bytes.data());
            for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint16_t>(kv.second[i]);
        }
        Accessor a;
        a.componentType = wide ? 5125 : 5123;
        a.type = "SCALAR";
        a.count = n;
        a.view = b.addView(bytes.data(), bytes.size(), 34963);
        prims.push_back({kv.first, b.addAccessor(a), n, representativeRegion(kv.first)});
    }
    res.primitives = static_cast<int>(prims.size());
    res.materials = static_cast<int>(materialNames().size());

    /* ==========================================================================================
     * Morph targets: the 52 ARKit blendshapes, as sparse accessors.
     *
     * A facial channel moves a few thousand of 176 000 vertices. Stored densely that is 2.1 MB per
     * channel, 110 MB for the set; stored sparsely it is the deltas plus one index each. The sparse
     * accessor's implicit base value is zero, which is exactly what a morph delta wants, so there
     * is no base view at all.
     * ======================================================================================== */
    std::vector<int> morphAcc;
    std::vector<std::string> morphNames;
    if (options.includeMorphTargets && haveSkin) {
        const float kEps = 5.0e-5f;    /* 0.05 mm - below a pixel at any viewing distance */
        for (int c = 0; c < kBlendshapeCount; ++c) {
            const std::vector<Vec3>& deltas = rig->blendshapeDeltas(c);
            if (deltas.size() != vcount) continue;
            std::vector<uint32_t> idx;
            std::vector<float> vals;
            idx.reserve(4096);
            vals.reserve(4096 * 3);
            for (size_t i = 0; i < vcount; ++i) {
                const Vec3& d = deltas[i];
                if (std::fabs(d.x) > kEps || std::fabs(d.y) > kEps || std::fabs(d.z) > kEps) {
                    idx.push_back(static_cast<uint32_t>(i));
                    vals.push_back(d.x);
                    vals.push_back(d.y);
                    vals.push_back(d.z);
                }
            }
            if (idx.empty()) {
                /*
                 * The channel moves nothing on this build. It is still written, as an accessor
                 * with a count and NO bufferView - glTF's implicit default is all zeros, so the
                 * entry costs zero bytes and the file stays legal.
                 *
                 * Dropping it instead would renumber every morph target after it: glTF morph
                 * targets are positional, so a consumer that maps weight[i] to channel i (which is
                 * what ARKit, Unity, Unreal and this app's own SDK all do) would silently drive
                 * the wrong expression. The `extras.targetNames` array documents the mapping, but
                 * "documented and wrong until you read the extras" is not a defensible export.
                 */
                Accessor empty;
                empty.count = vcount;
                empty.componentType = 5126;
                empty.type = "VEC3";
                empty.view = -1;
                morphAcc.push_back(b.addAccessor(empty));
                morphNames.push_back(kBlendshapeNames[c]);
                ++res.emptyMorphs;
                continue;
            }
            Accessor a;
            a.count = vcount;
            a.componentType = 5126;
            a.type = "VEC3";
            a.sparse = true;
            a.sparseCount = idx.size();
            a.sparseIndexView = b.addView(idx.data(), idx.size() * sizeof(uint32_t), 0);
            a.sparseValueView = b.addView(vals.data(), vals.size() * sizeof(float), 0);
            morphAcc.push_back(b.addAccessor(a));
            morphNames.push_back(kBlendshapeNames[c]);
            res.sparseDeltas += static_cast<int>(idx.size());
        }
        res.morphTargets = static_cast<int>(morphAcc.size());
    }

    /* ==========================================================================================
     * Textures: five PNGs, embedded in the binary chunk.
     * ======================================================================================== */
    int imageView[5] = {-1, -1, -1, -1, -1};
    int textureSlot[5] = {-1, -1, -1, -1, -1};   /* slot -> texture index */
    if (options.embedTextures && textures && textures->width > 0 && textures->height > 0) {
        const std::vector<uint8_t>* maps[5] = {&textures->baseColor, &textures->normal,
                                               &textures->metallicRoughness, &textures->occlusion,
                                               &textures->subsurface};
        const int channels[5] = {4, 4, 4, 1, 4};
        int texIndex = 0;
        for (int m = 0; m < 5; ++m) {
            const size_t need = static_cast<size_t>(textures->width) * static_cast<size_t>(textures->height) *
                                static_cast<size_t>(channels[m]);
            if (maps[m]->size() < need) continue;
            std::vector<uint8_t> png;
            if (!encodePng(maps[m]->data(), textures->width, textures->height, channels[m], &png)) continue;
            imageView[m] = b.addView(png.data(), png.size(), 0);
            textureSlot[m] = texIndex++;
            ++res.images;
        }
    }
    const int texBaseColor = textureSlot[0];
    const int texNormal = textureSlot[1];
    const int texMR = textureSlot[2];
    const int texAO = textureSlot[3];
    const int texSSS = textureSlot[4];

    /* ==========================================================================================
     * Skeleton.
     * ======================================================================================== */
    int accIbm = -1;
    int jointCount = 0;
    if (haveSkin) {
        jointCount = rig->jointCount();
        res.joints = jointCount;
        if (rig->inverseBind().size() == static_cast<size_t>(jointCount)) {
            std::vector<float> ib(static_cast<size_t>(jointCount) * 16);
            const Mat4 shift = Mat4::translation(Vec3(0.0f, -dy, 0.0f));
            for (int j = 0; j < jointCount; ++j) {
                const Mat4 m = rig->inverseBind()[static_cast<size_t>(j)] * shift;
                std::memcpy(ib.data() + static_cast<size_t>(j) * 16, m.m, 16 * sizeof(float));
            }
            Accessor a;
            a.componentType = 5126;
            a.type = "MAT4";
            a.count = static_cast<size_t>(jointCount);
            a.view = b.addView(ib.data(), ib.size() * sizeof(float), 0);
            accIbm = b.addAccessor(a);
        }
    }

    /* ==========================================================================================
     * Animations: every clip in the library, sampled at a fixed rate.
     *
     * Sampling rather than handing over the authored keys keeps the export player-agnostic: a
     * viewer only has to implement LINEAR, which every glTF runtime does, and the quaternions are
     * already normalised and shortest-path continuous.
     * ======================================================================================== */
    struct AnimSampler { int input; int output; };
    struct AnimChannel { int node; int sampler; const char* path; };
    struct AnimOut { std::string name; float duration; bool looping; std::vector<AnimSampler> samplers; std::vector<AnimChannel> channels; };
    std::vector<AnimOut> anims;
    if (options.includeAnimations && haveSkin) {
        const float hz = (options.animationSampleHz > 1.0f) ? options.animationSampleHz : 30.0f;
        for (const Clip& clip : builtinClips()) {
            AnimOut ao;
            ao.name = clip.name;
            ao.duration = clip.duration;
            ao.looping = clip.looping;
            const int frames = std::max(2, static_cast<int>(std::ceil(clip.duration * hz)) + 1);
            std::vector<float> times(static_cast<size_t>(frames));
            for (int f = 0; f < frames; ++f) {
                times[static_cast<size_t>(f)] = clip.duration * static_cast<float>(f) / static_cast<float>(frames - 1);
            }
            Accessor at;
            at.componentType = 5126;
            at.type = "SCALAR";
            at.count = times.size();
            at.hasBounds = true;                       /* required by the spec for sampler inputs */
            at.mn[0] = times.front();
            at.mx[0] = times.back();
            at.view = b.addView(times.data(), times.size() * sizeof(float), 0);
            const int accTime = b.addAccessor(at);

            int bound = 0;
            for (const Track& tr : clip.tracks) {
                const int joint = rig->jointIndex(tr.jointName.c_str());
                if (joint < 0) continue;
                std::vector<float> quats(static_cast<size_t>(frames) * 4);
                std::vector<float> trans(static_cast<size_t>(frames) * 3);
                bool anyTranslation = false;
                for (int f = 0; f < frames; ++f) {
                    const float t = times[static_cast<size_t>(f)];
                    const Quat q = tr.sample(t);
                    quats[static_cast<size_t>(f) * 4 + 0] = q.x;
                    quats[static_cast<size_t>(f) * 4 + 1] = q.y;
                    quats[static_cast<size_t>(f) * 4 + 2] = q.z;
                    quats[static_cast<size_t>(f) * 4 + 3] = q.w;
                    const Vec3 tv = tr.sampleTranslation(t);
                    trans[static_cast<size_t>(f) * 3 + 0] = tv.x;
                    trans[static_cast<size_t>(f) * 3 + 1] = tv.y + ((joint == 0) ? dy : 0.0f);
                    trans[static_cast<size_t>(f) * 3 + 2] = tv.z;
                    if (std::fabs(tv.x) > 1e-6f || std::fabs(tv.y) > 1e-6f || std::fabs(tv.z) > 1e-6f) {
                        anyTranslation = true;
                    }
                }
                Accessor aq;
                aq.componentType = 5126;
                aq.type = "VEC4";
                aq.count = static_cast<size_t>(frames);
                aq.view = b.addView(quats.data(), quats.size() * sizeof(float), 0);
                const int accQuat = b.addAccessor(aq);
                ao.channels.push_back({joint, static_cast<int>(ao.samplers.size()), "rotation"});
                ao.samplers.push_back({accTime, accQuat});
                if (anyTranslation) {
                    Accessor atr;
                    atr.componentType = 5126;
                    atr.type = "VEC3";
                    atr.count = static_cast<size_t>(frames);
                    atr.view = b.addView(trans.data(), trans.size() * sizeof(float), 0);
                    const int accT = b.addAccessor(atr);
                    ao.channels.push_back({joint, static_cast<int>(ao.samplers.size()), "translation"});
                    ao.samplers.push_back({accTime, accT});
                }
                ++bound;
            }
            if (bound > 0) {
                anims.push_back(std::move(ao));
                ++res.animations;
            }
        }
    }

    /* ==========================================================================================
     * JSON.
     * ======================================================================================== */
    std::string j;
    j.reserve(1u << 17);
    j += "{\"asset\":{\"version\":\"2.0\",\"generator\":\"AvatarForge ";
    jsonEscape(j, AF_ENGINE_VERSION);
    j += "\"},\"scene\":0,\"scenes\":[{\"name\":\"AvatarForgeAvatar\",\"nodes\":[";

    /* Node layout: joints 0..J-1, then the mesh node at index J. Keeping the joints first means a
       joint's glTF node index equals its rig index, which makes the animation channels readable. */
    const int meshNode = jointCount;
    if (jointCount > 0) {
        j += "0,";
    }
    j += std::to_string(meshNode);
    j += "]}],\"nodes\":[";

    if (haveSkin) {
        const std::vector<Joint>& joints = rig->joints();
        for (int jn = 0; jn < jointCount; ++jn) {
            const Joint& jt = joints[static_cast<size_t>(jn)];
            if (jn) j += ',';
            j += "{\"name\":\"";
            jsonEscape(j, jt.name);
            j += "\",\"translation\":[";
            jsonNum(j, jt.restTranslation.x);
            j += ',';
            /* The root absorbs the ground offset: children are parent-relative, so shifting the
               root shifts the whole skeleton, and the inverse bind matrices are shifted to match. */
            jsonNum(j, jt.restTranslation.y + ((jt.parent < 0) ? dy : 0.0f));
            j += ',';
            jsonNum(j, jt.restTranslation.z);
            j += "],\"rotation\":[";
            jsonNum(j, jt.restRotation.x); j += ',';
            jsonNum(j, jt.restRotation.y); j += ',';
            jsonNum(j, jt.restRotation.z); j += ',';
            jsonNum(j, jt.restRotation.w);
            j += "],\"scale\":[1,1,1]";
            std::string children;
            for (int c = 0; c < jointCount; ++c) {
                if (joints[static_cast<size_t>(c)].parent == jn) {
                    if (!children.empty()) children += ',';
                    children += std::to_string(c);
                }
            }
            if (!children.empty()) {
                j += ",\"children\":[";
                j += children;
                j += ']';
            }
            j += '}';
        }
        j += ',';
    }
    j += "{\"name\":\"AvatarMesh\",\"mesh\":0";
    if (accIbm >= 0) j += ",\"skin\":0";
    j += "}],";

    if (accIbm >= 0) {
        j += "\"skins\":[{\"name\":\"AvatarSkeleton\",\"inverseBindMatrices\":";
        j += std::to_string(accIbm);
        j += ",\"skeleton\":0,\"joints\":[";
        for (int jn = 0; jn < jointCount; ++jn) {
            if (jn) j += ',';
            j += std::to_string(jn);
        }
        j += "]}],";
    }

    /* mesh + primitives */
    j += "\"meshes\":[{\"name\":\"Avatar\",\"primitives\":[";
    for (size_t pi = 0; pi < prims.size(); ++pi) {
        const Primitive& pr = prims[pi];
        if (pi) j += ',';
        j += "{\"attributes\":{\"POSITION\":";
        j += std::to_string(accPos);
        j += ",\"NORMAL\":";
        j += std::to_string(accNrm);
        if (accTan >= 0) {
            j += ",\"TANGENT\":";
            j += std::to_string(accTan);
        }
        j += ",\"TEXCOORD_0\":";
        j += std::to_string(accUv);
        if (accJoints >= 0 && accWeights >= 0) {
            j += ",\"JOINTS_0\":";
            j += std::to_string(accJoints);
            j += ",\"WEIGHTS_0\":";
            j += std::to_string(accWeights);
        }
        j += "},\"indices\":";
        j += std::to_string(pr.accIndex);
        j += ",\"material\":";
        j += std::to_string(pr.material);
        j += ",\"mode\":4";
        if (!morphAcc.empty()) {
            j += ",\"targets\":[";
            for (size_t c = 0; c < morphAcc.size(); ++c) {
                if (c) j += ',';
                j += "{\"POSITION\":";
                j += std::to_string(morphAcc[c]);
                j += '}';
            }
            j += ']';
        }
        j += ",\"extras\":{\"region\":";
        j += std::to_string(pr.region);
        j += ",\"triangles\":";
        j += std::to_string(pr.indexCount / 3);
        j += "}}";
    }
    j += "]";
    /*
     * Morph target names and neutral weights live INSIDE the mesh object, next to the primitives
     * that reference the targets. Emitting them as siblings of "meshes" (which is valid JSON and
     * therefore easy to do by accident) is silently ignored by every viewer.
     */
    if (!morphAcc.empty()) {
        j += ",\"weights\":[";
        for (size_t c = 0; c < morphAcc.size(); ++c) {
            if (c) j += ',';
            j += '0';
        }
        j += ']';
        j += ",\"extras\":{\"targetNames\":[";
        for (size_t c = 0; c < morphNames.size(); ++c) {
            if (c) j += ',';
            j += '"';
            jsonEscape(j, morphNames[c]);
            j += '"';
        }
        j += "]}";
    }
    /* close the mesh object, then the meshes array */
    j += "}],";

    /* materials */
    {
        const std::vector<std::string>& names = materialNames();
        j += "\"materials\":[";
        for (size_t mi = 0; mi < names.size(); ++mi) {
            const MaterialDef& md = materialForRegion(representativeRegion(static_cast<int>(mi)));
            if (mi) j += ',';
            j += "{\"name\":\"";
            jsonEscape(j, names[mi]);
            j += "\",\"doubleSided\":false,\"pbrMetallicRoughness\":{\"baseColorFactor\":[";
            jsonNum(j, md.baseColor.x); j += ',';
            jsonNum(j, md.baseColor.y); j += ',';
            jsonNum(j, md.baseColor.z);
            j += ",1],\"metallicFactor\":";
            jsonNum(j, md.metallic);
            j += ",\"roughnessFactor\":";
            jsonNum(j, md.roughness);
            if (texBaseColor >= 0) {
                j += ",\"baseColorTexture\":{\"index\":";
                j += std::to_string(texBaseColor);
                j += "}";
            }
            if (texMR >= 0) {
                j += ",\"metallicRoughnessTexture\":{\"index\":";
                j += std::to_string(texMR);
                j += "}";
            }
            j += '}';
            if (texNormal >= 0) {
                j += ",\"normalTexture\":{\"index\":";
                j += std::to_string(texNormal);
                j += ",\"scale\":1}";
            }
            if (texAO >= 0) {
                j += ",\"occlusionTexture\":{\"index\":";
                j += std::to_string(texAO);
                j += ",\"strength\":1}";
            }
            j += ",\"extensions\":{\"KHR_materials_ior\":{\"ior\":";
            jsonNum(j, md.ior);
            j += '}';
            if (md.sheen > 1e-3f) {
                j += ",\"KHR_materials_sheen\":{\"sheenColorFactor\":[";
                jsonNum(j, md.sheenTint); j += ',';
                jsonNum(j, md.sheenTint); j += ',';
                jsonNum(j, md.sheenTint);
                j += "],\"sheenRoughnessFactor\":";
                jsonNum(j, 1.0f - 0.5f * md.sheen);
                j += '}';
            }
            if (texSSS >= 0 && md.subsurface > 1e-3f) {
                /* Subsurface scattering is not in core glTF 2.0. The map and the per-channel
                   scatter radii travel in a namespaced extension; renderers that do not know it
                   ignore it and still see a correct opaque material. */
                j += ",\"AF_subsurface\":{\"subsurfaceTexture\":{\"index\":";
                j += std::to_string(texSSS);
                j += "},\"subsurfaceFactor\":";
                jsonNum(j, md.subsurface);
                j += ",\"scatterRadiusMeters\":[";
                jsonNum(j, static_cast<double>(metrics.sssRadius)); j += ',';
                jsonNum(j, static_cast<double>(metrics.sssRadius) * 0.5); j += ',';
                jsonNum(j, static_cast<double>(metrics.sssRadius) * 0.25);
                j += "]}";
            }
            j += "},\"extras\":{\"region\":";
            j += std::to_string(representativeRegion(static_cast<int>(mi)));
            j += "}}";
        }
        j += "],";
    }

    /* textures */
    if (res.images > 0) {
        j += "\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}],";
        j += "\"images\":[";
        bool first = true;
        for (int m = 0; m < 5; ++m) {
            if (imageView[m] < 0) continue;
            if (!first) j += ',';
            first = false;
            j += "{\"mimeType\":\"image/png\",\"name\":\"";
            j += (m == 0 ? "baseColor" : m == 1 ? "normal" : m == 2 ? "metallicRoughness" :
                  m == 3 ? "occlusion" : "subsurface");
            j += "\",\"bufferView\":";
            j += std::to_string(imageView[m]);
            j += '}';
        }
        j += "],\"textures\":[";
        first = true;
        for (int m = 0; m < 5; ++m) {
            if (imageView[m] < 0) continue;
            if (!first) j += ',';
            first = false;
            j += "{\"sampler\":0,\"source\":";
            j += std::to_string(textureSlot[m]);
            j += '}';
        }
        j += "],";
    }

    /* animations */
    if (!anims.empty()) {
        j += "\"animations\":[";
        for (size_t ai = 0; ai < anims.size(); ++ai) {
            const AnimOut& ao = anims[ai];
            if (ai) j += ',';
            j += "{\"name\":\"";
            jsonEscape(j, ao.name);
            j += "\",\"samplers\":[";
            for (size_t si = 0; si < ao.samplers.size(); ++si) {
                if (si) j += ',';
                j += "{\"input\":";
                j += std::to_string(ao.samplers[si].input);
                j += ",\"output\":";
                j += std::to_string(ao.samplers[si].output);
                j += ",\"interpolation\":\"LINEAR\"}";
            }
            j += "],\"channels\":[";
            for (size_t ci = 0; ci < ao.channels.size(); ++ci) {
                const AnimChannel& c = ao.channels[ci];
                if (ci) j += ',';
                j += "{\"sampler\":";
                j += std::to_string(c.sampler);
                j += ",\"target\":{\"node\":";
                j += std::to_string(c.node);
                j += ",\"path\":\"";
                j += c.path;
                j += "\"}}";
            }
            j += "],\"extras\":{\"duration\":";
            jsonNum(j, ao.duration);
            j += ",\"looping\":";
            j += ao.looping ? "true" : "false";
            j += "}}";
        }
        j += "],";
    }

    /* extras: the provenance of the file */
    j += "\"extras\":{\"engine\":\"";
    jsonEscape(j, AF_ENGINE_VERSION);
    j += "\",\"abi\":";
    j += std::to_string(AF_ABI_VERSION);
    j += ",\"spec\":{\"sex\":";
    j += std::to_string(spec.sex);
    j += ",\"build\":";
    j += std::to_string(spec.build);
    j += ",\"height_m\":";
    jsonNum(j, spec.height_m);
    j += ",\"age_years\":";
    jsonNum(j, spec.age_years);
    j += ",\"muscle\":";
    jsonNum(j, spec.muscle);
    j += ",\"body_fat\":";
    jsonNum(j, spec.body_fat);
    j += ",\"melanin\":";
    jsonNum(j, spec.skin_melanin);
    j += ",\"outfit\":";
    j += std::to_string(spec.outfit);
    j += ",\"footwear\":";
    j += std::to_string(spec.footwear);
    j += ",\"hair_style\":";
    j += std::to_string(spec.hair_style);
    j += ",\"seed\":";
    j += std::to_string(static_cast<unsigned long long>(spec.seed));
    if (spec.prompt[0] != '\0') {
        j += ",\"prompt\":\"";
        jsonEscape(j, std::string(spec.prompt));
        j += '"';
    }
    j += "},\"landmarks\":{\"crown\":";
    jsonNum(j, metrics.y.crownY);
    j += ",\"chin\":";
    jsonNum(j, metrics.y.chinY);
    j += ",\"shoulder\":";
    jsonNum(j, metrics.y.shoulderY);
    j += ",\"hip\":";
    jsonNum(j, metrics.y.hipY);
    j += ",\"ankle\":";
    jsonNum(j, metrics.y.ankleY);
    j += "},\"counts\":{\"vertices\":";
    j += std::to_string(vcount);
    j += ",\"triangles\":";
    j += std::to_string(triCount);
    j += ",\"joints\":";
    j += std::to_string(res.joints);
    j += ",\"blendshapes\":";
    j += std::to_string(res.morphTargets);
    j += ",\"emptyBlendshapes\":";
    j += std::to_string(res.emptyMorphs);
    j += ",\"clips\":";
    j += std::to_string(res.animations);
    j += ",\"textureSize\":";
    j += std::to_string(textures ? textures->width : 0);
    j += "},\"uv\":{\"charts\":";
    j += std::to_string(layout ? layout->charts.size() : 0);
    j += ",\"coverage\":";
    jsonNum(j, layout ? layout->coverage : 0.0f);
    j += ",\"texelsPerMeter\":";
    jsonNum(j, layout ? layout->texelsPerMeter : 0.0f);
    j += "},\"groundOffsetY\":";
    jsonNum(j, res.groundOffsetY);
    j += "}";

    /* extensionsUsed: only those actually emitted. */
    {
        std::string used = "\"extensionsUsed\":[\"KHR_materials_ior\"";
        bool sheen = false, sss = false;
        const std::vector<std::string>& names = materialNames();
        for (size_t mi = 0; mi < names.size(); ++mi) {
            const MaterialDef& md = materialForRegion(representativeRegion(static_cast<int>(mi)));
            if (md.sheen > 1e-3f) sheen = true;
            if (texSSS >= 0 && md.subsurface > 1e-3f) sss = true;
        }
        if (sheen) used += ",\"KHR_materials_sheen\"";
        if (sss) used += ",\"AF_subsurface\"";
        used += "]";
        j += ',';
        j += used;
    }
    j += '}';

    /* ==========================================================================================
     * Accessors, buffer views and the buffer, appended before the closing brace.
     * ======================================================================================== */
    std::string tail;
    tail.reserve(1u << 16);
    tail += ",\"accessors\":[";
    for (size_t i = 0; i < b.accs.size(); ++i) {
        const Accessor& a = b.accs[i];
        if (i) tail += ',';
        tail += "{\"componentType\":";
        tail += std::to_string(a.componentType);
        tail += ",\"count\":";
        tail += std::to_string(a.count);
        tail += ",\"type\":\"";
        tail += a.type;
        tail += '"';
        if (a.view >= 0) {
            tail += ",\"bufferView\":";
            tail += std::to_string(a.view);
        }
        if (a.hasBounds) {
            tail += ",\"min\":[";
            jsonNum(tail, a.mn[0]); tail += ',';
            jsonNum(tail, a.mn[1]); tail += ',';
            jsonNum(tail, a.mn[2]);
            tail += "],\"max\":[";
            jsonNum(tail, a.mx[0]); tail += ',';
            jsonNum(tail, a.mx[1]); tail += ',';
            jsonNum(tail, a.mx[2]);
            tail += ']';
        }
        if (a.sparse) {
            tail += ",\"sparse\":{\"count\":";
            tail += std::to_string(a.sparseCount);
            tail += ",\"indices\":{\"bufferView\":";
            tail += std::to_string(a.sparseIndexView);
            tail += ",\"componentType\":5125},\"values\":{\"bufferView\":";
            tail += std::to_string(a.sparseValueView);
            tail += "}}";
        }
        tail += '}';
    }
    tail += "],\"bufferViews\":[";
    for (size_t i = 0; i < b.views.size(); ++i) {
        const BufferView& v = b.views[i];
        if (i) tail += ',';
        tail += "{\"buffer\":0,\"byteOffset\":";
        tail += std::to_string(v.offset);
        tail += ",\"byteLength\":";
        tail += std::to_string(v.length);
        if (v.target) {
            tail += ",\"target\":";
            tail += std::to_string(v.target);
        }
        tail += '}';
    }
    tail += "],\"buffers\":[{\"byteLength\":";
    tail += std::to_string(b.bin.size());
    tail += "}]}";

    /* Splice: drop the document's closing brace and append the arrays. */
    if (j.size() < 2 || j[j.size() - 1] != '}') return false;
    j.resize(j.size() - 1);
    j += tail;

    while ((j.size() % 4) != 0) j += ' ';              /* JSON chunk pads with spaces */
    while ((b.bin.size() % 4) != 0) b.bin.push_back(0); /* BIN chunk pads with zeros   */

    const size_t total = 12 + 8 + j.size() + 8 + b.bin.size();
    out->reserve(total);
    pushU32(*out, 0x46546C67u);                        /* "glTF" */
    pushU32(*out, 2u);
    pushU32(*out, static_cast<uint32_t>(total));
    pushU32(*out, static_cast<uint32_t>(j.size()));
    pushU32(*out, 0x4E4F534Au);                        /* "JSON" */
    out->insert(out->end(), j.begin(), j.end());
    pushU32(*out, static_cast<uint32_t>(b.bin.size()));
    pushU32(*out, 0x004E4942u);                        /* "BIN\0" */
    out->insert(out->end(), b.bin.begin(), b.bin.end());

    res.byteSize = out->size();
    res.jsonBytes = j.size();
    res.binBytes = b.bin.size();
    res.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
    if (result) *result = res;
    return true;
}

} // namespace af
