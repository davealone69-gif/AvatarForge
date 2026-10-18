/*
 * ============================================================================================
 *  AvatarForge :: shaders/common.glsl  --  the whole shading model, in one place
 * ============================================================================================
 *
 *  This file is compiled TWICE from the same source by tools/embed_shaders.py:
 *
 *    * for Vulkan  (AF_VULKAN defined)  -> glslc -> SPIR-V, embedded as uint32 words, and
 *    * for GLES 3.1 (AF_GLES defined)   -> "#version 310 es" + inlined includes, embedded as text.
 *
 *  Both dialects therefore evaluate *the same* lighting maths. The only differences left are the
 * ones the two APIs genuinely impose:
 *
 *      descriptor bindings  Vulkan takes (set, binding); GLES assigns sampler units by uniform
 *                           name at link time (GlRenderer::createPrograms does that).
 *      clip space           Vulkan's Y points down and its depth range is [0,1]; the projection is
 *                           built in the GL convention and afClip() flips Y for Vulkan.
 *      target addressing    afShadowUv()/postUv() reconcile the framebuffer origin flip so both
 *                           backends read the same texel for the same light-space position.
 *
 *  Everything else is shared code: the analytic sky, the SH irradiance, the BRDF, the
 *  skin/eye/hair/cloth/metal models, the shadow test and the tonemapper.
 *
 *  Atlas semantics (kept in step with src/af_texture.cpp, which is the authority):
 *
 *      baseColor        sRGB-encoded albedo; hardware-linearised on sample, never linearised here.
 *      normal           tangent-space normal, .xyz packed as n*0.5+0.5.
 *      metallicRoughness .g = roughness, .b = metallic (glTF convention; .r is unused and 1.0).
 *      occlusion        one channel, linear 0..1 (uploaded as R8).
 *      subsurface       .r = scattering thickness (encoded *4, saturating at 0.25 m)
 *                       .g = scatter radius (same encoding)
 *                       .b = scatter tint (skin melanin / cloth dye)
 *
 *  Uniform block layouts mirror src/render/af_render_common.h struct-for-struct and are asserted
 *  there with static_assert, so a layout drift breaks the build instead of the image.
 */
#ifndef AF_COMMON_GLSL
#define AF_COMMON_GLSL

/* ------------------------------------------------------------------ dialect bridge -------- */
/*
 * The macro parameters are named s/b, never "set"/"binding": the preprocessor substitutes *every*
 * occurrence of a parameter name, so `#define AF_UBO(set, binding) layout(set = set, ...)` would
 * expand the qualifying keyword itself into the argument and the shader would not compile.
 */
#ifdef AF_VULKAN
#define AF_UBO(s, b) layout(std140, set = s, binding = b)
#define AF_TEX(s, b) layout(set = s, binding = b)
#else
#define AF_UBO(s, b) layout(std140, binding = b)
#define AF_TEX(s, b)
#endif

/* Material kinds: the ABI's values (af_render_common.h::materialKindForRegion). */
#define AF_KIND_GENERIC 0
#define AF_KIND_SKIN    1
#define AF_KIND_EYE     2
#define AF_KIND_HAIR    3
#define AF_KIND_CLOTH   4
#define AF_KIND_METAL   5
#define AF_KIND_TEETH   6
#define AF_KIND_NAIL    7

/*
 * Local "const" is a dialect difference, not a style choice: GLSL ES 3.10 only allows const on
 * variables initialised by a *constant expression*, so `const vec3 e = someFunction(n);` is a
 * compile error on the GLES path while desktop/Vulkan GLSL accepts it. af_const keeps the intent
 * visible in the source and compiles to the right thing in both dialects.
 */
#ifdef AF_GLES
#define af_const
#else
#define af_const const
#endif

#define AF_PI 3.14159265358979

/* ------------------------------------------------------------------ uniform blocks --------- */
AF_UBO(0, 0) uniform CameraBlock {
    mat4 uViewProj;
    mat4 uView;
    mat4 uLightViewProj;
    vec4 uCameraPos;     /* xyz = eye                                                     */
    vec4 uSunDir;        /* xyz = unit vector towards the sun, w = intensity              */
    vec4 uSunColor;      /* rgb = sun tint                                                */
    vec4 uSkyProps;      /* x = turbidity, y = ground albedo, z = env intensity, w = exposure */
    vec4 uEnvFlags;      /* x = shadows, y = bloom, z = sss, w = flat shading             */
    vec4 uUiParams;      /* x = highlight region, y = highlight on, z = shadow radius (uv), w = 1/height */
    vec4 uGround;        /* x = floor y, y = ground plane on, z = ground tint             */
    vec4 uSh[9];         /* cosine-convolved SH9 irradiance                               */
} cam;

