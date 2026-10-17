/*
 * af_shaders.h -- the generated shader tables (see tools/embed_shaders.py).
 */
#ifndef AF_SHADERS_H
#define AF_SHADERS_H

#include <cstdint>

namespace af {

enum AfShaderStage { AF_STAGE_VERT = 0, AF_STAGE_FRAG = 1 };

/* Vulkan: SPIR-V words. In GLSL, arrays of uint32_t are 4-byte aligned and SPIR-V only requires
   4-byte alignment, so the baked arrays are directly usable in VkShaderModuleCreateInfo. */
struct AfShaderVk {
    const char* name;
    AfShaderStage stage;
    const uint32_t* words;
    uint32_t wordCount;
};

/* GLES 3.1: GLSL ES 3.10 source text. */
struct AfShaderGl {
    const char* name;
    AfShaderStage stage;
    const char* source;
};

/* Looks a shader up by (pipeline name, stage). Returns nullptr when it does not exist, which the
   backends treat as a hard initialisation failure rather than falling back to an unlit path. */
const AfShaderVk* afShaderVk(const char* name, AfShaderStage stage);
const AfShaderGl* afShaderGl(const char* name, AfShaderStage stage);
int afShaderCount();

} /* namespace af */

#endif /* AF_SHADERS_H */
