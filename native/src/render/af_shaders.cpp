/* Lookup over separate GLES GLSL and Vulkan SPIR-V tables. */
#include "af_shaders.h"
#include "af_shaders_gl.h"
#include "af_shaders_vk.h"
#include <cstring>

namespace af {
const AfShaderGl* afShaderGl(const char* name, AfShaderStage stage) {
    if (!name) return nullptr;
    for (const AfShaderGl& shader : shaders_gl::kTable)
        if (shader.stage == stage && std::strcmp(shader.name, name) == 0) return &shader;
    return nullptr;
}
const AfShaderVk* afShaderVk(const char* name, AfShaderStage stage) {
    if (!name) return nullptr;
    for (const AfShaderVk& shader : shaders_vk::kTable)
        if (shader.stage == stage && std::strcmp(shader.name, name) == 0 &&
            shader.wordCount > 0 && shader.words && shader.words[0] == 0x07230203u)
            return &shader;
    return nullptr;
}
int afShaderCount() { return int(sizeof(shaders_gl::kTable) / sizeof(shaders_gl::kTable[0])); }
int afShaderVkCount() { return int(sizeof(shaders_vk::kTable) / sizeof(shaders_vk::kTable[0])); }
} /* namespace af */