AF_UBO(0, 2) uniform PostBlock {
    vec4 uPostParams;    /* x = bloom threshold, y = filter radius, z = bloom intensity, w = exposure */
    vec4 uPostTexel;     /* xy = 1 / source texel size                                    */
} post;

AF_UBO(1, 0) uniform MaterialBlock {
    vec4 uBaseColorFactor;  /* rgb = linear tint, a = opacity                            */
    vec4 uPbr;              /* x = metallic, y = roughness, z = ior, w = sheen           */
    vec4 uPbr2;             /* x = subsurface, y = anisotropy, z = kind, w = has normal  */
    vec4 uTexFlags;         /* x = base colour, y = mr, z = ao, w = atlas uploaded       */
    vec4 uRegion;           /* x = region id, y = highlighted, z = iris uv scale, w = eye */
} mat;

/* ------------------------------------------------------------------ textures -------------- */
AF_TEX(0, 1) uniform sampler2D uShadowMap;
AF_TEX(0, 3) uniform sampler2D uSourceTex;
AF_TEX(0, 4) uniform sampler2D uBloomTex;

AF_TEX(1, 1) uniform sampler2D uBaseColorTex;
AF_TEX(1, 2) uniform sampler2D uNormalTex;
AF_TEX(1, 3) uniform sampler2D uMetalRoughTex;
AF_TEX(1, 4) uniform sampler2D uOcclusionTex;
AF_TEX(1, 5) uniform sampler2D uSubsurfaceTex;

#ifdef AF_VULKAN
struct AfForwardLight {
    vec4 positionRange;
    vec4 colorIntensity;
    vec4 reserved;
};
layout(std430, set = 2, binding = 0) readonly buffer ForwardLightBuffer {
    vec4 header;
    AfForwardLight lights[16];
} fpLights;
layout(std430, set = 2, binding = 1) readonly buffer ForwardTileBuffer {
    uvec4 tileLists[16 * 9];
} fpTiles;
#endif

/* ------------------------------------------------------------------ stage interface -------- */
/*
 * The vertex->fragment interface, declared once so the two stages cannot drift. The stage kind is
 * defined by the embedder (#define AF_STAGE_VERTEX for .vert files), which is also what lets both
 * stages include this one file.
 */
#ifdef AF_STAGE_VERTEX
#define AF_VARY_OUT layout(location = 0) out vec3 vWorldPos; \
                   layout(location = 1) out vec3 vNormal;   \
                   layout(location = 2) out vec4 vTangent;  \
                   layout(location = 3) out vec2 vUv;       \
                   layout(location = 4) out vec4 vColor
#else
#define AF_VARY_OUT layout(location = 0) in vec3 vWorldPos; \
                   layout(location = 1) in vec3 vNormal;    \
                   layout(location = 2) in vec4 vTangent;   \
                   layout(location = 3) in vec2 vUv;        \
                   layout(location = 4) in vec4 vColor
#endif
#ifndef AF_VARY_MINIMAL
AF_VARY_OUT;
#endif

/* ------------------------------------------------------------------ clip conventions ------- */
/*
 * Applied to every position pushed through a matrix built by buildCameraUbo(). Vulkan's clip
 * space has +Y down; the projection is authored in the GL convention, so the flip happens here,
 * once, for both the camera and the light matrix - and only on Vulkan.
 */
vec4 afClip(vec4 clip) {
#ifdef AF_VULKAN
    clip.y = -clip.y;
#endif
    return clip;
}

/*
 * Light-space ndc -> shadow map uv.
 *
 * The shadow pass rasterises with afClip(), so map row 0 holds the light-space *top* on Vulkan
 * (framebuffer origin top-left) and the light-space *bottom* on GLES (origin bottom-left). Both
 * cases are one line apart here rather than in every caller.
 */
vec2 afShadowUv(vec3 ndc) {
#ifdef AF_VULKAN
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
#else
    return vec2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);
#endif
}

/* ------------------------------------------------------------------ environment ----------- */
/*
 * Analytic sky radiance, mirroring af_environment.h::skyRadiance() term for term. The CPU projects
 * this same function onto spherical harmonics for the diffuse term, so if the two ever drift the
 * diffuse and specular lighting stop agreeing with each other.
 *
 * Radiance units, sun disc around 1e3: HDR, tonemapped once in the composite pass.
 */
