/*
 * ============================================================================================
 *  AvatarForge :: af_render.h -- GLES 3.1 real-time renderer
 * ============================================================================================
 *
 * One supported backend: GlRenderer (af_gl.cpp), using EGL, VAOs, UBOs and FBOs.
 * Renderer::create reports GLES initialization failures directly. No alternate API is
 * attempted and no software renderer is substituted. Android's driver implementation is
 * reported verbatim; an OS-level GLES translation layer is not an app Vulkan backend.
 *
 *  Lighting model
 *  ---------------------------------------------------
 *      * direct   : one directional sun, GGX specular, Smith visibility, 5-tap PCF shadow map
 *      * ambient  : analytic sky model (see af_environment.h) projected to 9 SH coefficients on
 *                   the CPU, giving true directional diffuse irradiance, not a constant ambient
 *      * specular IBL : 16-sample GGX importance sampling of the same analytic sky plus the
 *                   Karis analytic DFG approximation - no prefiltered cubemap, no LUT texture
 *      * skin     : sub-surface scattering - wrapped diffuse + thickness-driven translucency
 *                   (red-biased, from the baked subsurface map) + fresnel-weighted single scatter
 *      * eyes     : cornea at IOR 1.376 with real refraction of the view ray into the globe,
 *                   iris sampled where the refracted ray lands, limbal darkening, wet specular
 *      * hair     : Kajiya-Kay style anisotropic specular along the tangent with a shifted
 *                   secondary lobe, energy-conserving against the diffuse term
 *      * cloth    : sheen lobe (Charlie distribution) plus the standard PBR lobes
 *      * metal    : full metallic path (no diffuse, tinted F0) for armour and equipment
 *      * post     : threshold + 4-level dual-filter bloom, ACES filmic tonemap, vignette,
 *                   ordered dither to kill banding, sRGB encode
 *
 *  Coordinate frame and units are the engine's: +Y up, -Z forward, metres, and the model already
 *  sits on y = 0, so the renderer never moves the mesh.
 */
#ifndef AF_RENDER_H
#define AF_RENDER_H

#include <cstdint>
#include <string>

#include "../../include/afcore.h"

/* ANativeWindow is opaque to us; forward declaration keeps this header usable in host builds. */
struct ANativeWindow;

namespace af {

/* ------------------------------------------------------------------ configuration --------- */

/* Quality presets the UI and the SDK both use. `AUTO` measures the device: it starts from the
   middle preset and steps *down* at runtime if the smoothed frame time exceeds the 30 FPS
   budget (thermal/battery awareness - see Renderer::renderFrame()). */
enum class QualityPreset { AUTO = 0, LOW = 1, MEDIUM = 2, HIGH = 3 };

struct RendererOptions {
    QualityPreset quality      = QualityPreset::AUTO;
    bool  enableShadows        = true;
    bool  enableBloom          = false;
    bool  enableSss            = true;   /* sub-surface scattering on skin/lips                */
    int   msaaSamples          = 4;      /* clamped to 1..4 (and to GL_MAX_SAMPLES) by the backend */
    int   shadowMapSize        = 2048;   /* clamped to 512..2048; square, depth-only               */
    int   bloomLevels          = 4;

    /* Environment. Defaults are a clear late-afternoon studio exterior - the look the reference
       renders in docs/ were made with. All parameters are live-editable from the viewer. */
    float sunAzimuthDeg        = 42.0f;
    float sunElevationDeg      = 38.0f;
    float sunIntensity         = 1.5f;   /* irradiance multiplier, pre-tonemap HDR             */
    float turbidity            = 2.6f;   /* atmospheric turbidity, 1.8..6.0                    */
    float groundAlbedo         = 0.28f;
    float exposure             = 0.65f;
    float environmentIntensity = 0.65f;
    float shadowSoftness       = 1.0f;   /* PCF radius in texels                               */
    bool  groundPlane          = true;   /* draws the contact-shadow plane under the figure    */
};

struct CameraState {
    float yawDeg    = 180.0f;   /* orbit around +Y; 180 looks at the model's front (-Z)       */
    float pitchDeg  = 4.0f;     /* + looks down                                               */
    float distance  = 3.5f;     /* metres from the orbit target; frames a ~1.85 m avatar      */
    float targetY   = 0.95f;    /* orbit target height (mid-torso for a 1.75 m avatar)        */
    float fovDeg    = 32.0f;    /* vertical field of view; 32 mm-ish is flattering on faces   */
    float rollDeg   = 0.0f;
};

/* Per-frame timing the UI shows in the stats drawer, and the budget controller reads. */
struct RendererStats {
    float frameMs        = 0.0f;   /* presented-frame interval EMA, not a GPU timestamp       */
    float cpuMs          = 0.0f;   /* CPU wall time inside renderFrame()                      */
    float fps            = 0.0f;
    float gpuMs          = -1.0f;  /* -1 = unknown; no GPU timestamp query is implemented     */
    int   drawCalls      = 0;
    int   triangles      = 0;
    int   vertices       = 0;
    int   shadowMapSize  = 0;
    int   msaaSamples    = 1;
    int   bloomLevels    = 0;
    uint64_t textureBytes = 0;
    int   qualityLevel   = 2;      /* effective QualityPreset after automatic stepping        */
    int   frames         = 0;      /* accepted present requests, not mere loop iterations     */
    int   frameAttempts  = 0;
    int   frameSubmissions = 0;
    int   presentRequestsAccepted = 0;
};

struct RendererInfo {
    std::string backend;      /* "gles31"                                                */
    std::string deviceName;   /* e.g. "Adreno (TM) 650"                                  */
    std::string apiVersion;   /* e.g. "OpenGL ES 3.2"                                    */
    std::string driver;       /* driver version string, shown in the health screen       */
    bool supportsShadows = false;
    bool supportsMsaa    = false;
    bool supportsFloatHdr = false;
    bool supportsAnisotropy = false;
};

/* ------------------------------------------------------------------ the renderer ---------- */
/*
 *  Ownership: one Renderer per surface. All methods must be called from the thread that created
 *  it (the app runs a dedicated render thread that owns the ANativeWindow). attachEngine() does
 *  not copy the mesh; the renderer reads the engine's buffers directly and uploads them to GPU
 *  memory when invalidateMesh() says the geometry/pose changed.
 */
class Renderer {
public:
    /* Creates a backend for `window`. On failure returns nullptr and fills *error with a
       human-readable reason (shown verbatim by AvatarEngine.health()). */
    static Renderer* create(ANativeWindow* window, const RendererOptions& options, std::string* error);

