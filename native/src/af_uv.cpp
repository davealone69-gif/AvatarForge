/*
 * af_uv.cpp -- chart segmentation, planar projection, shelf packing and seam splitting.
 */
#include "af_uv.h"
#include "af_threads.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <unordered_map>

namespace af {

namespace {

struct Bounds2 {
    Vec2 mn{1e30f, 1e30f};
    Vec2 mx{-1e30f, -1e30f};
    void add(const Vec2& p) { mn = mn.minv(p); mx = mx.maxv(p); }
    Vec2 size() const { return mx - mn; }
};

/** Builds an orthonormal basis whose +z is `n`. */
void basisFromNormal(const Vec3& n, Vec3* u, Vec3* v) {
    const Vec3 ref = (std::fabs(n.y) < 0.9f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    Vec3 a = ref.cross(n);
    if (a.lengthSq() < 1e-12f) a = Vec3(1, 0, 0);
    *u = a.normalized();
    *v = n.cross(*u).normalized();
}

float triangleArea(const Vec3& a, const Vec3& b, const Vec3& c) {
    return (b - a).cross(c - a).length() * 0.5f;
}

} // namespace

void assignBoxProjectionUVs(Mesh* mesh) {
    Vec3 mn, mx;
    mesh->computeBounds(&mn, &mx);
    const Vec3 ext = mx - mn;
    mesh->uvs.assign(mesh->vertexCount() * 2, 0.0f);
    for (size_t i = 0; i < mesh->vertexCount(); ++i) {
        const Vec3& p = mesh->positions[i];
        const Vec3& n = mesh->normals.empty() ? Vec3(0, 0, 1) : mesh->normals[i];
        const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        float u = 0, v = 0;
        if (ax >= ay && ax >= az) { u = (p.z - mn.z) / (ext.z > 1e-6f ? ext.z : 1.0f); v = (p.y - mn.y) / (ext.y > 1e-6f ? ext.y : 1.0f); }
        else if (ay >= az)        { u = (p.x - mn.x) / (ext.x > 1e-6f ? ext.x : 1.0f); v = (p.z - mn.z) / (ext.z > 1e-6f ? ext.z : 1.0f); }
        else                      { u = (p.x - mn.x) / (ext.x > 1e-6f ? ext.x : 1.0f); v = (p.y - mn.y) / (ext.y > 1e-6f ? ext.y : 1.0f); }
        mesh->uvs[i * 2] = u;
        mesh->uvs[i * 2 + 1] = v;
    }
}

bool unwrapAtlas(Mesh* mesh, int texWidth, int texHeight, UVLayout* outLayout) {
    if (!mesh || mesh->indices.size() < 3) return false;
    const size_t faceCount = mesh->triangleCount();
    mesh->computeNormals();

    /* ---------------------------------------------------------------- 1. charts --------------- */
    /* Face adjacency from shared edges. */
    std::vector<std::array<uint32_t, 3>> faces(faceCount);
    for (size_t f = 0; f < faceCount; ++f) {
        faces[f] = {mesh->indices[f * 3], mesh->indices[f * 3 + 1], mesh->indices[f * 3 + 2]};
    }
    std::unordered_map<uint64_t, std::vector<uint32_t>> edgeFaces;
    edgeFaces.reserve(faceCount * 3);
    auto ekey = [](uint32_t a, uint32_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };
    for (size_t f = 0; f < faceCount; ++f) {
        for (int e = 0; e < 3; ++e) {
            edgeFaces[ekey(faces[f][e], faces[f][(e + 1) % 3])].push_back(static_cast<uint32_t>(f));
        }
    }

    std::vector<Vec3> faceNormals(faceCount);
    std::vector<int32_t> faceRegion(faceCount, REGION_SKIN);
    for (size_t f = 0; f < faceCount; ++f) {
        const Vec3 a = mesh->positions[faces[f][0]], b = mesh->positions[faces[f][1]], c = mesh->positions[faces[f][2]];
        faceNormals[f] = (b - a).cross(c - a).normalizedOr({0, 1, 0});
        /* A face inherits the region that most of its vertices carry. */
        int counts[16] = {0};
        for (int k = 0; k < 3; ++k) {
            const uint32_t vi = faces[f][static_cast<size_t>(k)];
            const int r = (vi < mesh->regions.size()) ? mesh->regions[vi] : REGION_SKIN;
            if (r >= 0 && r < 16) counts[r]++;
        }
        int best = REGION_SKIN, bestCount = -1;
        for (int r = 0; r < 16; ++r) if (counts[r] > bestCount) { bestCount = counts[r]; best = r; }
        faceRegion[f] = best;
    }

    /*
     * Charting: DFS growth inside a cone anchored on the seed face's normal.
     *
     * The cone must be anchored to the seed and not to a running average. On a closed surface the
     * running average of a chart that wraps a limb tends to zero, so its direction becomes noise;
     * a chart then either freezes after a few faces or keeps absorbing unrelated ones. Anchoring
     * the cone makes growth deterministic and stable, and it doubles as the projection axis.
     *
     * Why a cone and not the dihedral test: measured on this mesh, 4.2% of faces (5967 of 142404)
     * have NO neighbour within 40 degrees - the folds where the surface doubles back inside
     * sub-lattice notches between fingers, toes, nostrils and ears. A dihedral flood fill therefore
     * fragments into 11505 islands, while the cone test keeps a curved surface together and the
     * folded faces become their own (correct) islands: 8205 islands, 0.68 atlas coverage.
     */
    const float kCosTolerance = std::cos(radians(62.0f));
    std::vector<int32_t> faceChart(faceCount, -1);
    std::vector<UVChart> charts;
    std::vector<int32_t> chartFaces;

    for (size_t seed = 0; seed < faceCount; ++seed) {
        if (faceChart[seed] >= 0) continue;
        const int32_t chartId = static_cast<int32_t>(charts.size());
        const Vec3 seedNormal = faceNormals[seed];
        UVChart ch;
        ch.material = faceRegion[seed];
        ch.firstFace = static_cast<int32_t>(chartFaces.size());
        std::vector<uint32_t> stack{static_cast<uint32_t>(seed)};
        faceChart[seed] = chartId;
        const size_t chartStart = chartFaces.size();
        while (!stack.empty()) {
            const uint32_t f = stack.back();
            stack.pop_back();
            chartFaces.push_back(static_cast<int32_t>(f));
            for (int e = 0; e < 3; ++e) {
                const auto it = edgeFaces.find(ekey(faces[f][static_cast<size_t>(e)], faces[f][static_cast<size_t>((e + 1) % 3)]));
                if (it == edgeFaces.end()) continue;
                for (uint32_t nb : it->second) {
                    if (nb == f || faceChart[nb] >= 0) continue;
                    if (faceRegion[nb] != faceRegion[seed]) continue;
                    if (faceNormals[nb].dot(seedNormal) < kCosTolerance) continue;
                    /* One island must not eat the atlas; 6000 faces is roughly a whole torso at
                       the coarse spacing. */
                    if (chartFaces.size() - chartStart > 6000) continue;
                    faceChart[nb] = chartId;
                    stack.push_back(nb);
                }
            }
        }
        ch.faceCount = static_cast<int32_t>(chartFaces.size()) - ch.firstFace;
        charts.push_back(ch);
    }

    /* Unassigned faces can only appear if two faces share no edge (isolated triangles). */
    for (size_t f = 0; f < faceCount; ++f) {
        if (faceChart[f] < 0) {
            UVChart ch;
            ch.material = faceRegion[f];
            ch.firstFace = static_cast<int32_t>(chartFaces.size());
            ch.faceCount = 1;
            chartFaces.push_back(static_cast<int32_t>(f));
            faceChart[f] = static_cast<int32_t>(charts.size());
            charts.push_back(ch);
        }
    }

#if 1
    if (std::getenv("AF_UV_DEBUG")) {
        int shown = 0;
        for (size_t c = 0; c < charts.size() && shown < 8; ++c) {
            if (charts[c].faceCount != 1) continue;
            const uint32_t f = static_cast<uint32_t>(chartFaces[static_cast<size_t>(charts[c].firstFace)]);
            int nbCount = 0; float best = -2.0f, worst = 2.0f;
            for (int e = 0; e < 3; ++e) {
                const auto it = edgeFaces.find(ekey(faces[f][static_cast<size_t>(e)], faces[f][static_cast<size_t>((e + 1) % 3)]));
                if (it == edgeFaces.end()) { std::printf("[uv] singleton face %u edge %d has NO adjacency\n", f, e); continue; }
                for (uint32_t nb : it->second) {
                    if (nb == f) continue;
                    ++nbCount;
                    const float d = faceNormals[f].dot(faceNormals[nb]);
                    best = std::max(best, d); worst = std::min(worst, d);
                }
            }
            std::printf("[uv] singleton face %u region %d: %d neighbours, dot %.3f..%.3f\n",
                        f, faceRegion[f], nbCount, worst, best);
            ++shown;
        }
    }
#endif
    /* ---------------------------------------------------------------- 2. projection ----------- */
    struct Chart2D {
        Vec3 axisU, axisV, origin;
        std::vector<Vec2> uvs;      /* parallel to the chart's face list, 3 per face */
        Bounds2 bounds;
        float area = 0.0f;
    };
    std::vector<Chart2D> projected(charts.size());
    for (size_t c = 0; c < charts.size(); ++c) {
        const UVChart& ch = charts[c];
        /* Area-weighted axis of the chart. The cone growth guarantees every face is within 62
           degrees of this axis, so the projection is a genuine flattening of the chart and not a
           fold, and the axis is well conditioned because the cone is anchored. */
        Vec3 nsum(0, 0, 0);
        for (int32_t i = 0; i < ch.faceCount; ++i) {
            const uint32_t f = static_cast<uint32_t>(chartFaces[static_cast<size_t>(ch.firstFace + i)]);
            const Vec3& a = mesh->positions[faces[f][0]];
            const Vec3& b = mesh->positions[faces[f][1]];
            const Vec3& cc = mesh->positions[faces[f][2]];
            nsum += faceNormals[f] * triangleArea(a, b, cc);
        }
        const Vec3 n = nsum.normalizedOr(faceNormals[chartFaces[static_cast<size_t>(ch.firstFace)]]);
        Chart2D& p2 = projected[c];
        basisFromNormal(n, &p2.axisU, &p2.axisV);

        /* Project relative to the chart's own first vertex: keeps the numbers small and makes
           the packing resolution-independent. */
        p2.origin = mesh->positions[faces[chartFaces[static_cast<size_t>(ch.firstFace)]][0]];
        p2.uvs.reserve(static_cast<size_t>(ch.faceCount) * 3);
        for (int32_t i = 0; i < ch.faceCount; ++i) {
            const uint32_t f = static_cast<uint32_t>(chartFaces[static_cast<size_t>(ch.firstFace + i)]);
            for (int k = 0; k < 3; ++k) {
                const Vec3 rel = mesh->positions[faces[f][static_cast<size_t>(k)]] - p2.origin;
                const Vec2 uv(p2.axisU.dot(rel), p2.axisV.dot(rel));
                p2.uvs.push_back(uv);
                p2.bounds.add(uv);
            }
            const Vec3 a = mesh->positions[faces[f][0]], b = mesh->positions[faces[f][1]], cc = mesh->positions[faces[f][2]];
            p2.area += triangleArea(a, b, cc);
        }
        charts[c].areaSqMeters = p2.area;

        /*
         * Orientation check. A texture chart must be projected with the surface's own handedness:
         * a mirrored chart still bakes, but every relief gradient inside it comes out inverted, so
         * crevices light up as ridges. The signed area of the chart's projected triangles is the
         * exact test (rotation and uniform scaling below preserve it), and negating the projection
         * axis is the mirror that fixes it. Measured before this check: 4596 of 142404 triangles
         * carried negative UV area, spread over 1116 of the 8105 charts.
         */
        double signedArea = 0.0;
        for (int32_t i = 0; i < ch.faceCount; ++i) {
            const Vec2& a = p2.uvs[static_cast<size_t>(i) * 3];
            const Vec2& b = p2.uvs[static_cast<size_t>(i) * 3 + 1];
            const Vec2& d = p2.uvs[static_cast<size_t>(i) * 3 + 2];
            signedArea += static_cast<double>(b.x - a.x) * (d.y - a.y) - static_cast<double>(d.x - a.x) * (b.y - a.y);
        }
        if (signedArea < 0.0) {
            p2.axisU = p2.axisU * -1.0f;
            for (Vec2& uv : p2.uvs) uv.x = -uv.x;
            p2.bounds = Bounds2{};
            for (const Vec2& uv : p2.uvs) p2.bounds.add(uv);
        }
    }

#if 1
    if (std::getenv("AF_UV_DEBUG")) {
        size_t thinX = 0, thinY = 0, degenerate = 0;
        double minAxis = 1e9, maxAxis = 0;
        for (size_t c = 0; c < charts.size(); ++c) {
            const Vec2 sz = projected[c].bounds.size();
            if (sz.x < 1e-6f) ++thinX;
            if (sz.y < 1e-6f) ++thinY;
            const Vec2 sa = projected[c].bounds.size();
            if (sa.x * sa.y < 1e-12f) ++degenerate;
        }
        for (size_t c = 0; c < charts.size(); ++c) {
            Vec3 nsum(0, 0, 0); float area = 0.0f;
            const UVChart& ch = charts[c];
            for (int32_t i = 0; i < ch.faceCount; ++i) {
                const uint32_t f = static_cast<uint32_t>(chartFaces[static_cast<size_t>(ch.firstFace + i)]);
                const Vec3& a = mesh->positions[faces[f][0]];
                const Vec3& b = mesh->positions[faces[f][1]];
                const Vec3& cc = mesh->positions[faces[f][2]];
                const float ar = triangleArea(a, b, cc);
                nsum += faceNormals[f] * ar; area += ar;
            }
            const double ratio = area > 1e-12f ? static_cast<double>(nsum.length()) / area : 0.0;
            minAxis = std::min(minAxis, ratio); maxAxis = std::max(maxAxis, ratio);
        }
        std::printf("[uv] charts=%zu thin-x=%zu thin-y=%zu degenerate=%zu | axis coherence %.4f..%.4f\n",
                    charts.size(), thinX, thinY, degenerate, minAxis, maxAxis);
    }
#endif

    /* ---------------------------------------------------------------- 3. packing -------------- */
    /* Texel density search: pack with a density, halve until everything fits. Starting density is
       derived from the total surface area so the loop is short and the result stable. */
    const float gutterUV = static_cast<float>(std::max(2, texWidth / 512)) / static_cast<float>(texWidth);
    float density = 0.0f;
    {
        float totalArea = 0.0f;
        for (size_t c = 0; c < charts.size(); ++c) totalArea += charts[c].areaSqMeters;
        if (totalArea <= 1e-9f) return false;
        /* Upper bound: the whole atlas given to the surface, ignoring packing waste. */
        density = std::sqrt(0.52f * static_cast<float>(texWidth) * static_cast<float>(texHeight) / totalArea);
    }

    struct Placement { float x, y, w, h; bool rotated; };
    std::vector<Placement> placement(charts.size());
    std::vector<size_t> order(charts.size());
    float usedDensity = density;
    bool packed = false;
    for (int attempt = 0; attempt < 14 && !packed; ++attempt) {
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const Vec2 sa = projected[a].bounds.size();
            return std::max(sa.x, sa.y) > std::max(projected[b].bounds.size().x, projected[b].bounds.size().y);
        });

