/*
 * line.frag -- flat, unlit overlay. Lines are debug/inspection geometry, so they ignore every
 * lighting term; they are drawn in the HDR pass with blending off and a fixed line width.
 */
/* Minimal stage interface: these programs never read the shared varyings, and a Vulkan
   pipeline must be able to provide every vertex input its shaders consume. */
#define AF_VARY_MINIMAL 1
#include "common.glsl"

layout(location = 5) in vec4 vLineColor;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(vLineColor.rgb * 1.6, vLineColor.a);
}
