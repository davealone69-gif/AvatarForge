/*
 * ============================================================================================
 *  AvatarForge :: af_render.cpp  --  backend selection and the thermal/battery budget controller
 * ============================================================================================
 *
 *  Two responsibilities:
 *
 *   1. Renderer::create() initialises the sole shipped OpenGL ES 3.1 backend. If it cannot
 *      initialise, create() returns nullptr with the real GLES initialization error and the app
 *      keeps CPU-side features (generation, export) available. There is no software rasteriser
 *      pretending to be a renderer.
 *
 *   2. The quality controller. The app's hard requirement is 30 FPS minimum on a mid-range
 *      device with a 2 GB memory ceiling, on a phone that is also thermally throttling because
 *      the user just ran a 30-second geometry build. So the renderer measures its own frame time
 *      and steps a small quality ladder down when it cannot keep up, and back up (slowly) when
 *      there is headroom again. Only the three knobs that actually cost fill rate and bandwidth
 *      are touched: MSAA sample count, shadow map size and bloom level count. Exposure, colour
 *      and the model are never silently changed.
 */
#include "af_render.h"
#include "af_render_common.h"
#include "af_environment.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <android/log.h>

namespace af {

/* Only the OpenGL ES 3.1 renderer is built and shipped. */
Renderer* createGlRenderer(ANativeWindow* window, const RendererOptions& options, std::string* error);

namespace {

/* ------------------------------------------------------------------ quality ladder --------- */
struct QualityTier {
    int msaa;        /* clamped again by the backend against the device's sample counts */
    int shadowSize;
    int bloomLevels;
};

const QualityTier kTiers[4] = {
    /* LOW    */ {1, 1024, 2},
    /* MEDIUM */ {2, 1024, 3},
    /* HIGH   */ {4, 2048, 4},
    /* ULTRA  */ {4, 4096, 5},
};

int tierForPreset(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::LOW:    return 0;
        case QualityPreset::MEDIUM: return 1;
        case QualityPreset::HIGH:   return 2;
        /* AUTO starts in the middle and moves from there in either direction. */
        default:                    return 1;
    }
}

/*
 *  Wraps a backend and owns the quality decision. It deliberately does not own the surface or
 *  the engine: it forwards them, so a backend swap (rotation, settings change) does not disturb
 *  the app's frame loop.
 */
class BudgetRenderer final : public Renderer {
public:
    BudgetRenderer(Renderer* backend, const RendererOptions& requested)
        : backend_(backend), requested_(requested), profile_(backend->gpuProfile()),
          tier_(std::min(tierForPreset(requested.quality), profile_.maxQualityLevel)) {
        applyTier();
    }

    ~BudgetRenderer() override { delete backend_; }

    bool resize(int w, int h) override { return backend_->resize(w, h); }
    void attachEngine(af_engine* e) override { backend_->attachEngine(e); }
    void invalidateMesh() override { backend_->invalidateMesh(); }
    void setCamera(const CameraState& c) override { backend_->setCamera(c); }
    void setSkeletonVisible(bool v) override { backend_->setSkeletonVisible(v); }
    void setFlatShading(bool f) override { backend_->setFlatShading(f); }
    void setHighlightRegion(int r) override { backend_->setHighlightRegion(r); }
    void setClip(int clip, float speed, bool loop) override { backend_->setClip(clip, speed, loop); }
    int currentClip() const override { return backend_->currentClip(); }
    float clipTime() const override { return backend_->clipTime(); }
    void waitIdle() override { backend_->waitIdle(); }
    RendererInfo info() const override { return backend_->info(); }

    RendererOptions options() const override { return requested_; }
    GpuProfile gpuProfile() const override { return profile_; }

    void setOptions(const RendererOptions& options) override {
        const bool presetChanged = options.quality != requested_.quality;
        requested_ = options;
        if (presetChanged)
            tier_ = std::min(tierForPreset(options.quality), profile_.maxQualityLevel);
        applyTier();
    }

