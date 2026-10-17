/*
 * af_profiler.h -- frame profiling that works with a timer you have and admits when it does not.
 *
 * The engine already reports per-stage build times (af_stats) because a 30-90 s generation is slow
 * enough to measure at the second. Rendering is the opposite problem: it is 16 ms per frame and
 * the interesting question is which 2 ms are wasted. This is the per-frame instrument.
 *
 * Two levels, deliberately separated:
 *
 *   CPU   always available. `beginFrame()` ... `beginStage(name)` / `endStage()` ... `endFrame()`
 *         accumulate per-stage CPU milliseconds over a ring of frames, with min/median/max and a
 *         histogram suitable for a frame-time graph in the UI.
 *
 *   GPU   only when the backend exposes timestamp queries, which is a runtime capability:
 *         Vulkan with a timestampValidBits > 0 queue (and the device not in a debugger), or GLES
 *         with EXT_disjoint_timer_query. The renderer probes for it at init and reports
 *         `gpuTimingAvailable()`. When it is false, every GPU number reads as -1 and the UI must
 *         say "not available on this device" rather than showing a plausible-looking zero. That
 *         distinction is the whole point: a fake 0.0 ms would be a lie of exactly the kind this
 *         project forbids.
 *
 * The profiler allocates nothing per frame after warm-up: stage names are interned to small
 * integer ids at registration, and the ring buffer is fixed-size.
 */
#ifndef AF_PROFILER_H
#define AF_PROFILER_H

#include <cstdint>
#include <string>
#include <vector>

namespace af {

constexpr int MAX_STAGES = 16;
constexpr int MAX_SAMPLES = 240;   /* four seconds at 60 fps - enough for a graph and a median */

/** One stage's accumulated statistics over the sample window. */
struct StageStats {
    std::string name;
    float lastMs = 0.0f;
    float minMs = 0.0f;
    float maxMs = 0.0f;
    float medianMs = 0.0f;
    float meanMs = 0.0f;
};

/** Frame-level statistics. `fps` is derived from the median frame time, not the mean, because one
 *  long frame (a shader compile, a texture upload) would otherwise dominate a mean. */
struct FrameStats {
    int frames = 0;              /* samples currently in the window */
    float lastFrameMs = 0.0f;
    float minFrameMs = 0.0f;
    float maxFrameMs = 0.0f;
    float medianFrameMs = 0.0f;
    float p95FrameMs = 0.0f;
    float fps = 0.0f;
    /* Fraction of frames in the window that missed the target budget. The app's quality
       controller watches this, not the instantaneous frame time. */
    float missedBudgetFraction = 0.0f;
    float targetFrameMs = 1000.0f / 60.0f;
    int stageCount = 0;
    StageStats stages[MAX_STAGES];
    /* GPU timings in milliseconds, or -1 for every entry when the device cannot measure them. */
    bool gpuTimingAvailable = false;
    float gpuFrameMs = -1.0f;
    float gpuVertexMs = -1.0f;
    float gpuFragmentMs = -1.0f;
};

class Profiler {
public:
    Profiler();

    /* Interns a stage name. Returns an id used by beginStage(). Calling twice with the same name
       returns the same id, so callers do not need to cache it, though caching is cheaper. */
    int stageId(const char* name);

    /* Every call takes the timestamp of the event it marks. That is deliberate: a profiler that
       reads the clock itself cannot be tested with a synthetic clock, and the render loop already
       has the frame's time to hand. Mixing the two sources inside this class is what produced a
       window of zero-length stages the first time it was written. */
    void beginFrame(float nowSeconds);
    void beginStage(int id, float nowSeconds);
    void endStage(int id, float nowSeconds);
    void endFrame(float nowSeconds);

    /* GPU timing is filled by the backend, not measured here: the backend knows how to query its
       own API, and this class only stores what it is told. Passing a negative value marks the
       whole channel unavailable. */
    void setGpuTimingAvailable(bool available) { gpuAvailable_ = available; }
    bool gpuTimingAvailable() const { return gpuAvailable_; }
    void submitGpuTimings(float frameMs, float vertexMs, float fragmentMs);

    /* Statistics over the current window. Cheap enough to call every frame from the UI at 10 Hz. */
    FrameStats stats() const;

    void setTargetFrameMs(float ms) { targetFrameMs_ = ms; }
    void reset();

private:
    struct StageAccum {
        std::string name;
        double currentStart = 0.0;
        float samples[MAX_SAMPLES] = {0.0f};
        int count = 0;
        int next = 0;
    };

    int findStage(const char* name) const;

    std::vector<StageAccum> stages_;
    float frameSamples_[MAX_SAMPLES] = {0.0f};
    int frameCount_ = 0;
    int frameNext_ = 0;
    double frameStart_ = 0.0;
    float lastFrameMs_ = 0.0f;
    float targetFrameMs_ = 1000.0f / 60.0f;
    bool gpuAvailable_ = false;
    float gpuFrameMs_ = -1.0f;
    float gpuVertexMs_ = -1.0f;
    float gpuFragmentMs_ = -1.0f;
};

/** Monotonic seconds, the clock every timing in the engine uses. */
double profileNow();

} /* namespace af */
#endif /* AF_PROFILER_H */
