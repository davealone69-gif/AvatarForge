/* Lookup over the complete generated GLES table. */
#include "af_shaders.h"
#include "af_shaders_gl.h"
#include <cstring>

namespace af {
const AfShaderGl* afShaderGl(const char* name, AfShaderStage stage) {
    if (!name) return nullptr;
    for (const AfShaderGl& shader : shaders_gl::kTable)
        if (shader.stage == stage && std::strcmp(shader.name, name) == 0) return &shader;
    return nullptr;
}
int afShaderCount() {
    return int(sizeof(shaders_gl::kTable) / sizeof(shaders_gl::kTable[0]));
}
} /* namespace af */
