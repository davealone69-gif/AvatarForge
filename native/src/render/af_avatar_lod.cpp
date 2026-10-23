/*
 * af_avatar_lod.cpp -- deterministic index-only clustering for the single-avatar render path.
 */
#include "af_avatar_lod.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace af {
namespace {

/* Mobile-first LODs: the renderer selects the finest level at or below 100k triangles. */
constexpr float kLodFractions[2] = {0.18f, 0.08f};
constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

struct EdgeKey {
    uint32_t a;
    uint32_t b;
    bool operator==(const EdgeKey& other) const { return a == other.a && b == other.b; }
};

struct EdgeHash {
    size_t operator()(const EdgeKey& edge) const {
        const uint64_t key = (static_cast<uint64_t>(edge.a) << 32) | edge.b;
        return static_cast<size_t>(key ^ (key >> 33));
    }
};

struct TriangleKey {
    uint32_t v[3];
    bool operator==(const TriangleKey& other) const {
        return v[0] == other.v[0] && v[1] == other.v[1] && v[2] == other.v[2];
    }
};

struct TriangleHash {
    size_t operator()(const TriangleKey& triangle) const {
        uint64_t x = (static_cast<uint64_t>(triangle.v[0]) << 32) | triangle.v[1];
        x ^= static_cast<uint64_t>(triangle.v[2]) + 0x9e3779b97f4a7c15ull + (x << 6) + (x >> 2);
        return static_cast<size_t>(x ^ (x >> 32));
    }
};

/* The cluster identity preserves disconnected chart islands and quantised skin influence. */
struct ClusterKey {
    int32_t chart;
    int32_t x, y, z;
    uint16_t joints[4];
    uint8_t weights[4];

