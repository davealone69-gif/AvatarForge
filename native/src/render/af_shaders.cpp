/*
 * af_shaders.cpp -- lookup over the generated GLES shader table.
 *
 * Keeping the lookup here (rather than in the generated headers) means the backends see one
 * small interface and the headers stay pure data, which keeps regeneration cheap and diffable.
 */
#include "af_shaders.h"
#include "af_shaders_gl.h"

#include <cstring>

namespace af {

const AfShaderGl* afShaderGl(const char* name, AfShaderStage stage) {
    for (const AfShaderGl& s : shaders_gl::kTable)
        if (s.stage == stage && std::strcmp(s.name, name) == 0) return &s;
    return nullptr;
}

int afShaderCount() {
    return int(sizeof(shaders_gl::kTable) / sizeof(shaders_gl::kTable[0]));
}

} /* namespace af */
