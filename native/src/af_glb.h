/*
 * af_glb.h -- binary glTF 2.0 export.
 *
 * The export is the engine's contract with the outside world: whatever a user does in the app has
 * to survive leaving it, and the format that survives is glTF 2.0. Everything the engine produces
 * goes into ONE self-contained .glb with no external references:
 *
 *   - geometry, split into one primitive per material with a shared attribute set;
 *   - UVs, normals and tangents from the same mesh the renderer draws;
 *   - the skeleton as a node hierarchy, with inverse bind matrices and four-influence skin weights;
 *   - all 52 ARKit blendshapes as morph targets, stored as SPARSE accessors (a facial channel moves
 *     a few hundred vertices out of 176 000, and a dense target per channel would make the file
 *     52 times larger for no reason);
 *   - the animation clip library, sampled from the authored key data;
 *   - the five baked PBR maps as PNG images in the binary chunk;
 *   - an extras block with the spec, the landmarks and the build statistics, so a file generated on
 *     a phone can be audited later.
 */
#ifndef AF_GLB_H
#define AF_GLB_H

#include "af_body.h"
#include "af_mesh.h"
#include "af_rig.h"
#include "af_texture.h"
#include "af_uv.h"

#include <cstdint>
#include <vector>

namespace af {

struct GlbOptions {
    bool embedTextures = true;      /* false writes a geometry-only file (fast, for tests)       */
    bool includeMorphTargets = true;
    bool includeAnimations = true;
    bool includeTangents = true;
    /*
     * Animation sampling rate. The clips are authored as key data and sampled here: glTF can carry
     * either. Sampling keeps the file player-agnostic (no dependency on how a viewer interpolates
     * cubics) and costs nothing at these sizes - 30 Hz over a 2.4 s clip is 73 quaternions per
     * joint.
     */
    float animationSampleHz = 30.0f;
    /*
     * Places the exported avatar on y = 0.
     *
     * This is the one place a translation is legitimate, because it is a placement decision for a
     * FILE rather than a deformation of a model: the mesh is in bind space, so shifting it means
     * shifting the skeleton's root and post-multiplying every inverse bind matrix by the same
     * translation. Inside the engine nothing is ever moved this way - the field grounds the model
     * itself - and the offset is recorded in extras so a consumer can undo it.
     */
    bool groundToZero = true;
    bool prettyExtras = false;
};

struct GlbResult {
    size_t   byteSize = 0;
    size_t   jsonBytes = 0;
    size_t   binBytes = 0;
    int      primitives = 0;
    int      materials = 0;
    int      images = 0;
    int      animations = 0;
    int      morphTargets = 0;   /* morph targets written, one per ARKit channel, always 52 */
    int      emptyMorphs = 0;    /* of those: the ones that move no vertex on this build  */
    int      joints = 0;
    size_t   sparseDeltas = 0;
    float    groundOffsetY = 0.0f;
    double   millis = 0.0;
};

/**
 * Writes `mesh` (already unwrapped and skinned) as a GLB into `out`.
 * `layout`, `rig` and `textures` are optional; passing null omits the corresponding block.
 */
bool writeGlb(const Mesh& mesh, const UVLayout* layout, const Rig* rig, const TextureSet* textures,
              const BodyMetrics& metrics, const af_spec& spec, const GlbOptions& options,
              std::vector<uint8_t>* out, GlbResult* result);

} // namespace af
#endif /* AF_GLB_H */