vec3 afSkyRadiance(vec3 d) {
    af_const float turb = cam.uSkyProps.x;
    af_const float sunI = cam.uSunDir.w;
    af_const vec3 sd = cam.uSunDir.xyz;
    af_const float cosSun = dot(d, sd);

    af_const float up = max(d.y, 0.0);
    af_const float horiz = 1.0 - up;
    af_const float turbWarm = 0.15 * (turb - 1.8) * horiz * horiz;
    af_const float zenith = 0.14 + 0.10 * (turb - 2.0);
    vec3 sky;
    sky.r = 0.16 + 0.42 * up * up * (0.55 - 0.06 * (turb - 2.0)) + turbWarm * 0.9;
    sky.g = 0.18 + 0.34 * up * up * (0.75 - 0.04 * (turb - 2.0)) + turbWarm * 0.6;
    sky.b = 0.24 + 0.36 * up * up * (1.00 - 0.02 * (turb - 2.0)) + turbWarm * 0.3;
    sky += vec3(zenith * 0.10, zenith * 0.22, zenith * 0.55) * up;

    /* Solar aureole: forward scattering brightens the dome around the sun. */
    af_const float aureole = pow(max(cosSun, 0.0), 8.0) * (0.20 + 0.25 * turb);
    sky += vec3(1.00, 0.88, 0.70) * aureole;

    af_const float sunEl = asin(clamp(sd.y, -1.0, 1.0));
    af_const float day = 0.35 + 0.65 * max(sin(sunEl), 0.0);
    sky *= day * sunI * 0.6;

    /* Sun disc: 0.265 deg with a soft edge, so importance sampling produces a highlight with the
       right bloom instead of an aliased dot. */
    af_const float discCos = 0.9999893;
    af_const float edgeCos = 0.99995;
    if (cosSun > edgeCos) {
        af_const float t = clamp((cosSun - edgeCos) / (discCos - edgeCos), 0.0, 1.0);
        af_const float disc = (360.0 + 600.0 * sin(sunEl)) * sunI * t * t;
        sky += vec3(1.00, 0.96, 0.90) * disc;
    }

    if (d.y < 0.0) {
        af_const float g = -d.y;
        af_const float amb = day * sunI * 0.5;
        af_const vec3 ground = vec3(1.00, 0.96, 0.90) * (cam.uSkyProps.y * amb);
        af_const float w = g * g * (3.0 - 2.0 * g);
        sky = mix(sky, ground, w);
    }
    return sky * cam.uSkyProps.z;
}

/* Diffuse irradiance from the SH the CPU projected (cosine lobe already folded in). */
vec3 afIrradiance(vec3 n) {
    af_const float b0 = 0.282095;
    af_const vec3 b1 = vec3(0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x);
    af_const vec3 b2 = vec3(1.092548 * n.x * n.y, 1.092548 * n.y * n.z, 0.315392 * (3.0 * n.z * n.z - 1.0));
    af_const vec3 b3 = vec3(1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y), 0.0);
    vec3 e = cam.uSh[0].xyz * b0;
    e += cam.uSh[1].xyz * b1.x + cam.uSh[2].xyz * b1.y + cam.uSh[3].xyz * b1.z;
    e += cam.uSh[4].xyz * b2.x + cam.uSh[5].xyz * b2.y + cam.uSh[6].xyz * b2.z;
    e += cam.uSh[7].xyz * b3.x + cam.uSh[8].xyz * b3.y;
    return max(e, vec3(0.0));
}

/* ------------------------------------------------------------------ BRDF ------------------ */
float afDistributionGgx(float ndh, float roughness) {
    af_const float a = roughness * roughness;
    af_const float a2 = a * a;
    af_const float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(AF_PI * d * d, 1e-7);
}