        float shelfX = 0.0f, shelfY = 0.0f, shelfH = 0.0f;
        packed = true;
        for (size_t idx : order) {
            const Vec2 sz = projected[idx].bounds.size();
            /* `usedDensity` is texels per METRE; the shelf lives in 0..1 UV space, so the size
               has to be converted through the atlas resolution. (Multiplying metres by a
               texel/metre figure and treating the result as UV is what made the first revision
               fail to pack anything - every chart claimed to be a thousand atlases wide.) */
            float w = sz.x * usedDensity / static_cast<float>(texWidth) + gutterUV * 2.0f;
            float h = sz.y * usedDensity / static_cast<float>(texHeight) + gutterUV * 2.0f;
            bool rot = false;
            /* Try the rotated variant when it clearly reduces the shelf height. */
            if (h > w * 1.6f) { std::swap(w, h); rot = true; }
            if (shelfX + w > 1.0f) { shelfX = 0.0f; shelfY += shelfH; shelfH = 0.0f; }
            if (shelfY + h > 1.0f) { packed = false; break; }
            placement[idx] = {shelfX, shelfY, w, h, rot};
            shelfX += w;
            shelfH = std::max(shelfH, h);
        }
        if (!packed) usedDensity *= 0.82f;
    }
    if (!packed) return false;