    void renderFrame(float deltaSeconds) override {
        const auto callStart = std::chrono::steady_clock::now();
        backend_->renderFrame(deltaSeconds);
        const auto callEnd = std::chrono::steady_clock::now();
        stats_ = backend_->stats();
        /* CPU-side wall time for the renderer call. This can include driver/GPU fence waits; it is
           deliberately not presented as a GPU timestamp. Frame cadence remains the adaptive signal. */
        stats_.cpuMs = std::chrono::duration<float, std::milli>(callEnd - callStart).count();
        stats_.qualityLevel = tier_;
        updateBudget(deltaSeconds);
    }

    RendererStats stats() const override { return stats_; }

private:
    /* Sends the current tier's three fields into the backend, leaving every other setting alone. */
    void applyTier() {
        RendererOptions effective = requested_;
        effective.effectiveQualityLevel = std::min(tier_, profile_.maxQualityLevel);
        effective.msaaSamples = std::min(kTiers[tier_].msaa, static_cast<int>(profile_.maxMsaaSamples));
        effective.shadowMapSize = std::min(kTiers[tier_].shadowSize, profile_.maxShadowMapSize);
        effective.bloomLevels = std::min(kTiers[tier_].bloomLevels, profile_.maxBloomLevels);
        backend_->setOptions(effective);
    }

    /*
     *  Two asymmetric windows: step down after ~0.6 s of missed frames (a user notices a stutter
     *  long before that), step up only after ~4 s of solid headroom (so a scene that was briefly
     *  cheap does not ratchet the quality up and down every second).
     */
    void updateBudget(float deltaSeconds) {
        if (deltaSeconds <= 0.0f || deltaSeconds > 0.5f) return;   /* ignore paused/backgrounded frames */
        if (stats_.frames < 45) return;                            /* pipeline warm-up              */

        smoothed_ = smoothed_ <= 0.0f ? deltaSeconds : smoothed_ * 0.9f + deltaSeconds * 0.1f;

        if (smoothed_ > profile_.slowFrameSeconds) {
            if (++slowFrames_ > static_cast<int>(profile_.slowFramesBeforeDownshift) && tier_ > 0) {
                --tier_;
                applyTier();
                slowFrames_ = 0;
                fastFrames_ = 0;
            }
        } else {
            slowFrames_ = 0;
        }
        if (smoothed_ < profile_.fastFrameSeconds && requested_.quality == QualityPreset::AUTO) {
            if (++fastFrames_ > static_cast<int>(profile_.fastFramesBeforeUpshift) &&
                tier_ < profile_.maxQualityLevel) {
                ++tier_;
                applyTier();
                fastFrames_ = 0;
            }
        } else if (smoothed_ >= 0.0166f) {
            fastFrames_ = 0;
        }
    }

    Renderer* backend_ = nullptr;
    RendererOptions requested_{};
    GpuProfile profile_{};
    RendererStats stats_{};
    int tier_ = 1;
    int slowFrames_ = 0;
    int fastFrames_ = 0;
    float smoothed_ = 0.0f;
};

}  /* namespace */

Renderer* Renderer::create(ANativeWindow* window, const RendererOptions& options, std::string* error) {
    if (!window) {
        if (error) *error = "no native window (the SurfaceView is not ready yet)";
        return nullptr;
    }

    std::string glError;
    Renderer* backend = createGlRenderer(window, options, &glError);
    if (backend) {
        __android_log_print(ANDROID_LOG_INFO, "AvatarForgeRender",
                            "OpenGL ES 3.1 renderer initialized successfully");
        return new BudgetRenderer(backend, options);
    }

    if (error) {
        *error = "OpenGL ES 3.1 renderer unavailable: ";
        *error += glError.empty() ? std::string("unknown initialization failure") : glError;
    }
    return nullptr;
}

} /* namespace af */
