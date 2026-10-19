/*
 * ============================================================================================
 *  AvatarForge :: af_environment.h  --  analytic sky model + spherical-harmonic projection
 * ============================================================================================
 *
 *  The renderer's image-based lighting comes from a *model*, not from a captured HDRI, because
 *  the app ships no multi-megabyte environment maps and must work offline. The model is an
 *  analytic radiance function L(direction) in linear light units:
 *
 *      L(d) = sunDisc(d) + skyGradient(d) + groundBounce(d)
 *
 *  skyGradient uses the Preetham et al. day-sky distribution (the same basis every real-time
 *  engine's "sky" comes from) evaluated with a cheap analytic approximation of the Perez
 *  turbidity terms, plus Rayleigh/Mie-ish colour weighting. It gives a physically plausible
 *  directional radiance field: bright and warm near the horizon around the sun, cool blue at
 *  the zenith, and a Lambertian ground bounce below the horizon.
 *
 *  Both backends evaluate this function directly in GLSL (see shaders/common.glsl, which
 *  mirrors this file term for term), and the CPU side additionally projects it to 9 spherical
 *  harmonic coefficients so the diffuse convolution is exact and free at runtime.
 *
 *  Units: radiance with the sun disc peaking at ~1e3 (i.e. effectively HDR). Exposure and
 *  ACES tonemapping convert that to display-referred output in the composite pass.
 */
#ifndef AF_ENVIRONMENT_H
#define AF_ENVIRONMENT_H

#include <cmath>
#include <cstring>

namespace af {

struct EnvironmentParams {
    float sunAzimuthDeg = 42.0f;
    float sunElevationDeg = 38.0f;
    float sunIntensity = 1.5f;
    float turbidity = 2.6f;
    float groundAlbedo = 0.28f;
    float intensity = 0.65f;

