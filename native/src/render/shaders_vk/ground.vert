/*
 * ground.vert -- the studio contact plane.
 *
 * One large quad at y = uGround.x, drawn with culling disabled so it stays visible from below.
 * The engine keeps the avatar's soles on y = 0 (+2 mm), so the plane is where the shadow lands and
 * where the ambient occlusion of the figure reads as a contact shadow.
 */
#include "common.glsl"

layout(location = 0) in vec3 aPos;

void main() {
    af_const float size = 12.0;
    af_const vec3 world = vec3(aPos.x * size, cam.uGround.x, aPos.z * size);
    vWorldPos = world;
    vNormal = vec3(0.0, 1.0, 0.0);
    vTangent = vec4(1.0, 0.0, 0.0, 1.0);
    vUv = aPos.xz * 0.5 + 0.5;
    vColor = vec4(1.0);
    gl_Position = afClip(cam.uViewProj * vec4(world, 1.0));
}
