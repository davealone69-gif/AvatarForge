/*
 * ============================================================================================
 *  AvatarForge :: af_render_capi.cpp  --  a C ABI for the renderer
 * ============================================================================================
 *
 *  af_render.h is a C++ interface (virtuals, std::string), which is the right shape for the JNI
 *  bridge and for the app, but not something the Rust crates can bind to. This file wraps the
 *  renderer in a flat C surface - the same one the engine kernel exposes - so that:
 *
 *      * Rust code (crates `af-sys` / `af`) drives exactly the same renderer as Kotlin does, with
 *        no second implementation and no duplicated state;
 *      * a future AIDL/bound-service client (or a plain C tool) can own a renderer without
 *        touching C++ headers.
 *
 *  Everything here is a one-line forward. The only decisions are:
 *      * errors are returned as af_result, with the human-readable reason available through
 *        af_renderer_last_error() (the reason lives in a thread-local buffer, exactly like the
 *        engine's last_error);
 *      * stats are copied into a POD struct rather than returned by value, so the ABI is stable
 *        across compilers.
 */
#include "af_render.h"
#include "af_render_common.h"

#include <cstring>
#include <string>

namespace {

/* One error string per thread: the renderer is used from one thread at a time, and a shared
   buffer would need a lock on the hot path for no benefit. */
thread_local std::string g_lastError;

af::Renderer* asRenderer(void* handle) { return reinterpret_cast<af::Renderer*>(handle); }

} /* namespace */

