/* Separate GLES GLSL and native Vulkan SPIR-V tables; see tools/embed*_shaders.py. */
#ifndef AF_SHADERS_H
#define AF_SHADERS_H

#include <cstdint>

namespace af {
enum AfShaderStage { AF_STAGE_VERT = 0, AF_STAGE_FRAG = 1, AF_STAGE_COMP = 2 };
struct AfShaderGl {
    const char* name;
    AfShaderStage stage;
    const char* source;
};
struct AfShaderVk {
    const char* name;
    AfShaderStage stage;
    const uint32_t* words;
    uint32_t wordCount;
};
/* Unknown shaders/stages return nullptr, never an unlit substitute. */
const AfShaderGl* afShaderGl(const char* name, AfShaderStage stage);
const AfShaderVk* afShaderVk(const char* name, AfShaderStage stage);
int afShaderCount();    /* GLES stage count, including host-mock shader coverage */
int afShaderVkCount();  /* native Vulkan stage count */
} /* namespace af */
#endif
