/*
 * af_avatar_lod.h -- renderer-owned, deformation-safe index-only avatar LODs.
 *
 * LOD levels contain indices into the engine's original interleaved vertex stream. No vertex is
 * averaged, reweighted, re-morphed, or reordered: animation and blendshape updates can therefore
 * keep uploading the kernel's live vertex buffer unchanged while the renderer switches index
 * ranges. Simplification is chart-, submesh-, and skin-influence-local; unsupported input falls
 * back to the exact source indices.
 */
#ifndef AF_AVATAR_LOD_H
#define AF_AVATAR_LOD_H

#include "afcore.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace af {

constexpr int kAvatarLodLevels = 3;

struct AvatarLodRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

struct AvatarLodLevel {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    std::vector<AvatarLodRange> submeshes;
};

class AvatarLodSet {
public:
    /*
     * Builds LOD 0 (the exact source indices) and up to two progressively coarser index streams.
     * Returns false only when the input arrays/ranges are malformed; on failure the set is empty.
     * LOD indices always refer to original vertex indices, preserving the engine's CPU deformation
     * and every source vertex attribute. `restVertices` provides stable clustering bounds; the
     * renderer still draws the current/deformed vertices.
     */
    bool build(const af_vertex* restVertices, size_t vertexCount,
               const uint32_t* sourceIndices, size_t indexCount,
               const af_submesh* submeshes, size_t submeshCount);

    void clear();
    int levelCount() const { return static_cast<int>(levels_.size()); }
    const AvatarLodLevel& level(int index) const;
    const std::vector<uint32_t>& indices() const { return indices_; }
    int triangleCount(int level) const;

private:
    std::vector<uint32_t> indices_;
    std::vector<AvatarLodLevel> levels_;
};

} /* namespace af */

#endif /* AF_AVATAR_LOD_H */
