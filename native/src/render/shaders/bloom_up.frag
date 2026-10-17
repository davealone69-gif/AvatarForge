/*
 * bloom_up.frag -- the upsample half: a 3x3 tent filter, added into the finer level with additive
 * blending (both backends configure ONE/ONE for this pass only). The tent widens as it climbs the
 * chain because post.uPostTexel is the *target* level's texel size, which is what turns a stack of
 * separate blurs into one smooth glow.
 */
#include "common.glsl"

layout(location = 5) in vec2 vFullscreenUv;  /* 0..4 belong to the shared varying block */
layout(location = 0) out vec4 fragColor;

void main() {
    af_const vec2 texel = post.uPostTexel.xy * max(post.uPostParams.y, 0.001);
    vec3 c = texture(uSourceTex, vFullscreenUv).rgb * 4.0;
    c += (texture(uSourceTex, vFullscreenUv + vec2(-texel.x, 0.0)).rgb +
          texture(uSourceTex, vFullscreenUv + vec2( texel.x, 0.0)).rgb +
          texture(uSourceTex, vFullscreenUv + vec2(0.0, -texel.y)).rgb +
          texture(uSourceTex, vFullscreenUv + vec2(0.0,  texel.y)).rgb) * 2.0;
    c += texture(uSourceTex, vFullscreenUv + vec2(-texel.x, -texel.y)).rgb +
         texture(uSourceTex, vFullscreenUv + vec2( texel.x, -texel.y)).rgb +
         texture(uSourceTex, vFullscreenUv + vec2(-texel.x,  texel.y)).rgb +
         texture(uSourceTex, vFullscreenUv + vec2( texel.x,  texel.y)).rgb;
    fragColor = vec4(c / 16.0, 1.0);
}
