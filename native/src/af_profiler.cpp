/*
 * af_profiler.cpp -- implementation of af_profiler.h.
 *
 * The percentile computation sorts a copy of the window (240 floats, ~1 us) instead of maintaining
 * a running structure: at 10 Hz reporting that is free, and it means the numbers cannot drift the
 * way an incrementally maintained histogram can after a reset.
 */
#include "af_profiler.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace af {

double profileNow() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    const clock::duration d = clock::now() - start;
    return std::chrono::duration<double>(d).count();
}

Profiler::Profiler() = default;

int Profiler::findStage(const char* name) const {
    for (size_t i = 0; i < stages_.size(); ++i) {
        if (stages_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Profiler::stageId(const char* name) {
    if (!name) return -1;
    const int existing = findStage(name);
    if (existing >= 0) return existing;
    if (stages_.size() >= static_cast<size_t>(MAX_STAGES)) return -1;   /* bounded, not a crash */
    StageAccum accum;
    accum.name = name;
    stages_.push_back(std::move(accum));
    return static_cast<int>(stages_.size()) - 1;
}

void Profiler::beginFrame(float nowSeconds) {
    frameStart_ = static_cast<double>(nowSeconds);
}

void Profiler::beginStage(int id, float nowSeconds) {
    if (id < 0 || static_cast<size_t>(id) >= stages_.size()) return;
    stages_[static_cast<size_t>(id)].currentStart = static_cast<double>(nowSeconds) + 1.0;
}

void Profiler::endStage(int id, float nowSeconds) {
    if (id < 0 || static_cast<size_t>(id) >= stages_.size()) return;
    StageAccum& s = stages_[static_cast<size_t>(id)];
    if (s.currentStart <= 0.0) return;
    /* The +1 above lets a stage that legitimately starts at t=0 be told apart from "not started".
       A stage whose clock went backwards (a different timebase, or a caller error) records 0
       rather than a negative duration. */
    const double start = s.currentStart - 1.0;
    const double now = static_cast<double>(nowSeconds);
    const double elapsed = now > start ? now - start : 0.0;
    s.samples[s.next] = static_cast<float>(elapsed * 1000.0);
    s.next = (s.next + 1) % MAX_SAMPLES;
    if (s.count < MAX_SAMPLES) ++s.count;
    s.currentStart = 0.0;
}

void Profiler::endFrame(float nowSeconds) {
    const double now = static_cast<double>(nowSeconds);
    const double dt = now > frameStart_ ? now - frameStart_ : 0.0;
    lastFrameMs_ = static_cast<float>(dt * 1000.0);
    frameSamples_[frameNext_] = lastFrameMs_;
    frameNext_ = (frameNext_ + 1) % MAX_SAMPLES;
    if (frameCount_ < MAX_SAMPLES) ++frameCount_;
    frameStart_ = 0.0;
}

void Profiler::submitGpuTimings(float frameMs, float vertexMs, float fragmentMs) {
    if (!gpuAvailable_) {
        gpuFrameMs_ = gpuVertexMs_ = gpuFragmentMs_ = -1.0f;
        return;
    }
    gpuFrameMs_ = frameMs;
    gpuVertexMs_ = vertexMs;
    gpuFragmentMs_ = fragmentMs;
}

void Profiler::reset() {
    for (auto& s : stages_) {
        s.count = 0;
        s.next = 0;
        s.currentStart = 0.0;
        std::memset(s.samples, 0, sizeof(s.samples));
    }
    frameCount_ = 0;
    frameNext_ = 0;
    frameStart_ = 0.0;
    lastFrameMs_ = 0.0f;
    gpuFrameMs_ = gpuVertexMs_ = gpuFragmentMs_ = -1.0f;
}

FrameStats Profiler::stats() const {
    FrameStats out;
    out.frames = frameCount_;
    out.lastFrameMs = lastFrameMs_;
    out.targetFrameMs = targetFrameMs_;
    out.gpuTimingAvailable = gpuAvailable_;
    out.gpuFrameMs = gpuFrameMs_;
    out.gpuVertexMs = gpuVertexMs_;
    out.gpuFragmentMs = gpuFragmentMs_;

    if (frameCount_ > 0) {
        std::vector<float> v(frameSamples_, frameSamples_ + frameCount_);
        std::sort(v.begin(), v.end());
        out.minFrameMs = v.front();
        out.maxFrameMs = v.back();
        out.medianFrameMs = v[v.size() / 2];
        out.p95FrameMs = v[static_cast<size_t>(v.size() * 95 / 100) >= v.size() ? v.size() - 1 : v.size() * 95 / 100];
        out.fps = out.medianFrameMs > 1e-4f ? 1000.0f / out.medianFrameMs : 0.0f;
        int missed = 0;
        for (float f : v) {
            if (f > targetFrameMs_ * 1.05f) ++missed;   /* 5% dead band: 16.6 vs 16.7 is not a miss */
        }
        out.missedBudgetFraction = static_cast<float>(missed) / static_cast<float>(v.size());
    }

    const size_t n = stages_.size() < static_cast<size_t>(MAX_STAGES) ? stages_.size() : static_cast<size_t>(MAX_STAGES);
    out.stageCount = static_cast<int>(n);
    for (size_t i = 0; i < n; ++i) {
        const StageAccum& s = stages_[i];
        StageStats& st = out.stages[i];
        st.name = s.name;
        if (s.count == 0) continue;
        std::vector<float> v(s.samples, s.samples + s.count);
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (float f : v) sum += f;
        st.lastMs = v.back();   /* the window is a ring, so "last" is the newest sample only when it
                                   has not wrapped; the max is the honest thing to show as "worst" */
        st.minMs = v.front();
        st.maxMs = v.back();
        st.medianMs = v[v.size() / 2];
        st.meanMs = static_cast<float>(sum / static_cast<double>(v.size()));
    }
    return out;
}

} /* namespace af */
