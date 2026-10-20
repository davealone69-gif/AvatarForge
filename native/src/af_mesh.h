/*
 * af_mesh.h -- mesh container + the geometry-processing operations the pipeline needs:
 * welding, manifold repair, normals/tangents, Laplacian smoothing, cluster decimation,
 * region splitting, BVH build and queries, and ray casting for garment collision resolution.
 *
 * Header-only and dependency free; used on-device and by the host-side test harness.
 */
#ifndef AF_MESH_H
#define AF_MESH_H

#include "af_math.h"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstring>

namespace af {

/* ------------------------------------------------------------------ region / material ---- */
enum Region : int32_t {
    REGION_SKIN = 0,          /* body + face + scalp; SSS skin shader                        */
    REGION_EYE_L = 1,
    REGION_EYE_R = 2,
    REGION_TEETH = 3,
    REGION_TONGUE = 4,
    REGION_LIPS = 5,
    REGION_HAIR = 6,
    REGION_LASH = 7,          /* eyelashes + eyebrows (fibre shader)                         */
    REGION_NAIL = 8,
    REGION_CLOTH_UPPER = 9,
    REGION_CLOTH_LOWER = 10,
    REGION_SHOE = 11,
    REGION_GEAR = 12,         /* armour plates, hardware, metal                            */
    REGION_COUNT = 13
};

inline const char* regionName(int r) {
    switch (r) {
        case REGION_SKIN: return "body";
        case REGION_EYE_L: return "eye_L";
        case REGION_EYE_R: return "eye_R";
        case REGION_TEETH: return "teeth";
        case REGION_TONGUE: return "tongue";
        case REGION_LIPS: return "lips";
        case REGION_HAIR: return "hair";
        case REGION_LASH: return "lash_brow";
        case REGION_NAIL: return "nails";
        case REGION_CLOTH_UPPER: return "cloth_upper";
        case REGION_CLOTH_LOWER: return "cloth_lower";
        case REGION_SHOE: return "footwear";
        case REGION_GEAR: return "equipment";
        default: return "unknown";
    }
}

/* ------------------------------------------------------------------ Mesh ----------------- */
struct Mesh {
    std::vector<Vec3>  positions;
    std::vector<Vec3>  normals;
    std::vector<Vec4>  tangents;      /* xyz + handedness in w                             */
    std::vector<float> uvs;           /* 2 floats per vertex                               */
    std::vector<int32_t> regions;     /* 1 per vertex; drives material + submesh split      */
    std::vector<float> curvature;     /* cavity/AO term, 1 per vertex                       */
    std::vector<uint32_t> indices;    /* triangles                                          */

    size_t vertexCount() const { return positions.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
    bool empty() const { return positions.empty() || indices.empty(); }

    void clear() {
        positions.clear(); normals.clear(); tangents.clear(); uvs.clear();
        regions.clear(); curvature.clear(); indices.clear();
    }

    void reserveVertices(size_t n) {
        positions.reserve(n); normals.reserve(n); tangents.reserve(n);
        uvs.reserve(n); regions.reserve(n); curvature.reserve(n);
    }

    /** Appends `other` transformed by `xform`, offsetting indices. Returns first new index. */
    uint32_t append(const Mesh& other, const Mat4& xform, int regionOverride = -1) {
        const uint32_t base = static_cast<uint32_t>(positions.size());
        const size_t n = other.vertexCount();
        reserveVertices(n);
        const Mat4 nrm = xform.inverse().transposed();
        for (size_t i = 0; i < n; ++i) {
            positions.push_back(xform.transformPoint(other.positions[i]));
            normals.push_back(other.normals.empty()
                                  ? Vec3(0, 1, 0)
                                  : nrm.transformDirection(other.normals[i]).normalizedOr({0, 1, 0}));
            tangents.push_back(other.tangents.empty() ? Vec4(1, 0, 0, 1)
                                                      : Vec4(nrm.transformDirection(other.tangents[i].xyz()).normalizedOr({1, 0, 0}),
                                                             other.tangents[i].w));
            if (other.uvs.size() >= (i + 1) * 2) {
                uvs.push_back(other.uvs[i * 2]);
                uvs.push_back(other.uvs[i * 2 + 1]);
            } else {
                uvs.push_back(0.0f); uvs.push_back(0.0f);
            }
            regions.push_back(regionOverride >= 0 ? regionOverride
                                                  : (i < other.regions.size() ? other.regions[i] : REGION_SKIN));
            curvature.push_back(i < other.curvature.size() ? other.curvature[i] : 1.0f);
        }
        for (uint32_t idx : other.indices) indices.push_back(base + idx);
        return base;
    }

    void computeBounds(Vec3* outMin, Vec3* outMax) const {
        Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (const Vec3& p : positions) { mn = mn.minv(p); mx = mx.maxv(p); }
        if (positions.empty()) { mn = Vec3(-1, -1, -1); mx = Vec3(1, 1, 1); }
        *outMin = mn; *outMax = mx;
    }

    /** Area-weighted smooth vertex normals. Safe on non-manifold input. */
    void computeNormals() {
        const size_t n = positions.size();
        normals.assign(n, Vec3(0, 0, 0));
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
            const Vec3 a = positions[i0], b = positions[i1], c = positions[i2];
            const Vec3 fn = (b - a).cross(c - a);   /* length proportional to 2*area */
            if (!fn.isFinite()) continue;
            normals[i0] += fn; normals[i1] += fn; normals[i2] += fn;
        }
        for (size_t i = 0; i < n; ++i) normals[i] = normals[i].normalizedOr({0, 1, 0});
    }