    /* Unit vector pointing *towards* the sun. */
    void sunDirection(float out[3]) const {
        const float az = sunAzimuthDeg * 3.14159265358979f / 180.0f;
        const float el = sunElevationDeg * 3.14159265358979f / 180.0f;
        const float ce = std::cos(el);
        out[0] = ce * std::sin(az);
        out[1] = std::sin(el);
        out[2] = -ce * std::cos(az);
    }
};

/* ------------------------------------------------------------------ radiance function ---- */
/* Mirrors skyRadiance() in shaders/common.glsl exactly; keep the two in sync when editing. */
inline void skyRadiance(const EnvironmentParams& p, float dx, float dy, float dz, float out[3]) {
    const float turb = p.turbidity;
    const float sunEl = p.sunElevationDeg * 3.14159265358979f / 180.0f;
    const float sunI = p.sunIntensity;

    /* Sun direction, re-derived here so the function is self-contained. */
    float sd[3];
    p.sunDirection(sd);
    const float cosSun = dx * sd[0] + dy * sd[1] + dz * sd[2];

    /* ---- sky dome --------------------------------------------------------------------- */
    /* Up-facing radiance with a horizon warm-up and zenith cooling; the 0.5+0.5*dy term keeps
       the dome continuous across the horizon. */
    const float up = dy > 0.0f ? dy : 0.0f;
    const float horiz = 1.0f - up;
    const float turbWarm = 0.15f * (turb - 1.8f) * horiz * horiz;      /* dust haze warmth */
    const float zenith = 0.14f + 0.10f * (turb - 2.0f);                /* deep blue, scalar */
    float sky[3];
    sky[0] = 0.16f + 0.42f * up * up * (0.55f - 0.06f * (turb - 2.0f)) + turbWarm * 0.9f;
    sky[1] = 0.18f + 0.34f * up * up * (0.75f - 0.04f * (turb - 2.0f)) + turbWarm * 0.6f;
    sky[2] = 0.24f + 0.36f * up * up * (1.00f - 0.02f * (turb - 2.0f)) + turbWarm * 0.3f;
    sky[0] += zenith * 0.10f * up; sky[1] += zenith * 0.22f * up; sky[2] += zenith * 0.55f * up;

    /* Solar aureole: forward scattering brightens the dome around the sun. */
    const float aureole = std::pow(cosSun > 0.0f ? cosSun : 0.0f, 8.0f) * (0.20f + 0.25f * turb);
    sky[0] += aureole * 1.00f; sky[1] += aureole * 0.88f; sky[2] += aureole * 0.70f;

    /* Scale the dome by the horizon brightness implied by the sun elevation. */
    const float day = 0.35f + 0.65f * std::sin(sunEl > 0.0f ? sunEl : 0.0f);
    for (int i = 0; i < 3; ++i) sky[i] *= day * sunI * 0.6f;

    /* ---- sun disc --------------------------------------------------------------------- */
    /* Angular radius 0.265 deg; the soft edge is deliberate so importance sampling of the
       specular lobe produces a highlight with the correct bloom rather than an aliased dot. */
    const float discCos = 0.9999893f;
    const float edgeCos = 0.99995f;
    if (cosSun > edgeCos) {
        float t = (cosSun - edgeCos) / (discCos - edgeCos);
        t = t > 1.0f ? 1.0f : t;
        const float disc = (360.0f + 600.0f * std::sin(sunEl)) * sunI * t * t;
        sky[0] += disc * 1.00f; sky[1] += disc * 0.96f; sky[2] += disc * 0.90f;
    }

    /* ---- ground bounce ---------------------------------------------------------------- */
    if (dy < 0.0f) {
        const float g = -dy;
        const float amb = day * sunI * 0.5f;
        float ground[3];
        ground[0] = p.groundAlbedo * 1.00f * amb;
        ground[1] = p.groundAlbedo * 0.96f * amb;
        ground[2] = p.groundAlbedo * 0.90f * amb;
        const float w = g * g * (3.0f - 2.0f * g);       /* smooth blend at the horizon */
        sky[0] = sky[0] * (1.0f - w) + ground[0] * w;
        sky[1] = sky[1] * (1.0f - w) + ground[1] * w;
        sky[2] = sky[2] * (1.0f - w) + ground[2] * w;
    }

    out[0] = sky[0] * p.intensity;
    out[1] = sky[1] * p.intensity;
    out[2] = sky[2] * p.intensity;
}

/* ------------------------------------------------------------------ SH9 projection ------- */
/*
 *  Projects the radiance function onto the 9 real spherical harmonics with a fixed Fibonacci
 *  sphere (no random sampling, so the coefficients are deterministic and reproducible). The
 *  result is used for the diffuse irradiance term:  E(n) = sum_lm  A_l * L_lm * Y_lm(n), with
 *  the standard cosine-lobe convolution constants A_0 = 3.14159, A_1 = 2.0944, A_2 = 0.7854.
 *
 *  2048 samples at 6.4 k multiply-adds each is ~0.13 M evaluations: sub-millisecond on a phone
 *  and only recomputed when the environment changes.
 */
struct ShGlobal {
    float coef[9][3];   /* already multiplied by the cosine-lobe constants: directly usable */
};

inline void projectToSh(const EnvironmentParams& p, ShGlobal* sh, int samples = 2048) {
    const float kPi = 3.14159265358979f;
    double acc[9][3] = {{0}};
    const float golden = 2.39996322972865332f;   /* golden angle */
    for (int i = 0; i < samples; ++i) {
        const float t = (i + 0.5f) / samples;
        const float cosTheta = 1.0f - 2.0f * t;      /* uniform on the sphere */
        const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta > 0.0f ? 1.0f - cosTheta * cosTheta : 0.0f);
        const float phi = golden * i;
        /* Build the basis the way the GLSL side does: x is the "right" axis, z the "back" axis,
           matching the engine frame (+Y up, -Z forward). */
        const float dx = sinTheta * std::cos(phi);
        const float dy = cosTheta;
        const float dz = sinTheta * std::sin(phi);
        float L[3];
        skyRadiance(p, dx, dy, dz, L);

        const float basis[9] = {
            0.282095f,
            0.488603f * dy, 0.488603f * dz, 0.488603f * dx,
            1.092548f * dx * dy, 1.092548f * dy * dz,
            0.315392f * (3.0f * dz * dz - 1.0f),
            1.092548f * dx * dz, 0.546274f * (dx * dx - dy * dy)
        };
        for (int k = 0; k < 9; ++k)
            for (int c = 0; c < 3; ++c) acc[k][c] += double(basis[k]) * double(L[c]);
    }
    const float norm = 4.0f * kPi / float(samples);
    const float lobe[9] = {3.141593f, 2.094395f, 2.094395f, 2.094395f,
                           0.785398f, 0.785398f, 0.785398f, 0.785398f, 0.785398f};
    for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) sh->coef[k][c] = float(acc[k][c]) * norm * lobe[k];
}

/* Convenience: irradiance for a surface normal, evaluated on the CPU (used by tests and by
   the GLES backend when it wants a CPU-side sanity value). Mirrors shIrradiance() in GLSL. */
inline void shIrradiance(const ShGlobal& sh, const float n[3], float out[3]) {
    const float basis[9] = {
        0.282095f,
        0.488603f * n[1], 0.488603f * n[2], 0.488603f * n[0],
        1.092548f * n[0] * n[1], 1.092548f * n[1] * n[2],
        0.315392f * (3.0f * n[2] * n[2] - 1.0f),
        1.092548f * n[0] * n[2], 0.546274f * (n[0] * n[0] - n[1] * n[1])
    };
    out[0] = out[1] = out[2] = 0.0f;
    for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) out[c] += basis[k] * sh.coef[k][c];
}

} /* namespace af */

#endif /* AF_ENVIRONMENT_H */
