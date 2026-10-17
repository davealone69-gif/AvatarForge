/*
 * fullscreen.vert -- the vertex stage every post-processing pass shares.
 *
 * Generates one oversized triangle covering the target from gl_VertexIndex / gl_VertexID, so no
 * vertex buffer, no index buffer and no attribute state is involved. The mapping is
 * gl_Position = uv * 2 - 1 in BOTH backends: Vulkan's framebuffer origin is top-left and its clip
 * Y points down, GL's origin is bottom-left and its clip Y points up, and in each case the texel a
 * pass wrote for uv.y = 0 is the texel this stage reads for uv.y = 0. One convention, both APIs,
 * no per-backend flip in the post chain.
 */
#include "common.glsl"

#ifdef AF_VULKAN
#define AF_VID gl_VertexIndex
#else
#define AF_VID gl_VertexID
#endif

layout(location = 5) out vec2 vFullscreenUv;  /* 0..4 belong to the shared varying block */

void main() {
    af_const vec2 uv = vec2(float((AF_VID << 1) & 2), float(AF_VID & 2));
    vFullscreenUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
