/*
 * forward.vert -- avatar geometry pass.
 *
 * The engine hands the renderer a vertex buffer that is already in world space (the kernel does
 * skinning and blendshapes on the CPU, so what the renderer draws is what the GLB exports), which
 * is why there is no model matrix anywhere in this pipeline. That is deliberate: one deformation
 * implementation, one result.
 *
 * Attribute locations match af_vk.cpp's avatarAttrs / af_gl.cpp's vertex arrays and the 88-byte
 * af_vertex layout in afcore.h.
 */
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUv;
layout(location = 4) in vec4 aColor;

void main() {
    vWorldPos = aPos;
    vNormal = aNormal;
    vTangent = aTangent;
    vUv = aUv;
    vColor = aColor;
    gl_Position = afClip(cam.uViewProj * vec4(aPos, 1.0));
}