extern "C" {

/* ------------------------------------------------------------------ POD mirrors ----------- */
typedef struct af_render_options {
    int32_t quality;             /* af::QualityPreset */
    int32_t prefer_vulkan;
    int32_t enable_shadows;
    int32_t enable_bloom;
    int32_t enable_sss;
    int32_t msaa_samples;
    int32_t shadow_map_size;
    int32_t bloom_levels;
    float   sun_azimuth_deg;
    float   sun_elevation_deg;
    float   sun_intensity;
    float   turbidity;
    float   ground_albedo;
    float   exposure;
    float   environment_intensity;
    float   shadow_softness;
    int32_t ground_plane;
} af_render_options;

typedef struct af_render_camera {
    float yaw_deg, pitch_deg, distance, target_y, fov_deg, roll_deg;
} af_render_camera;

typedef struct af_render_info {
    char backend[32];
    char device_name[128];
    char api_version[64];
    char driver[96];
    int32_t supports_shadows, supports_msaa, supports_float_hdr, supports_anisotropy;
} af_render_info;

typedef struct af_render_stats {
    float frame_ms, cpu_ms, fps;
    int32_t draw_calls, triangles, vertices;
    int32_t shadow_map_size, msaa_samples, bloom_levels, quality_level, frames;
    uint64_t texture_bytes;
} af_render_stats;

void af_render_options_defaults(af_render_options* out) {
    if (!out) return;
    const af::RendererOptions d{};
    out->quality = int32_t(d.quality);
    out->prefer_vulkan = d.preferVulkan ? 1 : 0;
    out->enable_shadows = d.enableShadows ? 1 : 0;
    out->enable_bloom = d.enableBloom ? 1 : 0;
    out->enable_sss = d.enableSss ? 1 : 0;
    out->msaa_samples = d.msaaSamples;
    out->shadow_map_size = d.shadowMapSize;
    out->bloom_levels = d.bloomLevels;
    out->sun_azimuth_deg = d.sunAzimuthDeg;
    out->sun_elevation_deg = d.sunElevationDeg;
    out->sun_intensity = d.sunIntensity;
    out->turbidity = d.turbidity;
    out->ground_albedo = d.groundAlbedo;
    out->exposure = d.exposure;
    out->environment_intensity = d.environmentIntensity;
    out->shadow_softness = d.shadowSoftness;
    out->ground_plane = d.groundPlane ? 1 : 0;
}

static af::RendererOptions toCpp(const af_render_options& o) {
    af::RendererOptions options;
    options.quality = af::QualityPreset(o.quality);
    options.preferVulkan = o.prefer_vulkan != 0;
    options.enableShadows = o.enable_shadows != 0;
    options.enableBloom = o.enable_bloom != 0;
    options.enableSss = o.enable_sss != 0;
    options.msaaSamples = o.msaa_samples;
    options.shadowMapSize = o.shadow_map_size;
    options.bloomLevels = o.bloom_levels;
    options.sunAzimuthDeg = o.sun_azimuth_deg;
    options.sunElevationDeg = o.sun_elevation_deg;
    options.sunIntensity = o.sun_intensity;
    options.turbidity = o.turbidity;
    options.groundAlbedo = o.ground_albedo;
    options.exposure = o.exposure;
    options.environmentIntensity = o.environment_intensity;
    options.shadowSoftness = o.shadow_softness;
    options.groundPlane = o.ground_plane != 0;
    return options;
}

/* ------------------------------------------------------------------ lifetime -------------- */
void* af_renderer_create(void* native_window, const af_render_options* options) {
    g_lastError.clear();
    if (!native_window) {
        g_lastError = "af_renderer_create: native_window is null";
        return nullptr;
    }
    af::RendererOptions cppOptions = options ? toCpp(*options) : af::RendererOptions{};
    std::string error;
    af::Renderer* renderer =
        af::Renderer::create(static_cast<ANativeWindow*>(native_window), cppOptions, &error);
    if (!renderer) g_lastError = error;
    return renderer;
}

void af_renderer_destroy(void* handle) { delete asRenderer(handle); }

const char* af_renderer_last_error(void) { return g_lastError.c_str(); }

/* ------------------------------------------------------------------ framing --------------- */
int32_t af_renderer_resize(void* handle, int32_t width, int32_t height) {
    af::Renderer* r = asRenderer(handle);
    if (!r) return AF_ERR_INVALID_ARG;
    return r->resize(width, height) ? AF_OK : AF_ERR_INTERNAL;
}

int32_t af_renderer_attach_engine(void* handle, af_engine* engine) {
    af::Renderer* r = asRenderer(handle);
    if (!r) return AF_ERR_INVALID_ARG;
    r->attachEngine(engine);
    return AF_OK;
}

void af_renderer_invalidate_mesh(void* handle) {
    if (af::Renderer* r = asRenderer(handle)) r->invalidateMesh();
}

void af_renderer_set_camera(void* handle, const af_render_camera* camera) {
    af::Renderer* r = asRenderer(handle);
    if (!r || !camera) return;
    af::CameraState state{};
    state.yawDeg = camera->yaw_deg;
    state.pitchDeg = camera->pitch_deg;
    state.distance = camera->distance;
    state.targetY = camera->target_y;
    state.fovDeg = camera->fov_deg;
    state.rollDeg = camera->roll_deg;
    r->setCamera(state);
}

void af_renderer_set_options(void* handle, const af_render_options* options) {
    af::Renderer* r = asRenderer(handle);
    if (!r || !options) return;
    r->setOptions(toCpp(*options));
}

void af_renderer_set_overlays(void* handle, int32_t skeleton, int32_t flat, int32_t highlight_region) {
    af::Renderer* r = asRenderer(handle);
    if (!r) return;
    r->setSkeletonVisible(skeleton != 0);
    r->setFlatShading(flat != 0);
    r->setHighlightRegion(highlight_region);
}

void af_renderer_set_clip(void* handle, int32_t clip, float speed, int32_t loop) {
    if (af::Renderer* r = asRenderer(handle)) r->setClip(clip, speed, loop != 0);
}

int32_t af_renderer_current_clip(void* handle) {
    af::Renderer* r = asRenderer(handle);
    return r ? r->currentClip() : -1;
}

float af_renderer_clip_time(void* handle) {
    af::Renderer* r = asRenderer(handle);
    return r ? r->clipTime() : 0.0f;
}

void af_renderer_frame(void* handle, float delta_seconds) {
    if (af::Renderer* r = asRenderer(handle)) r->renderFrame(delta_seconds);
}

void af_renderer_wait_idle(void* handle) {
    if (af::Renderer* r = asRenderer(handle)) r->waitIdle();
}

/* ------------------------------------------------------------------ introspection --------- */
int32_t af_renderer_info(void* handle, af_render_info* out) {
    af::Renderer* r = asRenderer(handle);
    if (!r || !out) return AF_ERR_INVALID_ARG;
    const af::RendererInfo info = r->info();
    std::snprintf(out->backend, sizeof(out->backend), "%s", info.backend.c_str());
    std::snprintf(out->device_name, sizeof(out->device_name), "%s", info.deviceName.c_str());
    std::snprintf(out->api_version, sizeof(out->api_version), "%s", info.apiVersion.c_str());
    std::snprintf(out->driver, sizeof(out->driver), "%s", info.driver.c_str());
    out->supports_shadows = info.supportsShadows ? 1 : 0;
    out->supports_msaa = info.supportsMsaa ? 1 : 0;
    out->supports_float_hdr = info.supportsFloatHdr ? 1 : 0;
    out->supports_anisotropy = info.supportsAnisotropy ? 1 : 0;
    return AF_OK;
}

int32_t af_renderer_stats(void* handle, af_render_stats* out) {
    af::Renderer* r = asRenderer(handle);
    if (!r || !out) return AF_ERR_INVALID_ARG;
    const af::RendererStats stats = r->stats();
    out->frame_ms = stats.frameMs;
    out->cpu_ms = stats.cpuMs;
    out->fps = stats.fps;
    out->draw_calls = stats.drawCalls;
    out->triangles = stats.triangles;
    out->vertices = stats.vertices;
    out->shadow_map_size = stats.shadowMapSize;
    out->msaa_samples = stats.msaaSamples;
    out->bloom_levels = stats.bloomLevels;
    out->quality_level = stats.qualityLevel;
    out->frames = stats.frames;
    out->texture_bytes = stats.textureBytes;
    return AF_OK;
}

/* ------------------------------------------------------------------ picking --------------- */
int32_t af_render_pick_region(const af_engine* engine, const float ray_origin[3],
                              const float ray_direction[3], float* out_distance, float* out_point) {
    if (!engine || !ray_origin || !ray_direction) return -1;
    return af::pickRegion(engine, ray_origin, ray_direction, out_distance, out_point);
}

void af_render_screen_ray(const af_render_camera* camera, float ndc_x, float ndc_y, int32_t width,
                          int32_t height, float out_origin[3], float out_direction[3]) {
    if (!camera || !out_origin || !out_direction) return;
    af::CameraState state{};
    state.yawDeg = camera->yaw_deg;
    state.pitchDeg = camera->pitch_deg;
    state.distance = camera->distance;
    state.targetY = camera->target_y;
    state.fovDeg = camera->fov_deg;
    state.rollDeg = camera->roll_deg;
    af::screenRay(state, ndc_x, ndc_y, width, height, out_origin, out_direction);
}

} /* extern "C" */