    /** Per-vertex ambient-occlusion-ish curvature term in [0,1] (1 = exposed, 0 = deep crease). */
    void computeCurvature(int iterations = 2) {
        const size_t n = positions.size();
        std::vector<Vec3> avg(n, Vec3(0, 0, 0));
        std::vector<uint32_t> deg(n, 0);
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                const uint32_t i = indices[t + e], j = indices[t + (e + 1) % 3];
                avg[i] += positions[j]; deg[i]++;
                avg[j] += positions[i]; deg[j]++;
            }
        }
        std::vector<Vec3> lap(n);
        for (size_t i = 0; i < n; ++i) {
            lap[i] = deg[i] ? (avg[i] / static_cast<float>(deg[i]) - positions[i]) : Vec3(0, 0, 0);
        }
        curvature.assign(n, 1.0f);
        for (size_t i = 0; i < n; ++i) {
            const float conv = lap[i].dot(normals.empty() ? Vec3(0, 1, 0) : normals[i]);
            curvature[i] = saturate(0.5f + conv * 40.0f);
        }
        for (int it = 0; it < iterations; ++it) {
            std::vector<float> next(n);
            for (size_t i = 0; i < n; ++i) next[i] = curvature[i];
            for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                for (int e = 0; e < 3; ++e) {
                    const uint32_t i = indices[t + e], j = indices[t + (e + 1) % 3];
                    next[i] += curvature[j];
                    next[j] += curvature[i];
                }
            }
            for (size_t i = 0; i < n; ++i) {
                const float w = deg[i] ? 1.0f / static_cast<float>(deg[i] + 1) : 1.0f;
                curvature[i] = saturate(curvature[i] * 0.5f + next[i] * w * 0.5f);
            }
        }
    }

    /** MikkTSpace-equivalent tangents from the UV chart (fallback: a stable orthonormal basis). */
    void computeTangents() {
        const size_t n = positions.size();
        std::vector<Vec3> tan(n, Vec3(0, 0, 0)), bit(n, Vec3(0, 0, 0));
        if (uvs.size() < n * 2) {
            tangents.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const Vec3 nrm = normals.empty() ? Vec3(0, 1, 0) : normals[i];
                Vec3 t = Vec3(0, 0, 1).cross(nrm);
                if (t.lengthSq() < 1e-6f) t = Vec3(1, 0, 0);
                tangents[i] = Vec4(t.normalized(), 1.0f);
            }
            return;
        }
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
            const Vec3 p0 = positions[i0], p1 = positions[i1], p2 = positions[i2];
            const float u0 = uvs[i0 * 2], v0 = uvs[i0 * 2 + 1];
            const float u1 = uvs[i1 * 2], v1 = uvs[i1 * 2 + 1];
            const float u2 = uvs[i2 * 2], v2 = uvs[i2 * 2 + 1];
            const Vec3 e1 = p1 - p0, e2 = p2 - p0;
            const float du1 = u1 - u0, dv1 = v1 - v0;
            const float du2 = u2 - u0, dv2 = v2 - v0;
            const float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-12f) continue;
            const float r = 1.0f / det;
            const Vec3 t3 = (e1 * dv2 - e2 * dv1) * r;
            const Vec3 b3 = (e2 * du1 - e1 * du2) * r;
            tan[i0] += t3; tan[i1] += t3; tan[i2] += t3;
            bit[i0] += b3; bit[i1] += b3; bit[i2] += b3;
        }
        tangents.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec3 nrm = normals.empty() ? Vec3(0, 1, 0) : normals[i];
            Vec3 t = tan[i];
            if (t.lengthSq() < 1e-10f) {
                t = Vec3(0, 0, 1).cross(nrm);
                if (t.lengthSq() < 1e-8f) t = Vec3(1, 0, 0);
                tangents[i] = Vec4(t.normalized(), 1.0f);
                continue;
            }
            t = (t - nrm * nrm.dot(t)).normalizedOr({1, 0, 0});
            const float w = (nrm.cross(t).dot(bit[i]) < 0.0f) ? -1.0f : 1.0f;
            tangents[i] = Vec4(t, w);
        }
    }

    /** Weld coincident vertices; returns mapping old->new in `remap` and drops degenerates. */
    void weld(float epsilon = 1e-5f) {
        const float invEps = 1.0f / epsilon;
        std::unordered_map<uint64_t, uint32_t> table;
        table.reserve(positions.size() * 2);
        std::vector<uint32_t> remap(positions.size());
        std::vector<Vec3> np; std::vector<Vec3> nn; std::vector<Vec4> nt;
        std::vector<float> nu; std::vector<int32_t> nr; std::vector<float> nc;
        np.reserve(positions.size()); nn.reserve(positions.size()); nt.reserve(positions.size());
        nu.reserve(uvs.size()); nr.reserve(positions.size()); nc.reserve(positions.size());

        for (size_t i = 0; i < positions.size(); ++i) {
            const Vec3& p = positions[i];
            const int64_t xi = static_cast<int64_t>(std::llround(p.x * invEps));
            const int64_t yi = static_cast<int64_t>(std::llround(p.y * invEps));
            const int64_t zi = static_cast<int64_t>(std::llround(p.z * invEps));
            const uint64_t key = (static_cast<uint64_t>(xi & 0x1FFFFF) << 42) ^
                                 (static_cast<uint64_t>(yi & 0x1FFFFF) << 21) ^
                                 static_cast<uint64_t>(zi & 0x1FFFFF);
            auto it = table.find(key);
            if (it != table.end()) {
                remap[i] = it->second;
            } else {
                const uint32_t ni = static_cast<uint32_t>(np.size());
                table.emplace(key, ni);
                remap[i] = ni;
                np.push_back(p);
                nn.push_back(i < normals.size() ? normals[i] : Vec3(0, 1, 0));
                nt.push_back(i < tangents.size() ? tangents[i] : Vec4(1, 0, 0, 1));
                nu.push_back(i * 2 + 1 < uvs.size() ? uvs[i * 2] : 0.0f);
                nu.push_back(i * 2 + 1 < uvs.size() ? uvs[i * 2 + 1] : 0.0f);
                nr.push_back(i < regions.size() ? regions[i] : REGION_SKIN);
                nc.push_back(i < curvature.size() ? curvature[i] : 1.0f);
            }
        }
        std::vector<uint32_t> ni;
        ni.reserve(indices.size());
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t a = remap[indices[t]], b = remap[indices[t + 1]], c = remap[indices[t + 2]];
            if (a == b || b == c || a == c) continue;   /* degenerate after weld */
            ni.push_back(a); ni.push_back(b); ni.push_back(c);
        }
        positions.swap(np); normals.swap(nn); tangents.swap(nt);
        uvs.swap(nu); regions.swap(nr); curvature.swap(nc); indices.swap(ni);
    }

    /** Removes unreferenced vertices and compacts the buffers. */
    void compact() {
        const size_t n = positions.size();
        std::vector<int32_t> map(n, -1);
        for (uint32_t idx : indices) {
            if (idx < n) map[idx] = 0;
        }
        std::vector<Vec3> np; std::vector<Vec3> nn; std::vector<Vec4> nt;
        std::vector<float> nu; std::vector<int32_t> nr; std::vector<float> nc;
        np.reserve(n); nn.reserve(n); nt.reserve(n); nu.reserve(uvs.size());
        nr.reserve(n); nc.reserve(n);
        int32_t next = 0;
        for (size_t i = 0; i < n; ++i) {
            if (map[i] != 0) continue;
            map[i] = next++;
            np.push_back(positions[i]);
            nn.push_back(i < normals.size() ? normals[i] : Vec3(0, 1, 0));
            nt.push_back(i < tangents.size() ? tangents[i] : Vec4(1, 0, 0, 1));
            nu.push_back(i * 2 + 1 < uvs.size() ? uvs[i * 2] : 0.0f);
            nu.push_back(i * 2 + 1 < uvs.size() ? uvs[i * 2 + 1] : 0.0f);
            nr.push_back(i < regions.size() ? regions[i] : REGION_SKIN);
            nc.push_back(i < curvature.size() ? curvature[i] : 1.0f);
        }
        std::vector<uint32_t> ni;
        ni.reserve(indices.size());
        for (uint32_t idx : indices) {
            const int32_t m = (idx < n) ? map[idx] : -1;
            if (m >= 0) ni.push_back(static_cast<uint32_t>(m));
        }
        positions.swap(np); normals.swap(nn); tangents.swap(nt);
        uvs.swap(nu); regions.swap(nr); curvature.swap(nc); indices.swap(ni);
    }

    /** Rotates the winding of every triangle (used when a grid hands us inside-out winding). */
    void flipWinding() {
        for (size_t t = 0; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
    }

    /** Taubin (lambda/mu) smoothing: shrinks less than plain Laplacian, keeps volume. */
    void taubinSmooth(int iterations, float lambda = 0.5f, float mu = -0.53f) {
        const size_t n = positions.size();
        if (n == 0) return;
        std::vector<Vec3> sum(n);
        for (int it = 0; it < iterations; ++it) {
            for (int pass = 0; pass < 2; ++pass) {
                const float f = pass == 0 ? lambda : mu;
                std::fill(sum.begin(), sum.end(), Vec3(0, 0, 0));
                std::vector<int32_t> deg(n, 0);
                for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                    for (int e = 0; e < 3; ++e) {
                        const uint32_t i = indices[t + e], j = indices[t + (e + 1) % 3];
                        sum[i] += positions[j]; deg[i]++;
                        sum[j] += positions[i]; deg[j]++;
                    }
                }
                for (size_t i = 0; i < n; ++i) {
                    if (deg[i] == 0) continue;
                    const Vec3 target = sum[i] / static_cast<float>(deg[i]);
                    positions[i] += (target - positions[i]) * f;
                }
            }
        }
    }

    /**
     * Deterministic vertex-clustering decimation used to prepare candidate LOD meshes.
     *
     * This is an approximate simplifier, not QEM: it averages positions within spatial cells and
     * drops collapsed triangles. It can alter silhouettes and topology, so callers must validate
     * the result for their asset before using it. The cell count is derived from the requested
     * triangle budget (surface triangle count scales approximately with cells squared), capped to
     * keep the packed cluster key bounded. Material regions are kept in separate clusters.
     */
    void clusterDecimate(int targetTriangles) {
        const size_t sourceTriangles = triangleCount();
        if (targetTriangles <= 0 || sourceTriangles <= static_cast<size_t>(targetTriangles) ||
            positions.empty() || sourceTriangles == 0) return;
        /* A malformed mesh should remain untouched rather than indexing outside the source pool. */
        for (uint32_t index : indices) if (index >= positions.size()) return;

        Vec3 mn, mx;
        computeBounds(&mn, &mx);
        const Vec3 ext = mx - mn;
        const int cells = std::max(2, std::min(1024,
            static_cast<int>(std::sqrt(static_cast<double>(targetTriangles) * 0.5))));
        const Vec3 invCell(std::fabs(ext.x) < kEps ? 0.0f : cells / ext.x,
                           std::fabs(ext.y) < kEps ? 0.0f : cells / ext.y,
                           std::fabs(ext.z) < kEps ? 0.0f : cells / ext.z);
        std::unordered_map<uint64_t, uint32_t> table;
        table.reserve(positions.size());
        std::vector<uint32_t> remap(positions.size());
        std::vector<Vec3> acc;
        std::vector<int32_t> count;
        acc.reserve(positions.size());
        count.reserve(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) {
            const Vec3 rel = positions[i] - mn;
            const int gx = maxi(0, mini(cells, static_cast<int>(rel.x * invCell.x)));
            const int gy = maxi(0, mini(cells, static_cast<int>(rel.y * invCell.y)));
            const int gz = maxi(0, mini(cells, static_cast<int>(rel.z * invCell.z)));
            const int32_t region = i < regions.size() ? regions[i] : REGION_SKIN;
            /* `cells` is capped at 1024, so each coordinate fits in 11 bits. */
            const uint64_t key = ((static_cast<uint64_t>(static_cast<uint32_t>(region) & 0xFFu)) << 33) |
                                 (static_cast<uint64_t>(gx) << 22) |
                                 (static_cast<uint64_t>(gy) << 11) |
                                  static_cast<uint64_t>(gz);
            auto it = table.find(key);
            if (it == table.end()) {
                const uint32_t ni = static_cast<uint32_t>(acc.size());
                table.emplace(key, ni);
                acc.push_back(positions[i]);
                count.push_back(1);
                remap[i] = ni;
            } else {
                acc[it->second] += positions[i];
                count[it->second]++;
                remap[i] = it->second;
            }
        }
        for (size_t i = 0; i < acc.size(); ++i) acc[i] /= static_cast<float>(maxi(1, count[i]));

        std::vector<uint32_t> ni;
        ni.reserve(indices.size());
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t a = remap[indices[t]], b = remap[indices[t + 1]], c = remap[indices[t + 2]];
            if (a == b || b == c || a == c) continue;
            ni.push_back(a); ni.push_back(b); ni.push_back(c);
        }
        /* If this resolution collapsed the whole surface, leave the original usable mesh intact. */
        if (ni.empty()) return;

        /* Carry per-vertex attributes from the first source vertex in each region-local cluster. */
        std::vector<int32_t> newRegion(acc.size(), REGION_SKIN);
        std::vector<float> newU(acc.size() * 2, 0.0f);
        std::vector<float> newCurv(acc.size(), 1.0f);
        std::vector<uint8_t> copied(acc.size(), 0);
        for (size_t i = 0; i < positions.size(); ++i) {
            const uint32_t dst = remap[i];
            if (copied[dst]) continue;
            copied[dst] = 1;
            if (i < regions.size()) newRegion[dst] = regions[i];
            if (i < curvature.size()) newCurv[dst] = curvature[i];
            if (i * 2 + 1 < uvs.size()) {
                newU[dst * 2] = uvs[i * 2];
                newU[dst * 2 + 1] = uvs[i * 2 + 1];
            }
        }

        positions.swap(acc);
        indices.swap(ni);
        regions.swap(newRegion);
        uvs.swap(newU);
        curvature.swap(newCurv);
        normals.clear();
        tangents.clear();
        computeNormals();
        computeTangents();
    }

    /** Estimates the enclosed volume via the divergence theorem (m^3). Used by tests. */
    float volume() const {
        float v = 0.0f;
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const Vec3 a = positions[indices[t]], b = positions[indices[t + 1]], c = positions[indices[t + 2]];
            v += a.dot(b.cross(c)) / 6.0f;
        }
        return std::fabs(v);
    }

    /**
     * Per-vertex normals from face areas only (no smoothing, no weighting by anything else).
     *
     * Needed wherever the analytic field gradient cannot describe the surface. The eye, teeth and
     * tongue shells sit inside cavities that the body SDF carves out, so the gradient at a tooth
     * vertex describes the surrounding skin and points sideways: shading a tooth with it lights the
     * inside of the mouth as if it were the outside of the head. Face normals are always local and
     * always correct; the analytic gradient is smoother, so the two are combined per vertex rather
     * than replacing one with the other.
     */
    std::vector<Vec3> areaWeightedNormals() const {
        std::vector<Vec3> acc(positions.size(), Vec3(0, 0, 0));
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
            /* Un-normalised cross product: its length is twice the face area, which is exactly the
               weight wanted here. */
            const Vec3 fn = (positions[b] - positions[a]).cross(positions[c] - positions[a]);
            acc[a] += fn; acc[b] += fn; acc[c] += fn;
        }
        for (Vec3& n : acc) n = n.normalizedOr(Vec3(0, 1, 0));
        return acc;
    }

    /**
     * Makes triangle winding globally consistent, then points every connected shell outward.
     *
     * Marching tets decides orientation per tetrahedron from a local gradient sample, and a wrong
     * decision on a single tet leaves that triangle wound the other way. The damage is not
     * cosmetic: the audit sees both of its directed edges duplicated (non-manifold), the UV
     * charter refuses to merge it with any neighbour (its normal opposes all of theirs), and a
     * renderer with backface culling drops it. Measured before this pass: 2497 of 142400
     * triangles (1.75%) wound backwards, 823 non-manifold edges, 4857 one-face texture charts.
     *
     * Step 1 is the standard dual-graph BFS: two triangles sharing an edge agree when they
     * traverse that edge in OPPOSITE directions; if they traverse it the same way, one of them is
     * flipped. Edges with more than two incident triangles are skipped because the pairing is
     * ambiguous there (they are counted by audit() as non-manifold and reported to the caller).
     *
     * Step 2 fixes the global sign: consistency alone cannot tell inside from outside, so each
     * connected shell is flipped as a whole when its face normals oppose the per-vertex normals
     * (which come from the analytic field gradient and therefore always point outward).
     *
     * Returns the number of flipped triangles. Costs one pass over the indices.
     */
    int orientConsistently() {
        if (indices.size() < 3) return 0;
        const size_t faceCount = triangleCount();
        /* UNDIRECTED adjacency: each edge maps to the faces that use it. Directions are tested
           against the live index buffer, so flipping a face mid-traversal cannot invalidate the
           structure (a directed map would go stale the moment the first flip happened). */
        std::unordered_map<uint64_t, std::vector<uint32_t>> edgeFaces;
        edgeFaces.reserve(indices.size());
        auto ukey = [](uint32_t a, uint32_t b) {
            if (a > b) std::swap(a, b);
            return (static_cast<uint64_t>(a) << 32) | b;
        };
        for (size_t f = 0; f < faceCount; ++f) {
            const uint32_t* v = &indices[f * 3];
            for (int e = 0; e < 3; ++e) edgeFaces[ukey(v[e], v[(e + 1) % 3])].push_back(static_cast<uint32_t>(f));
        }
        auto traverses = [&](uint32_t f, uint32_t a, uint32_t b) {
            const uint32_t* v = &indices[static_cast<size_t>(f) * 3];
            for (int k = 0; k < 3; ++k) if (v[k] == a && v[(k + 1) % 3] == b) return true;
            return false;
        };
        auto flipFace = [&](uint32_t f) {
            std::swap(indices[static_cast<size_t>(f) * 3 + 1], indices[static_cast<size_t>(f) * 3 + 2]);
        };

        int flipped = 0;
        std::vector<uint8_t> seen(faceCount, 0);
        std::vector<uint32_t> stack, component;
        for (size_t seed = 0; seed < faceCount; ++seed) {
            if (seen[seed]) continue;
            component.clear();
            seen[seed] = 1;
            stack.assign(1, static_cast<uint32_t>(seed));
            while (!stack.empty()) {
                const uint32_t f = stack.back();
                stack.pop_back();
                component.push_back(f);
                const uint32_t* v = &indices[static_cast<size_t>(f) * 3];
                for (int e = 0; e < 3; ++e) {
                    const uint32_t a = v[e], b = v[(e + 1) % 3];
                    const auto it = edgeFaces.find(ukey(a, b));
                    if (it == edgeFaces.end() || it->second.size() != 2) continue;  /* >2 = non-manifold, ambiguous */
                    const uint32_t nb = (it->second[0] == f) ? it->second[1] : it->second[0];
                    if (seen[nb]) continue;
                    /* Two triangles agree on a shared edge when they traverse it in opposite
                       directions; the same direction means the neighbour is inside out. */
                    if (traverses(nb, a, b)) { flipFace(nb); ++flipped; }
                    seen[nb] = 1;
                    stack.push_back(nb);
                }
            }
            /*
             * Consistency cannot distinguish inside from outside. For a CLOSED shell the sign of
             * the enclosed volume decides it exactly and cannot be fooled by a few noisy vertex
             * normals: an outward-wound shell has positive signed volume. Only when the shell is
             * open or numerically flat (|V| under half a cubic millimetre, which is what a
             * degenerate island looks like) fall back to comparing face normals with the vertex
             * normals, which is the weaker test that an earlier revision used alone.
             */
            double volume = 0.0, agree = 0.0;
            for (uint32_t f : component) {
                const uint32_t* v = &indices[static_cast<size_t>(f) * 3];
                volume += static_cast<double>(positions[v[0]].dot(positions[v[1]].cross(positions[v[2]]))) / 6.0;
                if (!normals.empty() && normals.size() == positions.size()) {
                    const Vec3 gn = (positions[v[1]] - positions[v[0]]).cross(positions[v[2]] - positions[v[0]]);
                    if (gn.lengthSq() < 1e-20f) continue;
                    agree += static_cast<double>(gn.normalized().dot(
                        (normals[v[0]] + normals[v[1]] + normals[v[2]]).normalizedOr(Vec3(0, 1, 0))));
                }
            }
            const bool inverted = (std::fabs(volume) > 5e-10) ? (volume < 0.0) : (agree < 0.0);
            if (inverted) {
                for (uint32_t f : component) { flipFace(f); ++flipped; }
            }
        }
        return flipped;
    }

    /** Manifold audit: every directed edge must appear exactly once and have a twin. */
    struct ManifoldReport {
        size_t openEdges = 0;      /* edges with no opposite twin => holes                   */
        size_t nonManifoldEdges = 0;/* edges sharing >2 triangles                            */
        size_t boundaryLoops = 0;
        bool watertight() const { return openEdges == 0 && nonManifoldEdges == 0; }
    };

    ManifoldReport audit() const {
        ManifoldReport r;
        std::unordered_map<uint64_t, int> dir;
        dir.reserve(indices.size());
        auto key = [](uint32_t a, uint32_t b) { return (static_cast<uint64_t>(a) << 32) | b; };
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t i[3] = {indices[t], indices[t + 1], indices[t + 2]};
            for (int e = 0; e < 3; ++e) dir[key(i[e], i[(e + 1) % 3])]++;
        }
        for (auto& kv : dir) {
            const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
            const uint32_t b = static_cast<uint32_t>(kv.first & 0xFFFFFFFFu);
            if (kv.second > 1) { r.nonManifoldEdges++; continue; }
            if (dir.find(key(b, a)) == dir.end()) r.openEdges++;
        }
        r.boundaryLoops = r.openEdges;
        return r;
    }

