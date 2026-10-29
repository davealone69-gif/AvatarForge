/*
 * bloom_down.frag -- the downsample half of the bloom chain (the "dual filter" from Jimenez'
 * SIGGRAPH 2014 presentation), evaluated as a 13-tap box so the result is stable under motion
 * instead of shimmering.
 *
 * post.uPostParams.x carries the threshold: the first level selects the bright pixels, later levels
 * just filter what is already there (which is what keeps the glow smooth rather than blocky).
 */
#include "common.glsl"

layout(location = 5) in vec2 vFullscreenUv;  /* 0..4 belong to the shared varying block */
layout(location = 0) out vec4 fragColor;

void main() {
    af_const vec2 texel = post.uPostTexel.xy * max(post.uPostParams.y, 0.001) ;
    vec3 c = texture(uSourceTex, vFullscreenUv).rgb * 4.0;
    c += (texture(uSourceTex, vFullscreenUv + vec2(-texel.x, -texel.y)).rgb +
          texture(uSourceTex, vFullscreenUv + vec2( texel.x, -texel.y)).rgb +
          texture(uSourceTex, vFullscreenUv + vec2(-texel.x,  texel.y)).rgb +
          texture(uSourceTex, vFullscreenUv + vec2( texel.x,  texel.y)).rgb) * 2.0;
    c += texture(uSourceTex, vFullscreenUv + vec2(-texel.x * 2.0, 0.0)).rgb +
         texture(uSourceTex, vFullscreenUv + vec2( texel.x * 2.0, 0.0)).rgb +
         texture(uSourceTex, vFullscreenUv + vec2(0.0, -texel.y * 2.0)).rgb +
         texture(uSourceTex, vFullscreenUv + vec2(0.0,  texel.y * 2.0)).rgb;
    c *= 1.0 / 16.0;

    af_const float threshold = post.uPostParams.x;
    if (threshold > 0.0) {
        af_const float l = max(max(c.r, c.g), c.b);
        af_const float knee = threshold * 0.6 + 1e-5;
        af_const float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
        c *= max(soft * soft / (4.0 * knee), l - threshold) / max(l, 1e-5);
    }
    fragColor = vec4(c, 1.0);
}
