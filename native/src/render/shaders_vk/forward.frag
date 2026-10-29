/*
 * forward.frag -- avatar shading. All of the maths lives in common.glsl; this file is the entry
 * point plus the small amount of per-fragment plumbing (view vector, output slot).
 */
#include "common.glsl"

layout(location = 0) out vec4 fragColor;

void main() {
    /* Early-out for fully transparent fragments: the atlas alpha is the opacity channel and
       nothing in the current pipeline is blended, so this only saves work on stray texels. */
    af_const vec3 viewDir = normalize(cam.uCameraPos.xyz - vWorldPos);
    fragColor = afShade(vWorldPos, vNormal, vTangent, vUv, viewDir);
    if (fragColor.a <= 0.0) discard;
}
