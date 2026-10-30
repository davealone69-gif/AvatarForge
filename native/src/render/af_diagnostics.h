/* Diagnostic ladder and honest GPU telemetry contracts. */
#ifndef AF_DIAGNOSTICS_H
#define AF_DIAGNOSTICS_H

#include <cstdint>

namespace af {

enum class DiagnosticRung : std::uint8_t {
    CLEAR = 0, TRIANGLE, INDEXED_TRIANGLE, TEXTURED_QUAD, LIT_SPHERE,
    SKINNED_MESH, BLENDSHAPE_MESH, PRODUCTION_AVATAR, COUNT
};

struct DiagnosticRungInfo {
    DiagnosticRung rung;
    const char* name;
    const char* passCriterion;
};

constexpr int kDiagnosticRungCount = static_cast<int>(DiagnosticRung::COUNT);
const DiagnosticRungInfo& diagnosticRungInfo(DiagnosticRung rung);
bool diagnosticRungValid(int rung);
bool gpuTimestampDeltaToMilliseconds(std::uint64_t startTicks, std::uint64_t endTicks,
                                     std::uint32_t validBits, double periodNs,
                                     float* outMilliseconds);

/* Capability is explicit: a negative timing value remains unknown, never zero. */
struct GpuTimingCapabilities {
    bool supported = false;
    bool disjoint = false;
    bool pending = false;
};

struct GpuTimingResult {
    GpuTimingCapabilities capability;
    float frameMs = -1.0f;
    std::uint64_t drawCalls = 0;
    std::uint64_t triangles = 0;
};

} // namespace af
#endif
