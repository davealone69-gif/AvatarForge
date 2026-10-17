/*
 * af_glb_read.h -- glTF 2.0 (.glb) import.
 *
 * The engine had a writer and a validator but no reader, which meant it could produce a model it
 * could not open again: no round-trip test, no way to load a GLB a user brought back, and no way
 * for the Viewer to show anything except what the kernel had just generated in memory. This is the
 * reader.
 *
 * Scope, stated exactly so nothing here is optimistic:
 *
 *   SUPPORTED   GLB container (v2); JSON chunk; BIN chunk; external .bin and data: URIs are NOT
 *               read (the app is offline and self-contained by design, and its own exports are
 *               single-file - a file that references the network is rejected rather than fetched);
 *               accessors of every component type, normalised or not, with and without `sparse`
 *               (the exporter writes sparse morph targets, so the reader must); the attribute set
 *               the renderer consumes (POSITION, NORMAL, TANGENT, TEXCOORD_0, COLOR_0, JOINTS_0,
 *               WEIGHTS_0); indices of u8/u16/u32; node hierarchies with matrix or TRS; skins with
 *               inverse bind matrices; animation samplers with LINEAR/STEP/CUBICSPLINE keyframes;
 *               materials with the metallic-roughness parameters and base-colour/emissive factors;
 *               embedded images in PNG (decoded to RGBA8).
 *
 *   REJECTED    glTF extensions of any kind (the file is reported as unsupported: an extension can
 *               change the meaning of everything else, and guessing would be worse than refusing);
 *               JPEG images (reported per-image, import continues - the model is still usable
 *               without its albedo map, and the caller is told exactly which image and why);
 *               Draco/meshopt compression; non-glTF JSON.
 *
 * Robustness contract: `importGlb` NEVER crashes or reads out of bounds on malformed input. Every
 * offset, count and index is validated against the buffer it addresses, all arithmetic that could
 * overflow is done in 64 bits, and the only recursion is over a scene graph whose depth is capped.
 * The harness fuzzes this: truncated files, corrupted chunk lengths, hostile accessor counts and
 * cyclic node graphs must all come back as a clean error.
 */
#ifndef AF_GLB_READ_H
#define AF_GLB_READ_H

#include "af_math.h"
#include "af_png_decode.h"
#include "../include/afcore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace af {

/*
 * One drawable range: a glTF primitive, expressed as an index range into a shared vertex pool.
 *
 * Why not one vertex array per primitive: the exporter (correctly) lets every primitive of a mesh
 * reference the *same* POSITION accessor and selects its submesh with its own index range. Copying
 * that accessor once per primitive turned a 271 k-vertex avatar into 2.7 M vertices - 188 MB of
 * af_vertex - for no reason. Primitives that share an identical attribute set now share one pool,
 * which is also the layout the renderer wants: a single vertex buffer and a per-primitive index
 * range.
 */
struct ImportedPrimitive {
    int pool = -1;              /* index into GlbAsset::pools                         */
    size_t vertexOffset = 0;    /* first vertex this primitive addresses inside the pool */
    size_t vertexCount = 0;
    std::vector<uint32_t> indices;   /* relative to the pool, already offset-corrected   */
    int material = -1;
    int mesh = -1;            /* index into the document's meshes array               */
    int node = -1;            /* the first node that references this mesh, or -1      */
    std::string meshName;     /* for diagnostics and the asset browser                */
};

struct ImportedMaterial {
    std::string name;
    float baseColor[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    bool doubleSided = false;
    int baseColorTexture = -1;   /* index into GlbAsset::textures, -1 when absent */
    int metallicRoughnessTexture = -1;
    int normalTexture = -1;
    int emissiveTexture = -1;
};

struct ImportedTexture {
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   /* tight RGBA8, decoded from the embedded PNG */
};

struct ImportedNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    Mat4 local = Mat4::identity();
    Mat4 world = Mat4::identity();   /* filled by resolveWorldTransforms() */
    int mesh = -1;
    int skin = -1;
};

struct ImportedSkin {
    std::string name;
    std::vector<int> joints;                 /* node indices */
    std::vector<Mat4> inverseBindMatrices;   /* parallel to joints; identity when absent */
    int skeletonRoot = -1;
};

/* One animation channel: target node + path + keyframes. Values are stored as vec4 for
   translation/scale as well (w unused), so one sampler type covers all three paths. */
struct ImportedAnimationChannel {
    int node = -1;
    int path = 0;                       /* 0 translation, 1 rotation, 2 scale */
    std::vector<float> times;           /* seconds, strictly increasing as written */
    std::vector<Vec4> values;
    int interpolation = 0;              /* 0 linear, 1 step, 2 cubicspline (in/out tangents kept) */
    std::vector<Vec4> inTangents;
    std::vector<Vec4> outTangents;
};

struct ImportedAnimation {
    std::string name;
    float duration = 0.0f;
    std::vector<ImportedAnimationChannel> channels;
};

/* A problem the importer survived: the asset is still usable, but the caller must know. */
struct ImportWarning {
    std::string what;
    std::string detail;
};

struct GlbAsset {
    std::string generator;
    std::string version;
    std::vector<ImportedNode> nodes;
    std::vector<int> sceneRoots;
    /* Vertex pools, shared by primitives with an identical attribute set. */
    std::vector<std::vector<af_vertex>> pools;
    std::vector<ImportedPrimitive> primitives;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedTexture> textures;
    std::vector<ImportedSkin> skins;
    std::vector<ImportedAnimation> animations;
    std::vector<ImportWarning> warnings;
    std::vector<std::string> extensionsUsed;   /* recorded so the caller can say why it refused */

    /** Vertices actually stored (each pool once) - what the importer's memory cost is. */
    size_t uniqueVertexCount() const {
        size_t n = 0;
        for (const auto& pool : pools) n += pool.size();
        return n;
    }
    /** Vertex references summed over primitives - what a naive per-primitive copy would store. */
    size_t referencedVertexCount() const {
        size_t n = 0;
        for (const auto& p : primitives) n += p.vertexCount;
        return n;
    }
    size_t triangleCount() const {
        size_t n = 0;
        for (const auto& prim : primitives) n += prim.indices.size() / 3;
        return n;
    }
    /* Bounds over every primitive, in asset space. Empty primitives yield a zero box. */
    void bounds(Vec3* outMin, Vec3* outMax) const;
};

/**
 * Reads a GLB from memory. Returns AF_OK, or a negative af_error (see afcore.h). On failure `out`
 * is cleared. On success `out->warnings` may still be non-empty - warnings are things the asset
 * survives, errors are things it does not.
 */
int importGlb(const uint8_t* data, size_t size, GlbAsset* out, std::string* error);

/** Reads a GLB from disk. Same contract. */
int importGlbFile(const char* path, GlbAsset* out, std::string* error);

/**
 * Fills `world` for every node, honouring parent/child links. Detects cycles (a hostile file can
 * make one) and returns false, leaving the traversal depth-capped so it cannot recurse forever.
 */
bool resolveWorldTransforms(GlbAsset* asset, std::string* error);

} /* namespace af */
#endif /* AF_GLB_READ_H */