private:
};

/* ------------------------------------------------------------------ BVH ------------------ */
/** Binned-construction BVH over mesh triangles; used for picking and cloth collision. */
class Bvh {
public:
    struct Hit { bool hit = false; float t = 0.0f; uint32_t triangle = 0; Vec3 point, normal; };

    void build(const Mesh& mesh, int leafSize = 8) {
        mesh_ = &mesh;
        nodes_.clear();
        triOrder_.resize(mesh.triangleCount());
        for (size_t i = 0; i < triOrder_.size(); ++i) triOrder_[i] = static_cast<uint32_t>(i);
        if (triOrder_.empty()) return;
        nodes_.reserve(mesh.triangleCount() * 2);
        buildNode(0, static_cast<uint32_t>(triOrder_.size()), leafSize);
        refitAll();
    }

    bool empty() const { return nodes_.empty(); }

    Hit raycast(const Vec3& origin, const Vec3& dir, float tMax = 1e9f) const {
        Hit best;
        best.t = tMax;
        if (nodes_.empty()) return best;
        const Vec3 invDir(1.0f / (std::fabs(dir.x) < 1e-9f ? (dir.x < 0 ? -1e-9f : 1e-9f) : dir.x),
                          1.0f / (std::fabs(dir.y) < 1e-9f ? (dir.y < 0 ? -1e-9f : 1e-9f) : dir.y),
                          1.0f / (std::fabs(dir.z) < 1e-9f ? (dir.z < 0 ? -1e-9f : 1e-9f) : dir.z));
        uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& n = nodes_[stack[--sp]];
            float t0, t1;
            if (!slabTest(n.bmin, n.bmax, origin, invDir, best.t, &t0, &t1)) continue;
            if (n.count > 0) {
                for (uint32_t i = 0; i < n.count; ++i) {
                    const uint32_t tri = triOrder_[n.first + i];
                    float t;
                    Vec3 p, nrm;
                    if (intersectTriangle(tri, origin, dir, &t, &p, &nrm) && t < best.t) {
                        best.hit = true; best.t = t; best.triangle = tri; best.point = p; best.normal = nrm;
                        best.t = t;
                    }
                }
            } else {
                if (sp + 2 <= 64) {
                    stack[sp++] = n.right;
                    stack[sp++] = n.left;
                }
            }
        }
        return best;
    }

    /** Signed clearance: min distance from `p` to the surface, clamped to `searchRadius`. */
    float signedDistance(const Vec3& p, float searchRadius = 0.4f) const {
        if (nodes_.empty()) return searchRadius;
        float best = searchRadius;
        for (const Node& n : nodes_) {
            if (n.count == 0) continue;
            if (aabbDistanceSq(n.bmin, n.bmax, p) > best * best) continue;
            for (uint32_t i = 0; i < n.count; ++i) {
                const uint32_t tri = triOrder_[n.first + i];
                const Vec3 a = mesh_->positions[mesh_->indices[tri * 3]];
                const Vec3 b = mesh_->positions[mesh_->indices[tri * 3 + 1]];
                const Vec3 c = mesh_->positions[mesh_->indices[tri * 3 + 2]];
                const Vec3 q = closestPointOnTriangle(p, a, b, c);
                const float d = (q - p).length();
                if (d < best) best = d;
            }
        }
        return best;
    }

    static Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 ab = b - a, ac = c - a, ap = p - a;
        const float d1 = ab.dot(ap), d2 = ac.dot(ap);
        if (d1 <= 0.0f && d2 <= 0.0f) return a;
        const Vec3 bp = p - b;
        const float d3 = ab.dot(bp), d4 = ac.dot(bp);
        if (d3 >= 0.0f && d4 <= d3) return b;
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
        const Vec3 cp = p - c;
        const float d5 = ab.dot(cp), d6 = ac.dot(cp);
        if (d6 >= 0.0f && d5 <= d6) return c;
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
            return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        }
        const float denom = 1.0f / (va + vb + vc);
        return a + ab * (vb * denom) + ac * (vc * denom);
    }

