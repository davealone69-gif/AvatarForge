/* Generated GLES 3.1 shader table interface; see tools/embed_shaders.py. */
#ifndef AF_SHADERS_H
#define AF_SHADERS_H

namespace af {
enum AfShaderStage { AF_STAGE_VERT = 0, AF_STAGE_FRAG = 1 };
struct AfShaderGl {
    const char* name;
    AfShaderStage stage;
    const char* source;
};
/* Unknown stages return nullptr; there is no unlit or alternate-API fallback. */
const AfShaderGl* afShaderGl(const char* name, AfShaderStage stage);
int afShaderCount();
} /* namespace af */
#endif