float afGeometrySmith(float ndv, float ndl, float roughness) {
    af_const float r = roughness + 1.0;
    af_const float k = (r * r) / 8.0;
    af_const float gv = ndv / (ndv * (1.0 - k) + k);
    af_const float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

vec3 afFresnelSchlick(float cosTheta, vec3 f0) {
    af_const float f = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

/* Karis' analytic fit to the split-sum environment BRDF (the mobile approximation of DFG).
   Four multiply-adds instead of a LUT texture: the app ships no extra assets. */
vec3 afEnvBrdf(vec3 f0, float roughness, float ndv) {
    af_const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    af_const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    af_const vec4 r = roughness * c0 + c1;
    af_const float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    af_const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float afRadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec3 afImportanceSampleGgx(vec2 xi, vec3 n, float roughness) {
    af_const float a = roughness * roughness;
    af_const float phi = 2.0 * AF_PI * xi.x;
    af_const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    af_const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    af_const vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    af_const vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    af_const vec3 tx = normalize(cross(up, n));
    af_const vec3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

/*
 * Specular image-based lighting: 16-sample GGX importance sampling of the analytic sky.
 *
 * There is no prefiltered cubemap to sample because there is no captured environment to prefilter:
 * the sky is a closed-form function, so one "prefiltered lookup" is one evaluation of that
 * function, and the roughness level of detail becomes the number of samples and the cone width.
 * Same maths a prefiltered mip chain approximates, no environment assets in the APK.
 */
vec3 afSpecularIbl(vec3 n, vec3 v, vec3 f0, float roughness) {
    af_const vec3 r = reflect(-v, n);
    af_const float ndv = max(dot(n, v), 1e-4);
    af_const uint samples = 16u;
    vec3 acc = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < samples; ++i) {
        af_const vec2 xi = vec2(float(i) / float(samples), afRadicalInverse(i));
        af_const vec3 h = afImportanceSampleGgx(xi, n, max(roughness, 0.045));
        af_const vec3 l = normalize(2.0 * dot(v, h) * h - v);
        af_const float ndl = dot(n, l);
        if (ndl <= 0.0) continue;
        /* Widen the sampled cone towards the mirror direction as roughness falls: grazing angles
           otherwise alias into fireflies. */
        af_const float blend = clamp(1.0 - roughness * 1.6, 0.0, 1.0);
        af_const vec3 dir = normalize(mix(l, r, blend));
        acc += afSkyRadiance(dir) * ndl;
        weight += ndl;
    }
    af_const vec3 prefiltered = weight > 0.0 ? acc / weight : afSkyRadiance(r);
    return prefiltered * afEnvBrdf(f0, roughness, ndv);
}

/* ------------------------------------------------------------------ shadows ---------------- */
/*
 * One directional shadow map with 5-tap Poisson PCF. The radius arrives through uUiParams.z
 * already expressed in uv units (the backend divides the softness control by the map size), so
 * the control means the same thing at every shadow resolution.
 */
float afShadow(vec3 worldPos, float ndl) {
    if (cam.uEnvFlags.x < 0.5) return 1.0;
    af_const vec4 lc = cam.uLightViewProj * vec4(worldPos, 1.0);
    if (lc.w <= 0.0) return 1.0;
    af_const vec3 ndc = lc.xyz / lc.w;
    af_const vec2 uv = afShadowUv(ndc);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    /* Slope-scaled bias plus a constant that covers the texel footprint. */
    af_const float bias = 0.0016 + 0.0040 * (1.0 - clamp(ndl, 0.0, 1.0));
    af_const float radius = clamp(cam.uUiParams.z, 1e-5, 0.01);
    af_const vec2 taps[5] = vec2[5](vec2(0.0, 0.0), vec2(0.94, 0.55), vec2(-0.94, 0.55),
                                 vec2(0.94, -0.55), vec2(-0.94, -0.55));
    float sum = 0.0;
    for (int i = 0; i < 5; ++i) {
        af_const float d = texture(uShadowMap, uv + taps[i] * radius).r;
        sum += (ndc.z - bias) <= d ? 1.0 : 0.0;
    }
    return sum * 0.2;
}

/* ------------------------------------------------------------------ surface sampling ------- */
struct AfSurface {
    vec3  albedo;
    float alpha;
    float metallic;
    float roughness;
    float occlusion;
    float ior;
    float sheen;
    float anisotropy;
    float thickness;      /* 0 = opaque, 1 = very thin (encoded /4 in the atlas) */
    float scatterRadius;
    float scatterTint;
    int   kind;
};

AfSurface afSampleSurface(vec2 uv) {
    AfSurface s;
    af_const float atlasFlags = mat.uTexFlags.w;
    af_const float hasBase = atlasFlags * mat.uTexFlags.x;
    af_const float hasMr = atlasFlags * mat.uTexFlags.y;
    af_const float hasAo = atlasFlags * mat.uTexFlags.z;

    af_const vec4 baseTex = texture(uBaseColorTex, uv);
    /* af_vertex::color is the per-vertex base-colour multiplier (the engine fills it with 1.0;
       the hook exists so a future region tint or vertex bake costs nothing to add). */
    s.albedo = mix(vec3(1.0), baseTex.rgb, hasBase) * mat.uBaseColorFactor.rgb;
    s.alpha = mix(1.0, baseTex.a, hasBase) * mat.uBaseColorFactor.a;

    af_const vec4 mr = texture(uMetalRoughTex, uv);
    s.metallic = clamp(mat.uPbr.x * mix(1.0, mr.b, hasMr), 0.0, 1.0);
    s.roughness = clamp(mat.uPbr.y * mix(1.0, mr.g, hasMr), 0.035, 1.0);
    s.occlusion = mix(1.0, texture(uOcclusionTex, uv).r, hasAo);
    s.ior = max(mat.uPbr.z, 1.01);
    s.sheen = mat.uPbr.w;
    s.anisotropy = mat.uPbr2.y;

    af_const vec4 ss = texture(uSubsurfaceTex, uv);
    s.thickness = clamp(ss.r * 0.25, 0.0, 0.25);
    s.scatterRadius = clamp(ss.g * 0.25, 0.0, 0.25);
    s.scatterTint = ss.b;
    s.kind = int(mat.uPbr2.z + 0.5);
    return s;
}

/* F0 from the interface IOR: glTF's dielectric default (4%) for skin, cloth and cloth-like
   surfaces, the real value where the kernel measured one (cornea 1.376, enamel 1.62). */
vec3 afF0FromIor(float ior) {
    af_const float f = (ior - 1.0) / (ior + 1.0);
    return vec3(f * f);
}

/* Tangent-space normal mapping. When the atlas is absent or flat shading is on, the geometric
   normal is returned unchanged. */
vec3 afPerturbNormal(vec3 n, vec4 tangent, vec2 uv) {
    if (mat.uTexFlags.w < 0.5 || mat.uPbr2.w < 0.5 || cam.uEnvFlags.w > 0.5) return n;
    af_const vec3 t0 = tangent.xyz;
    if (dot(t0, t0) < 1e-6) return n;
    af_const vec3 t = normalize(t0 - n * dot(n, t0));
    af_const vec3 b = cross(n, t) * (tangent.w < 0.0 ? -1.0 : 1.0);
    vec3 nm = texture(uNormalTex, uv).xyz * 2.0 - 1.0;
    nm.xy *= 1.0;
    return normalize(mat3(t, b, n) * nm);
}

/* ------------------------------------------------------------------ kind-specific ---------- */
/*
 * Sub-surface scattering on skin.
 *
 * Two terms, both driven by the atlas the kernel baked:
 *   * wrapped diffuse with a per-channel wrap width, which is the classic cheap approximation of
 *     multiple scattering - light wraps around the terminator, and it wraps *further* in red than
 *     in blue, which is what makes skin read as skin rather than as plastic;
 *   * a transmission term for thin, back-lit regions (ears, nose rim, lids), gated by the baked
 *     thickness so it only appears where the geometry is actually thin.
 */
vec3 afSkinDiffuse(AfSurface s, vec3 n, vec3 v, vec3 l, float ndl) {
    af_const float thin = clamp(s.thickness * 4.0, 0.0, 1.0);
    af_const vec3 wrap = vec3(0.52, 0.34, 0.20) * (0.5 + 0.9 * thin);
    af_const vec3 wrapped = (vec3(ndl) + wrap) / (vec3(1.0) + wrap);
    vec3 diff = s.albedo * max(wrapped, vec3(0.0)) * (1.0 - s.metallic);

    /* Transmission: the fraction of light that enters on the far side and exits towards the eye. */
    af_const float back = max(dot(v, -l), 0.0);
    af_const float trans = pow(back, 2.5) * thin * 0.55;
    af_const vec3 tint = mix(vec3(1.0), vec3(1.0, 0.62 + 0.25 * s.scatterTint, 0.55 + 0.20 * s.scatterTint), 0.75);
    diff += s.albedo * tint * trans;
    return diff;
}

/* ------------------------------------------------------------------ inspector highlight --- */
/* The viewer's region inspector tints the picked submesh. It is an overlay, not a material, so it
   is applied once, here, after shading. */
vec4 afApplyHighlight(vec4 colour, vec3 n, vec3 p) {
    if (cam.uUiParams.y * mat.uRegion.y < 0.5) return colour;
    af_const vec3 v = normalize(cam.uCameraPos.xyz - p);
    af_const float rim = pow(1.0 - clamp(abs(dot(normalize(n), v)), 0.0, 1.0), 2.0);
    return vec4(mix(colour.rgb, vec3(0.20, 0.68, 1.00), 0.16 + 0.34 * rim), colour.a);
}

/* An orthonormal basis around a normal, with a non-degenerate reference axis. */
void afBasisFromNormal(vec3 n, out vec3 t, out vec3 b) {
    af_const vec3 ref = abs(n.y) > 0.95 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    t = normalize(cross(ref, n));
    b = cross(n, t);
}

/*
 * Eye shading: a real cornea, not a painted highlight.
 *
 * The cornea is a sphere of the anatomical anterior radius seated behind the surface point. The
 * view ray is refracted at the interface with the material's IOR (1.376 for cornea, from the
 * kernel's material table), the refracted ray is intersected with the globe, and the iris is
 * sampled where that ray lands - so the pupil and iris shift with the view angle, exactly the
 * parallax that makes a rendered eye look wet and alive. The atlas island's uv extent
 * (mat.uRegion.z, measured by the backend from the eye submesh) converts the metric offset into
 * atlas coordinates. Reflections on the tear film come from the standard specular IBL with a very
 * low roughness, and the limbus - the sclerocorneal junction - is darkened where the cornea's
 * curvature turns away.
 */
vec3 afEyeShading(AfSurface s, vec3 p, vec3 n, vec3 v, vec3 f0, vec2 uv) {
    af_const float corneaR = 0.0078;                       /* 7.8 mm anterior radius */
    af_const vec3 globe = p - n * corneaR;
    af_const vec3 rd = refract(-v, n, 1.0 / s.ior);
    vec3 dir = dot(rd, rd) > 0.5 ? rd : -v;

    /* Intersect the refracted ray with the iris plane's sphere (2 mm inside the cornea). */
    af_const float irisR = corneaR - 0.0020;
    af_const vec3 oc = globe - p;
    af_const float b = dot(dir, oc);
    af_const float c = dot(oc, oc) - irisR * irisR;
    af_const float disc = b * b - c;
    vec3 irisSample = p;
    if (disc > 0.0) {
        af_const float t = -b - sqrt(disc);
        if (t > 0.0) irisSample = p + dir * t;
    }
    vec3 eyeT, eyeB;
    afBasisFromNormal(n, eyeT, eyeB);
    af_const vec3 lateralVec = irisSample - globe;
    af_const vec2 lateral = vec2(dot(lateralVec, eyeT), dot(lateralVec, eyeB));
    af_const float scale = max(mat.uRegion.z, 1e-4);
    af_const vec2 uvOffset = clamp(lateral / max(irisR, 1e-4), -1.0, 1.0) * scale * 0.5;
    af_const vec3 refractedAlbedo = texture(uBaseColorTex, clamp(uv + uvOffset, vec2(0.0), vec2(1.0))).rgb;

    /* Limbal darkening: the shading terminator of the globe seen through the cornea. */
    af_const float limb = smoothstep(0.35, 0.95, length(uvOffset) / max(scale * 0.5, 1e-4));
    af_const vec3 iris = mix(refractedAlbedo, refractedAlbedo * 0.22, limb * 0.85);

    af_const float ndl = max(dot(n, cam.uSunDir.xyz), 0.0);
    af_const vec3 diffuse = iris * (afIrradiance(n) * 0.30 + cam.uSunColor.rgb * cam.uSunDir.w * ndl * 0.35);
    /* The tear film is nearly a mirror: roughness 0.04, F0 from the IOR, plus the specular IBL. */
    return diffuse + afSpecularIbl(n, v, f0, 0.04) * 1.15;
}

/* Kajiya-Kay strand specular, used for scalp hair, brows and lashes. */
float afKajiyaKay(vec3 tangent, vec3 n, vec3 l, vec3 v, float shift, float exponent) {
    af_const vec3 t = normalize(tangent + n * shift);
    af_const float tl = dot(t, l);
    af_const float tv = dot(t, v);
    af_const float sl = sqrt(max(1.0 - tl * tl, 0.0));
    af_const float sv = sqrt(max(1.0 - tv * tv, 0.0));
    return pow(max(sl * sv - tl * tv, 0.0), exponent);
}

/* Charlie sheen: the cloth-specific lobe that makes fabric look soft rather than glossy. */
float afCharlieD(float ndh, float roughness) {
    af_const float invR = 1.0 / max(roughness, 0.01);
    af_const float cos2h = ndh * ndh;
    af_const float sin2h = max(1.0 - cos2h, 0.0078125);
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * AF_PI);
}

float afAshikhminV(float ndv, float ndl) {
    return 1.0 / max(4.0 * (ndl + ndv - ndl * ndv), 1e-4);
}

vec3 afForwardPlus(vec3 p, vec3 n, vec3 v, vec3 albedo) {
#if defined(AF_VULKAN) && !defined(AF_STAGE_VERTEX)
    if (fpLights.header.w < 0.5) return vec3(0.0);
    float width = max(fpLights.header.y, 1.0);
    float height = max(fpLights.header.z, 1.0);
    uint tileX = min(uint(gl_FragCoord.x / (width / 16.0)), 15u);
    uint tileY = min(uint(gl_FragCoord.y / (height / 9.0)), 8u);
    uvec4 tile = fpTiles.tileLists[tileY * 16u + tileX];
    vec3 result = vec3(0.0);
    for (uint slot = 0u; slot < min(tile.x, 3u); ++slot) {
        AfForwardLight light = fpLights.lights[tile[slot + 1u]];
        vec3 toLight = light.positionRange.xyz - p;
        float distanceToLight = length(toLight);
        vec3 direction = toLight / max(distanceToLight, 1e-4);
        float range = max(light.positionRange.w, 0.01);
        float falloff = clamp(1.0 - distanceToLight / range, 0.0, 1.0);
        falloff *= falloff;
        float diffuse = max(dot(n, direction), 0.0);
        float specular = pow(max(dot(reflect(-direction, n), v), 0.0), 24.0);
        result += light.colorIntensity.rgb * light.colorIntensity.w * falloff *
                  (albedo * diffuse + vec3(specular * 0.08));
    }
    return result;
#else
    return vec3(0.0);
#endif
}

/* ------------------------------------------------------------------ the light loop --------- */
vec4 afShade(vec3 p, vec3 geometricNormal, vec4 tangent, vec2 uv, vec3 v) {
    af_const AfSurface s = afSampleSurface(uv);
    vec3 n = afPerturbNormal(normalize(geometricNormal), tangent, uv);
    af_const vec3 l = cam.uSunDir.xyz;
    af_const float ndlRaw = dot(n, l);
    af_const float ndl = max(ndlRaw, 0.0);
    af_const float ndv = max(dot(n, v), 1e-4);
    af_const vec3 f0 = mix(afF0FromIor(s.ior), s.albedo, s.metallic);
    af_const float shadow = afShadow(p, ndlRaw);
    af_const vec3 sun = cam.uSunColor.rgb * cam.uSunDir.w * shadow;

    vec3 diffuse;
    vec3 specular;

    if (s.kind == AF_KIND_EYE && mat.uRegion.w > 0.5) {
        /* The eye model owns both terms: it evaluates the globe's own diffuse and specular. */
        return afApplyHighlight(vec4(afEyeShading(s, p, n, v, f0, uv), s.alpha), n, p);
    }

    if (s.kind == AF_KIND_HAIR) {
        /*
         * Strand direction.
         *
         * The kernel's vertex tangents come from the uv charts, which say nothing about which way
         * a hair strand runs, so the strand direction is derived from the world instead: scalp hair
         * grows out of the crown and falls, so its local direction is the surface tangent that is
         * closest to vertical. Where the surface is nearly horizontal (the crown itself) that
         * degenerates and the interpolated uv tangent takes over. It is an approximation - a real
         * groom would carry per-strand data - and it is documented as one.
         */
        af_const vec3 upWorld = vec3(0.0, 1.0, 0.0);
        vec3 fallback = tangent.xyz;
        if (dot(fallback, fallback) < 1e-6) fallback = vec3(1.0, 0.0, 0.0);
        af_const vec3 radial = normalize(fallback - n * dot(n, fallback));
        vec3 strand = upWorld - n * dot(upWorld, n);
        af_const float strandLen = length(strand);
        strand = strandLen > 0.22 ? strand / strandLen : radial;
        if (dot(strand, radial) < 0.0) strand = -strand;

        af_const float aniso = clamp(0.25 + s.anisotropy * 1.75, 0.05, 1.0);
        af_const float shift = 0.06 * (0.35 + aniso);
        af_const float hi = afKajiyaKay(strand, n, l, v, shift, mix(48.0, 140.0, aniso));
        af_const float lo = afKajiyaKay(strand, n, l, v, -shift, mix(10.0, 26.0, aniso));
        af_const vec3 hairSpec = sun * (hi * 0.85 + lo * 0.35) * s.anisotropy;
        /* Hair is thin: it transmits a lot of light, which is why backlit hair glows. */
        af_const float back = pow(max(dot(v, -l), 0.0), 2.0);
        af_const vec3 backScatter = s.albedo * sun * back * 0.30 * clamp(s.thickness * 6.0, 0.0, 1.0);
        diffuse = s.albedo * afIrradiance(n) * s.occlusion * 0.55 + backScatter;
        specular = hairSpec * shadow + afSpecularIbl(n, v, vec3(0.05), max(s.roughness, 0.22)) * s.occlusion;
        return afApplyHighlight(vec4(diffuse + specular, s.alpha), n, p);
    }

    /* ---- diffuse ------------------------------------------------------------------------- */
    af_const vec3 irradiance = afIrradiance(n) * s.occlusion;
    if (s.kind == AF_KIND_SKIN) {
        /* Skin owns its diffuse lobe: wrapped + per-channel scatter, see afSkinDiffuse(). */
        af_const float sssOn = cam.uEnvFlags.z > 0.5 ? 1.0 : 0.0;
        diffuse = mix(s.albedo * ndl * (1.0 - s.metallic), afSkinDiffuse(s, n, v, l, ndl), sssOn);
    } else if (s.kind == AF_KIND_TEETH) {
        /* Enamel is translucent, so the back-scatter term is kept but the wrap is not. */
        diffuse = s.albedo * ndl * (1.0 - s.metallic) * 0.9
                + vec3(0.72, 0.82, 0.90) * pow(max(dot(v, -l), 0.0), 2.0) * clamp(s.thickness * 8.0, 0.0, 1.0) * 0.30;
    } else if (s.kind == AF_KIND_CLOTH) {
        diffuse = s.albedo * ndl * (1.0 - s.metallic);
        /* Cloth also takes a soft transmission term along the fibre silhouette. */
        diffuse += s.albedo * pow(max(dot(v, -l), 0.0), 2.0) * clamp(s.thickness * 8.0, 0.0, 1.0) * 0.25;
    } else {
        diffuse = s.albedo * ndl * (1.0 - s.metallic);
    }
    vec3 ambientDiffuse = s.albedo * irradiance * (1.0 - s.metallic);
    if (s.kind == AF_KIND_SKIN) {
        /* The ambient term uses the same wrap so the shadowed side of a face keeps its redness. */
        ambientDiffuse = afSkinDiffuse(s, n, v, n, 1.0) * 0.40 * irradiance
                       + ambientDiffuse * 0.60;
    }

    /* ---- specular ------------------------------------------------------------------------ */
    af_const float a = max(s.roughness * s.roughness, 1e-4);
    af_const vec3 h = normalize(l + v);
    af_const float ndh = max(dot(n, h), 0.0);
    af_const float vdh = max(dot(v, h), 1e-4);
    af_const vec3 f = afFresnelSchlick(vdh, f0);
    af_const vec3 specDirect = f * afDistributionGgx(ndh, s.roughness) * afGeometrySmith(ndv, ndl, s.roughness);
    specular = specDirect * sun * ndl;

    /* Cloth sheen: Charlie distribution with an Ashikhmin visibility and a Fresnel-weighted tint. */
    if (s.kind == AF_KIND_CLOTH || s.kind == AF_KIND_NAIL) {
        af_const float sheen = max(s.sheen, 0.15);
        af_const float sheenF = pow(clamp(1.0 - vdh, 0.0, 1.0), 5.0);
        af_const float sheenWeight = afCharlieD(ndh, max(s.roughness, 0.15)) * afAshikhminV(ndv, ndl) * sheenF * sheen;
        specular += vec3(sheenWeight) * sun * ndl * 4.0;
    }

    af_const vec3 specIbl = afSpecularIbl(n, v, f0, s.roughness) * s.occlusion;
    vec3 localLights = afForwardPlus(p, n, v, s.albedo * (1.0 - s.metallic));
    vec4 result = vec4(ambientDiffuse + diffuse * sun + specular + specIbl + localLights, s.alpha);
    return afApplyHighlight(result, n, p);
}

/* ------------------------------------------------------------------ tonemapping ------------ */
/* ACES filmic (Narkowicz's fit). Applied once, in the composite pass. */
vec3 afTonemapAces(vec3 x) {
    af_const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float afLuminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

/* Interleaved gradient noise: cheap, screen-stable dithering that removes 8-bit banding. */
float afDither(vec2 fragCoord) {
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

#endif /* AF_COMMON_GLSL */
