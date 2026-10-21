/*
 * af_texture.h -- procedural PBR texture synthesis baked onto the UV atlas.
 *
 * Design note: every pattern is a function of the 3D surface point, not of the UV coordinate.
 * That makes the bake independent of how the charts were laid out (no seam discontinuity when a
 * pattern crosses a chart border) and lets the exact same material description drive both the
 * offline bake here and any runtime re-bake after the user changes skin tone or fabric.
 *
 * Maps produced (all at the requested resolution):
 *   BASE_COLOR          RGBA8  albedo, alpha unused (1.0)
 *   NORMAL              RGBA8  tangent-space normal from analytic micro-relief
 *   METALLIC_ROUGHNESS  RGBA8  G = roughness, B = metallic (glTF packing)
 *   OCCLUSION           R8     crevice darkening from mesh curvature and cover geometry
 *   SUBSURFACE          RGBA8  R = thickness, G = scatter radius, B = tint index
 */
#ifndef AF_TEXTURE_H
#define AF_TEXTURE_H

#include "af_field.h"
#include "af_uv.h"
#include "../include/afcore.h"
#include <cstdint>
#include <string>
#include <vector>

namespace af {

struct MaterialDef {
    std::string name;
    Vec3  baseColor{0.8f, 0.8f, 0.8f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ior = 1.4f;
    float sheen = 0.0f;
    float sheenTint = 0.5f;
    float subsurface = 0.0f;
    float anisotropic = 0.0f;
    float detailScale = 1.0f;      /* metres per pattern tile                         */
};

/** One material per af_region; used by the GLB material table and by the baker. */
const MaterialDef& materialForRegion(int region);

struct TextureSet {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> baseColor;          /* RGBA8, width*height*4   */
    std::vector<uint8_t> normal;             /* RGBA8                   */
    std::vector<uint8_t> metallicRoughness;  /* RGBA8 (G=rough, B=metal) */
    std::vector<uint8_t> occlusion;          /* R8                      */
    std::vector<uint8_t> subsurface;         /* RGBA8                   */
};

/** Options for the bake. `simplifiedSkin` disables the finest pore octaves for low-RAM runs. */
struct BakeOptions {
    int   width = 2048;
    int   height = 2048;
    int   dilationPasses = 6;
    bool  bakeMicroRelief = true;
    bool  bakeHairScroll = false;   /* reserved: hair shells bake their own maps */
    int   threads = 0;
};

/**
 * Bakes all five maps for `mesh` (which must already carry UVs and curvature) using `metrics`
 * for landmark-driven features (iris, lips, brows, nails, blush) and `spec` for skin tone,
 * age and fabric choices.
 */
bool bakeTextures(const Mesh& mesh, const UVLayout& layout, const BodyMetrics& metrics,
                  const af_spec& spec, const BakeOptions& options, TextureSet* out);

/** Total byte size of a texture set (for the engine's memory accounting). */
uint64_t textureSetBytes(const TextureSet& set);

/** Chooses the largest power-of-two atlas whose peak bake workspace fits the remaining budget. */
int atlasResolutionForBudget(uint64_t budgetBytes, uint64_t retainedBytes);

/** Conservative bake working-set estimate: five output maps plus raster/dilation masks. */
uint64_t atlasBakeWorkspaceBytes(int resolution);

/** Marker colour used where nothing was baked; makes un-baked texels obvious instead of black. */
extern const uint8_t kUnbakedRGBA[4];

} // namespace af
#endif /* AF_TEXTURE_H */
