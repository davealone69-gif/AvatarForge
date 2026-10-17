/*
 * composite.frag -- the only place the image becomes display-referred.
 *
 * hdr = source + bloom * intensity
 *      -> exposure (photographic stops, from post.uPostParams.w)
 *      -> ACES filmic tonemap
 *      -> vignette
 *      -> ordered dither (kills 8-bit banding in the sky gradient)
 *      -> sRGB encode (the HDR target was linear, the swap chain is not)
 *
 * Writing the sRGB transfer function by hand rather than using an sRGB framebuffer is deliberate:
 * the bloom chain and the eye highlight are still linear at this point, and the encode has to
 * happen exactly once, after the tonemapper.
 */
#include "common.glsl"

layout(location = 5) in vec2 vFullscreenUv;  /* 0..4 belong to the shared varying block */
layout(location = 0) out vec4 fragColor;

vec3 afLinearToSrgb(vec3 c) {
    af_const vec3 lo = c * 12.92;
    af_const vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec3 hdr = texture(uSourceTex, vFullscreenUv).rgb;
    af_const float bloomAmount = post.uPostParams.z;
    if (bloomAmount > 0.0) hdr += texture(uBloomTex, vFullscreenUv).rgb * bloomAmount;

    hdr *= max(post.uPostParams.w, 0.0);
    vec3 colour = afTonemapAces(hdr);

    /* Vignette: a gentle falloff from the centre, which keeps the eye on the figure. */
    af_const vec2 centred = vFullscreenUv - 0.5;
    af_const float vignette = smoothstep(0.95, 0.30, length(centred) * 1.30);
    colour *= mix(0.72, 1.0, vignette);

    colour += (afDither(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);
    fragColor = vec4(afLinearToSrgb(clamp(colour, 0.0, 1.0)), 1.0);
}