    bool operator==(const ClusterKey& other) const {
        if (chart != other.chart || x != other.x || y != other.y || z != other.z) return false;
        for (int i = 0; i < 4; ++i) {
            if (joints[i] != other.joints[i] || weights[i] != other.weights[i]) return false;
        }
        return true;
    }
};

struct ClusterHash {
    size_t operator()(const ClusterKey& key) const {
        uint64_t h = static_cast<uint32_t>(key.chart);
        auto mix = [&h](uint64_t value) {
            h ^= value + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(static_cast<uint32_t>(key.x));
        mix(static_cast<uint32_t>(key.y));
        mix(static_cast<uint32_t>(key.z));
        for (int i = 0; i < 4; ++i) {
            mix((static_cast<uint64_t>(key.joints[i]) << 8) | key.weights[i]);
        }
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

int32_t findRoot(std::vector<int32_t>* parent, int32_t value) {
    int32_t root = value;
    while ((*parent)[static_cast<size_t>(root)] != root) {
        root = (*parent)[static_cast<size_t>(root)];
    }
    while ((*parent)[static_cast<size_t>(value)] != value) {
        const int32_t next = (*parent)[static_cast<size_t>(value)];
        (*parent)[static_cast<size_t>(value)] = root;
        value = next;
    }
    return root;
}

void join(std::vector<int32_t>* parent, int32_t a, int32_t b) {
    a = findRoot(parent, a);
    b = findRoot(parent, b);
    if (a != b) (*parent)[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
}

bool finitePosition(const af_vertex& vertex) {
    return std::isfinite(vertex.pos[0]) && std::isfinite(vertex.pos[1]) && std::isfinite(vertex.pos[2]);
}

ClusterKey makeKey(const af_vertex& vertex, int32_t chart, const float lo[3],
                   const float invCell[3], int cells) {
    ClusterKey key{};
    key.chart = chart;
    for (int axis = 0; axis < 3; ++axis) {
        const float relative = std::max(0.0f, vertex.pos[axis] - lo[axis]);
        const float quantized = relative * invCell[axis];
        const int coordinate = std::isfinite(quantized) ? static_cast<int>(quantized) : 0;
        const int bounded = std::max(0, std::min(cells, coordinate));
        if (axis == 0) key.x = bounded;
        else if (axis == 1) key.y = bounded;
        else key.z = bounded;
    }
    for (int influence = 0; influence < 4; ++influence) {
        key.joints[influence] = vertex.joints[influence];
        const float weight = std::isfinite(vertex.weights[influence])
                                 ? std::max(0.0f, std::min(1.0f, vertex.weights[influence]))
                                 : 0.0f;
        key.weights[influence] = static_cast<uint8_t>(std::lround(weight * 16.0f));
    }
    return key;
}

/*
 * Simplifies one submesh by collapsing corners to representative source vertices. Adjacent faces
 * are labelled by shared index edges first, so UV-split chart islands do not collapse into one
 * another. Distinct joint sets and coarse weight bins also stay separate. Every output index is an
 * original source index; deformed positions, normals, UVs, weights and morph deltas remain sourced
 * from the engine's live vertex stream.
 */
std::vector<uint32_t> simplifySubmesh(const af_vertex* vertices, size_t vertexCount,
                                      const uint32_t* indices, size_t first, size_t count,
                                      float fraction) {
    std::vector<uint32_t> source(indices + first, indices + first + count);
    const size_t triangleCount = count / 3;
    if (triangleCount < 8 || fraction <= 0.0f || fraction >= 1.0f) return source;
    for (uint32_t index : source) if (index >= vertexCount) return source;

    std::vector<int32_t> parent(triangleCount);
    for (size_t i = 0; i < triangleCount; ++i) parent[i] = static_cast<int32_t>(i);
    std::unordered_map<EdgeKey, int32_t, EdgeHash> edgeFace;
    edgeFace.reserve(count);
    for (size_t face = 0; face < triangleCount; ++face) {
        const uint32_t* tri = &source[face * 3];
        for (int edge = 0; edge < 3; ++edge) {
            uint32_t a = tri[edge];
            uint32_t b = tri[(edge + 1) % 3];
            if (a > b) std::swap(a, b);
            const EdgeKey key{a, b};
            const auto inserted = edgeFace.emplace(key, static_cast<int32_t>(face));
            if (!inserted.second) join(&parent, static_cast<int32_t>(face), inserted.first->second);
        }
    }

    std::unordered_map<int32_t, int32_t> chartIds;
    std::vector<int32_t> charts(triangleCount);
    for (size_t face = 0; face < triangleCount; ++face) {
        const int32_t root = findRoot(&parent, static_cast<int32_t>(face));
        auto it = chartIds.emplace(root, static_cast<int32_t>(chartIds.size()));
        charts[face] = it.first->second;
    }

    float lo[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity()};
    float hi[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                   -std::numeric_limits<float>::infinity()};
    for (uint32_t index : source) {
        const af_vertex& vertex = vertices[index];
        if (!finitePosition(vertex)) return source;
        for (int axis = 0; axis < 3; ++axis) {
            lo[axis] = std::min(lo[axis], vertex.pos[axis]);
            hi[axis] = std::max(hi[axis], vertex.pos[axis]);
        }
    }

    const size_t target = std::max<size_t>(1, static_cast<size_t>(triangleCount * fraction));
    const int cells = std::max(2, std::min(1024,
        static_cast<int>(std::sqrt(static_cast<double>(target) * 0.5))));
    float invCell[3] = {};
    for (int axis = 0; axis < 3; ++axis) {
        const float extent = hi[axis] - lo[axis];
        invCell[axis] = extent > 1e-8f ? static_cast<float>(cells) / extent : 0.0f;
    }

    std::unordered_map<ClusterKey, uint32_t, ClusterHash> representatives;
    representatives.reserve(source.size());
    std::vector<uint32_t> simplified;
    simplified.reserve(source.size());
    std::unordered_set<TriangleKey, TriangleHash> uniqueTriangles;
    uniqueTriangles.reserve(triangleCount);

    for (size_t face = 0; face < triangleCount; ++face) {
        uint32_t mapped[3];
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t original = source[face * 3 + static_cast<size_t>(corner)];
            const ClusterKey key = makeKey(vertices[original], charts[face], lo, invCell, cells);
            auto it = representatives.emplace(key, original);
            mapped[corner] = it.first->second;
        }
        if (mapped[0] == mapped[1] || mapped[1] == mapped[2] || mapped[0] == mapped[2]) continue;
        TriangleKey key{{mapped[0], mapped[1], mapped[2]}};
        std::sort(key.v, key.v + 3);
        if (!uniqueTriangles.insert(key).second) continue;
        simplified.insert(simplified.end(), {mapped[0], mapped[1], mapped[2]});
    }

    /* Keep a usable source submesh if clustering collapses too much or makes no progress. */
    if (simplified.size() < 3 || simplified.size() >= source.size()) return source;
    return simplified;
}

} /* namespace */

void AvatarLodSet::clear() {
    indices_.clear();
    levels_.clear();
}

const AvatarLodLevel& AvatarLodSet::level(int index) const {
    static const AvatarLodLevel empty{};
    if (index < 0 || static_cast<size_t>(index) >= levels_.size()) return empty;
    return levels_[static_cast<size_t>(index)];
}

int AvatarLodSet::triangleCount(int index) const {
    return static_cast<int>(level(index).indexCount / 3);
}

bool AvatarLodSet::build(const af_vertex* restVertices, size_t vertexCount,
                         const uint32_t* sourceIndices, size_t indexCount,
                         const af_submesh* submeshes, size_t submeshCount) {
    clear();
    try {
        if ((vertexCount > 0 && !restVertices) || (indexCount > 0 && !sourceIndices) ||
            indexCount % 3 != 0 || indexCount > std::numeric_limits<uint32_t>::max()) return false;

        std::vector<af_submesh> fallback;
        if (submeshCount == 0 || !submeshes) {
            af_submesh all{};
            all.vertex_offset = 0;
            all.vertex_count = static_cast<int32_t>(std::min<size_t>(vertexCount, INT32_MAX));
            all.index_offset = 0;
            all.index_count = static_cast<int32_t>(std::min<size_t>(indexCount, INT32_MAX));
            fallback.push_back(all);
            submeshes = fallback.data();
            submeshCount = fallback.size();
        }
        if (submeshCount > static_cast<size_t>(INT32_MAX)) return false;

        std::vector<uint8_t> covered(indexCount, 0);
        for (size_t i = 0; i < submeshCount; ++i) {
            const af_submesh& submesh = submeshes[i];
            if (submesh.index_offset < 0 || submesh.index_count < 0 || submesh.index_count % 3 != 0) return false;
            const size_t first = static_cast<size_t>(submesh.index_offset);
            const size_t count = static_cast<size_t>(submesh.index_count);
            if (first > indexCount || count > indexCount - first) return false;
            for (size_t k = first; k < first + count; ++k) {
                if (covered[k]) return false;
                covered[k] = 1;
            }
        }
        if (std::find(covered.begin(), covered.end(), uint8_t{0}) != covered.end()) return false;
        for (size_t i = 0; i < indexCount; ++i) if (sourceIndices[i] >= vertexCount) return false;

        AvatarLodLevel full;
        full.firstIndex = 0;
        full.indexCount = static_cast<uint32_t>(indexCount);
        full.submeshes.resize(submeshCount);
        if (indexCount > 0) indices_.assign(sourceIndices, sourceIndices + indexCount);
        for (size_t i = 0; i < submeshCount; ++i) {
            full.submeshes[i] = AvatarLodRange{static_cast<uint32_t>(submeshes[i].index_offset),
                                               static_cast<uint32_t>(submeshes[i].index_count)};
        }
        levels_.push_back(std::move(full));
        if (indexCount == 0 || vertexCount == 0) return true;

        for (float fraction : kLodFractions) {
            AvatarLodLevel candidate;
            candidate.firstIndex = static_cast<uint32_t>(indices_.size());
            candidate.submeshes.resize(submeshCount);
            std::vector<std::vector<uint32_t>> simplifiedSubmeshes(submeshCount);
            size_t candidateCount = 0;
            for (size_t i = 0; i < submeshCount; ++i) {
                const size_t first = static_cast<size_t>(submeshes[i].index_offset);
                const size_t count = static_cast<size_t>(submeshes[i].index_count);
                simplifiedSubmeshes[i] = simplifySubmesh(restVertices, vertexCount, sourceIndices,
                                                           first, count, fraction);
                candidateCount += simplifiedSubmeshes[i].size();
            }
            if (candidateCount >= levels_.back().indexCount || candidateCount == 0 ||
                indices_.size() + candidateCount > std::numeric_limits<uint32_t>::max()) continue;

            size_t offset = indices_.size();
            for (size_t i = 0; i < submeshCount; ++i) {
                const auto& data = simplifiedSubmeshes[i];
                candidate.submeshes[i] = AvatarLodRange{static_cast<uint32_t>(offset),
                                                        static_cast<uint32_t>(data.size())};
                indices_.insert(indices_.end(), data.begin(), data.end());
                offset += data.size();
            }
            candidate.indexCount = static_cast<uint32_t>(candidateCount);
            levels_.push_back(std::move(candidate));
        }
        return true;
    } catch (...) {
        /* A renderer allocation failure must degrade to the exact source index stream, not escape
           across the C/JNI boundary and terminate the host or app. */
        clear();
        return false;
    }
}

} /* namespace af */
