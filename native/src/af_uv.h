/*
 * af_uv.h -- UV atlas generation.
 *
 * Pipeline (all deterministic, no external library):
 *   1. chart segmentation  - faces are grown into islands that share a region label and stay
 *                            within an angular tolerance of the chart's average normal;
 *   2. projection          - each chart is projected onto its average-normal plane in metres,
 *                            so one global texel density applies to the whole atlas;
 *   3. packing             - shelf packing into the atlas with a gutter, rotating charts 90
 *                            degrees when that saves vertical space;
 *   4. seam splitting      - vertices shared by two charts are duplicated so UVs stay continuous
 *                            inside a chart (glTF requires one UV per vertex).
 *
 * The output is a NEW mesh with split vertices; every per-vertex attribute (region, curvature,
 * normals, tangents) travels with the split copies, so downstream stages (rigging, texture
 * baking, GLB export) all read the same mesh.
 */
#ifndef AF_UV_H
#define AF_UV_H

#include "af_mesh.h"
#include <cstdint>
#include <vector>

namespace af {

struct UVChart {
    int32_t firstFace = 0;          /* offset into `faces`                                 */
    int32_t faceCount = 0;
    int32_t material = 0;           /* af_region-derived material index                    */
    Vec2    rectMin{0, 0};          /* placement in UV space (0..1, origin top-left)        */
    Vec2    rectMax{0, 0};
    float   areaSqMeters = 0.0f;    /* 3D area of the chart                                */
};

struct UVLayout {
    std::vector<UVChart> charts;
    std::vector<int32_t> faces;     /* triangle index (i/3) per chart face slot             */
    int   texWidth = 2048;
    int   texHeight = 2048;
    float texelsPerMeter = 0.0f;    /* resulting density - reported so the SDK can expose it */
    float coverage = 0.0f;          /* fraction of the atlas occupied by charts              */
    int   gutterTexels = 4;
};

/**
 * Unwraps `mesh` (in place: UVs are written into `mesh.uvs`, vertices may be duplicated).
 * Returns false only if the mesh has no faces. `chartList` optionally receives the chart table
 * that the texture baker and the GLB exporter share.
 */
bool unwrapAtlas(Mesh* mesh, int texWidth, int texHeight, UVLayout* outLayout);

/** Fills uvs with a per-triangle planar fallback (used by tools that need a mesh without charts). */
void assignBoxProjectionUVs(Mesh* mesh);

} // namespace af
#endif /* AF_UV_H */