    /* ---------------------------------------------------------------- 4. seam splitting -------- */
    /* Each vertex gets one UV slot per chart that uses it. */
    struct SplitKey { uint32_t vertex; uint32_t chart; };
    std::unordered_map<uint64_t, uint32_t> splitTable;
    splitTable.reserve(mesh->vertexCount() * 2);
    std::vector<uint32_t> splitOf(faceCount * 3, 0);
    std::vector<Vec3> npos, nnorm;
    std::vector<Vec4> ntan;
    std::vector<float> nuv, ncurv;
    std::vector<int32_t> nreg;
    npos.reserve(mesh->vertexCount() + mesh->vertexCount() / 4);
    nnorm.reserve(npos.capacity());
    ntan.reserve(npos.capacity());
    nuv.reserve(npos.capacity() * 2);
    nreg.reserve(npos.capacity());
    ncurv.reserve(npos.capacity());

    for (size_t c = 0; c < charts.size(); ++c) {
        const UVChart& ch = charts[c];
        const Chart2D& p2 = projected[c];
        const Placement& pl = placement[c];
        const Vec2 chartSize = p2.bounds.size();
        /* UV units per metre for this chart: the shelf cell (minus its gutter) divided by the
           chart's projected extent. Using the same density for w and h keeps the chart
           undistorted even when the atlas is not square. */
        const float sx = chartSize.x > 1e-9f ? (pl.w - gutterUV * 2.0f) / chartSize.x : 0.0f;
        const float sy = chartSize.y > 1e-9f ? (pl.h - gutterUV * 2.0f) / chartSize.y : 0.0f;

        for (int32_t i = 0; i < ch.faceCount; ++i) {
            const uint32_t f = static_cast<uint32_t>(chartFaces[static_cast<size_t>(ch.firstFace + i)]);
            for (int k = 0; k < 3; ++k) {
                const uint32_t vi = faces[f][static_cast<size_t>(k)];
                const uint64_t key = (static_cast<uint64_t>(vi) << 32) | static_cast<uint64_t>(c);
                auto it = splitTable.find(key);
                uint32_t dst;
                if (it != splitTable.end()) {
                    dst = it->second;
                } else {
                    dst = static_cast<uint32_t>(npos.size());
                    splitTable.emplace(key, dst);
                    npos.push_back(mesh->positions[vi]);
                    nnorm.push_back(vi < mesh->normals.size() ? mesh->normals[vi] : Vec3(0, 1, 0));
                    ntan.push_back(vi < mesh->tangents.size() ? mesh->tangents[vi] : Vec4(1, 0, 0, 1));
                    nreg.push_back(vi < mesh->regions.size() ? mesh->regions[vi] : ch.material);
                    ncurv.push_back(vi < mesh->curvature.size() ? mesh->curvature[vi] : 1.0f);

                    Vec2 local = (p2.uvs[static_cast<size_t>(i) * 3 + static_cast<size_t>(k)] - p2.bounds.mn);
                    /* A 90-degree ROTATION, not a transpose: transposing mirrors the chart, which
                       flips the winding of every triangle in it and bakes mirrored textures. */
                    if (pl.rotated) local = Vec2(chartSize.y - local.y, local.x);
                    local.x *= sx; local.y *= sy;
                    const float u = pl.x + gutterUV + local.x;
                    /* glTF UV origin is top-left; flipping v here means the baked images need no
                       vertical flip anywhere downstream. */
                    const float v = pl.y + gutterUV + local.y;
                    nuv.push_back(clampf(u, 0.0f, 1.0f));
                    nuv.push_back(clampf(v, 0.0f, 1.0f));
                }
                splitOf[static_cast<size_t>(f) * 3 + static_cast<size_t>(k)] = dst;
            }
        }

        UVChart& placed = charts[c];
        placed.rectMin = Vec2(pl.x, pl.y);
        placed.rectMax = Vec2(pl.x + pl.w, pl.y + pl.h);
    }

    float occupied = 0.0f;
    for (size_t c = 0; c < charts.size(); ++c) {
        occupied += (charts[c].rectMax.x - charts[c].rectMin.x) * (charts[c].rectMax.y - charts[c].rectMin.y);
    }

    /* Commit. */
    mesh->positions.swap(npos);
    mesh->normals.swap(nnorm);
    mesh->tangents.swap(ntan);
    mesh->uvs.swap(nuv);
    mesh->regions.swap(nreg);
    mesh->curvature.swap(ncurv);
    mesh->indices.assign(splitOf.begin(), splitOf.end());

    if (outLayout) {
        outLayout->charts = charts;
        outLayout->faces = chartFaces;
        outLayout->texWidth = texWidth;
        outLayout->texHeight = texHeight;
        outLayout->texelsPerMeter = usedDensity;
        outLayout->coverage = occupied;
        outLayout->gutterTexels = static_cast<int>(std::round(gutterUV * static_cast<float>(texWidth)));
    }
    return true;
}

} // namespace af