    virtual ~Renderer() {}

    /* Non-empty after a frame failed. JNI converts this into an exception so the bounded
       recovery policy can act; an empty string means no frame error was recorded. */
    virtual std::string frameError() const { return {}; }

    /* Surface size in pixels. Safe to call every frame with the current size; the backend
       recreates swapchain/framebuffers only when the size actually changed. */
    virtual bool resize(int width, int height) = 0;

    /* Points the renderer at an engine handle. The handle stays owned by the caller and must
       outlive the renderer. Passing nullptr detaches (renders an empty stage). */
    virtual void attachEngine(af_engine* engine) = 0;

    /* Marks the vertex buffer stale: call after apply_pose / apply_clip / apply_blendshapes,
       after any stage that rebuilds geometry, or after a texture re-bake. */
    virtual void invalidateMesh() = 0;

    virtual void setCamera(const CameraState& camera) = 0;
    virtual void setOptions(const RendererOptions& options) = 0;
    virtual RendererOptions options() const = 0;

    /* Renders one frame and presents it. `deltaSeconds` drives the automatic quality
       controller and any time-based effects (the environment sun has no animation, so the
       figure itself is animated by the engine's clips, not by the renderer). */
    virtual void renderFrame(float deltaSeconds) = 0;

    /* Viewer overlays. Both are real draws in the same pass:
         skeleton  - line list of the 71 joints with a per-bone-class colour ramp
         flat      - facet shading (derivative normals) for inspection of the extracted mesh */
    virtual void setSkeletonVisible(bool visible) = 0;
    virtual void setFlatShading(bool flat) = 0;

    /* Highlights one af_region (or -1 for none) by tinting its submesh. Used by the region
       inspector: the UI taps the surface, does a CPU ray cast, and asks for the region name. */
    virtual void setHighlightRegion(int region) = 0;

    /* Plays one of the kernel's 15 authored clips. The renderer owns the playback clock: every
       frame it advances the phase, calls af_engine_apply_clip() and re-uploads the deformed
       vertex stream, so a clip plays at the display rate with no per-frame work on the UI thread
       and no Kotlin/native round trip. `clip` is an af_clip id; AF_CLIP_REST or -1 stops playback
       and returns the figure to the bind pose. */
    virtual void setClip(int clip, float speed, bool loop) = 0;
    virtual int   currentClip() const = 0;
    virtual float clipTime() const = 0;

    /* Waits for the GPU to idle. Needed before the engine frees or rebuilds buffers that the
       renderer may still be reading (e.g. before af_engine_destroy or a geometry rebuild). */
    virtual void waitIdle() = 0;

    virtual RendererStats stats() const = 0;
    virtual RendererInfo info() const = 0;
};

/* ------------------------------------------------------------------ CPU-side picking ------ */
/*
 *  Ray/avatar intersection used by "tap the model to inspect a region". Brute force over the
 *  engine's index buffer with a slab test and a back-face-culled Möller-Trumbore; ~0.4 M
 *  triangles is a few milliseconds on a mid-range phone, and it runs on the UI thread only in
 *  response to a tap, so no acceleration structure is warranted.
 *
 *  `rayOrigin` / `rayDir` are in world space (the same space as af_engine_vertices()) and the
 *  ray direction must be normalised. Returns the af_region hit, or -1. `outDistance` and
 *  `outPoint` (3 floats) may be null.
 */
int pickRegion(const af_engine* engine, const float rayOrigin[3], const float rayDir[3],
               float* outDistance, float* outPoint);

} /* namespace af */

#endif /* AF_RENDER_H */
