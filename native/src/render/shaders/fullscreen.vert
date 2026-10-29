/*
 * fullscreen.vert -- the vertex stage every post-processing pass shares.
 *
 * Generates one oversized triangle using gl_VertexID, with no vertex buffer.
 * GL clip space and framebuffer addressing share the same UV convention throughout post.
 */
#include "common.glsl"


layout(location = 5) out vec2 vFullscreenUv;  /* 0..4 belong to the shared varying block */

void main() {
    af_const vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vFullscreenUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
