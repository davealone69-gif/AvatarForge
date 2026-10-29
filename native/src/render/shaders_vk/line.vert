/*
 * line.vert -- bone overlay (and the future ground grid). Vertices are written by the backend once
 * per mesh change: 28 bytes = vec3 position + vec4 colour, coloured by bone class.
 */
/* Minimal stage interface: these programs never read the shared varyings, and a Vulkan
   pipeline must be able to provide every vertex input its shaders consume. */
#define AF_VARY_MINIMAL 1
#include "common.glsl"

layout(location = 0) in vec3 lPos;
layout(location = 1) in vec4 lColor;

layout(location = 5) out vec4 vLineColor;

void main() {
    vLineColor = lColor;
    gl_Position = afClip(cam.uViewProj * vec4(lPos, 1.0));
}
