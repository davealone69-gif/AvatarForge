/*
 * ground.frag -- the studio floor.
 *
 * A matte dielectric with a large-scale albedo variation (two octaves of value noise on the world
 * position, evaluated analytically so no texture is needed), lit by the same model as the avatar:
 * the sun with the shadow map, the SH irradiance, and the analytic sky for the specular reflection.
 * The result is a floor that receives the figure's cast shadow and its reflection, which is what
 * makes a render read as "on the ground" rather than "floating in a void".
 */
#include "common.glsl"

layout(location = 0) out vec4 fragColor;

float afHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float afValueNoise(vec2 p) {
    af_const vec2 i = floor(p);
    af_const vec2 f = fract(p);
    af_const vec2 u = f * f * (3.0 - 2.0 * f);
    af_const float a = afHash(i);
    af_const float b = afHash(i + vec2(1.0, 0.0));
    af_const float c = afHash(i + vec2(0.0, 1.0));
    af_const float d = afHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void main() {
    af_const vec3 n = vec3(0.0, 1.0, 0.0);
    af_const float ndlRaw = dot(n, cam.uSunDir.xyz);
    af_const float ndl = max(ndlRaw, 0.0);
    af_const float shadow = afShadow(vWorldPos, ndlRaw);

    af_const float grain = afValueNoise(vWorldPos.xz * 3.0) * 0.5 + afValueNoise(vWorldPos.xz * 11.0) * 0.5;
    af_const float tint = cam.uGround.z;
    af_const vec3 albedo = vec3(tint) * (0.86 + 0.14 * grain);

    af_const vec3 irradiance = afIrradiance(n) * 0.75;
    /* Contact darkening: the floor is not a perfect plane in a studio either, and without this the
       horizon line reads as a cut-out edge. */
    af_const float contact = clamp(0.35 + 0.65 * smoothstep(0.0, 1.6, length(vWorldPos.xz)), 0.0, 1.0);

    af_const vec3 diffuse = albedo * (irradiance * contact + cam.uSunColor.rgb * cam.uSunDir.w * ndl * shadow);
    af_const vec3 specular = afSpecularIbl(n, normalize(cam.uCameraPos.xyz - vWorldPos), vec3(0.04), 0.78) * contact;
    fragColor = afApplyHighlight(vec4(diffuse + specular, 1.0), n, vWorldPos);
}