private:
    struct Node {
        Vec3 bmin, bmax;
        uint32_t first = 0, count = 0, left = 0, right = 0;
    };

    const Mesh* mesh_ = nullptr;
    std::vector<Node> nodes_;
    std::vector<uint32_t> triOrder_;

    void triBounds(uint32_t tri, Vec3* mn, Vec3* mx) const {
        const Vec3 a = mesh_->positions[mesh_->indices[tri * 3]];
        const Vec3 b = mesh_->positions[mesh_->indices[tri * 3 + 1]];
        const Vec3 c = mesh_->positions[mesh_->indices[tri * 3 + 2]];
        *mn = a.minv(b).minv(c);
        *mx = a.maxv(b).maxv(c);
    }

    uint32_t buildNode(uint32_t first, uint32_t count, int leafSize) {
        Node n;
        Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
        for (uint32_t i = 0; i < count; ++i) {
            Vec3 a, b;
            triBounds(triOrder_[first + i], &a, &b);
            mn = mn.minv(a); mx = mx.maxv(b);
        }
        n.bmin = mn; n.bmax = mx; n.first = first; n.count = (count <= static_cast<uint32_t>(leafSize)) ? count : 0;
        const uint32_t myIndex = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back(n);
        if (n.count > 0) return myIndex;

        const Vec3 ext = mx - mn;
        const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
        const uint32_t mid = first + count / 2;
        std::nth_element(triOrder_.begin() + first, triOrder_.begin() + mid, triOrder_.begin() + first + count,
                         [&](uint32_t ta, uint32_t tb) {
                             Vec3 amn, amx, bmn, bmx;
                             triBounds(ta, &amn, &amx);
                             triBounds(tb, &bmn, &bmx);
                             const float ca = 0.5f * (amn[axis] + amx[axis]);
                             const float cb = 0.5f * (bmn[axis] + bmx[axis]);
                             return ca < cb;
                         });
        const uint32_t leftIdx = buildNode(first, mid - first, leafSize);
        const uint32_t rightIdx = buildNode(mid, first + count - mid, leafSize);
        nodes_[myIndex].left = leftIdx;
        nodes_[myIndex].right = rightIdx;
        return myIndex;
    }

    void refitAll() {
        for (size_t i = nodes_.size(); i-- > 0;) {
            Node& n = nodes_[i];
            if (n.count > 0) continue;
            const Node& l = nodes_[n.left];
            const Node& r = nodes_[n.right];
            n.bmin = l.bmin.minv(r.bmin);
            n.bmax = l.bmax.maxv(r.bmax);
        }
    }

    static float aabbDistanceSq(const Vec3& mn, const Vec3& mx, const Vec3& p) {
        const float dx = maxf(maxf(mn.x - p.x, 0.0f), p.x - mx.x);
        const float dy = maxf(maxf(mn.y - p.y, 0.0f), p.y - mx.y);
        const float dz = maxf(maxf(mn.z - p.z, 0.0f), p.z - mx.z);
        return dx * dx + dy * dy + dz * dz;
    }

    static bool slabTest(const Vec3& mn, const Vec3& mx, const Vec3& o, const Vec3& invDir,
                         float tMax, float* t0Out, float* t1Out) {
        float t0 = (mn.x - o.x) * invDir.x, t1 = (mx.x - o.x) * invDir.x;
        if (t0 > t1) { const float t = t0; t0 = t1; t1 = t; }
        const float t2 = (mn.y - o.y) * invDir.y, t3 = (mx.y - o.y) * invDir.y;
        t0 = maxf(t0, minf(t2, t3)); t1 = minf(t1, maxf(t2, t3));
        const float t4 = (mn.z - o.z) * invDir.z, t5 = (mx.z - o.z) * invDir.z;
        t0 = maxf(t0, minf(t4, t5)); t1 = minf(t1, maxf(t4, t5));
        *t0Out = t0; *t1Out = t1;
        return t1 >= maxf(t0, 0.0f) && t0 <= tMax;
    }

    bool intersectTriangle(uint32_t tri, const Vec3& o, const Vec3& d, float* tOut, Vec3* pOut, Vec3* nOut) const {
        const Vec3 a = mesh_->positions[mesh_->indices[tri * 3]];
        const Vec3 b = mesh_->positions[mesh_->indices[tri * 3 + 1]];
        const Vec3 c = mesh_->positions[mesh_->indices[tri * 3 + 2]];
        const Vec3 e1 = b - a, e2 = c - a;
        const Vec3 pv = d.cross(e2);
        const float det = e1.dot(pv);
        if (std::fabs(det) < 1e-12f) return false;
        const float invDet = 1.0f / det;
        const Vec3 tv = o - a;
        const float u = tv.dot(pv) * invDet;
        if (u < -1e-6f || u > 1.0f + 1e-6f) return false;
        const Vec3 qv = tv.cross(e1);
        const float v = d.dot(qv) * invDet;
        if (v < -1e-6f || u + v > 1.0f + 1e-6f) return false;
        const float t = e2.dot(qv) * invDet;
        if (t <= 1e-6f) return false;
        *tOut = t;
        *pOut = o + d * t;
        *nOut = e1.cross(e2).normalizedOr({0, 1, 0});
        return true;
    }
};

} // namespace af

#endif /* AF_MESH_H */
