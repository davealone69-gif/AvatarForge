/*
 * shadow.vert -- depth-only pass for the directional sun.
 *
 * One orthographic map wrapped tightly around the figure (see buildCameraUbo): the avatar is under
 * 2.5 m in every dimension, so a single 2048 map covers it at ~1.2 mm/texel, which is far more
 * than the 5-tap PCF footprint needs. No cascades, no atlas, no second light.
 *
 * The depth bias is applied by the pipeline (depthBiasConstantFactor/SlopeFactor in af_vk.cpp,
 * glPolygonOffset in af_gl.cpp), not here.
 */
/* Minimal stage interface: these programs never read the shared varyings, and a Vulkan
   pipeline must be able to provide every vertex input its shaders consume. */
#define AF_VARY_MINIMAL 1
#include "common.glsl"

layout(location = 0) in vec3 aPos;

void main() {
    gl_Position = afClip(cam.uLightViewProj * vec4(aPos, 1.0));
}
