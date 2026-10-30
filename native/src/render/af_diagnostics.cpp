#include "af_diagnostics.h"

#include <cmath>
#include <limits>

namespace af {
namespace {
constexpr DiagnosticRungInfo kRungs[] = {
    {DiagnosticRung::CLEAR, "clear", "surface clears and presents with no geometry"},
    {DiagnosticRung::TRIANGLE, "triangle", "one non-indexed triangle is visible"},
    {DiagnosticRung::INDEXED_TRIANGLE, "indexed triangle", "one indexed triangle is visible"},
    {DiagnosticRung::TEXTURED_QUAD, "textured quad", "a sampled textured quad is visible"},
    {DiagnosticRung::LIT_SPHERE, "lit sphere", "a sphere responds to the lighting path"},
    {DiagnosticRung::SKINNED_MESH, "skinned mesh", "joint deformation changes the mesh"},
    {DiagnosticRung::BLENDSHAPE_MESH, "blendshape mesh", "a blendshape deformation is visible"},
    {DiagnosticRung::PRODUCTION_AVATAR, "production avatar", "the production avatar renders end-to-end"},
};
}

const DiagnosticRungInfo& diagnosticRungInfo(DiagnosticRung rung) {
    const int index = static_cast<int>(rung);
    static constexpr DiagnosticRungInfo invalid{
        DiagnosticRung::CLEAR, "invalid", "no diagnostic rung"};
    return index >= 0 && index < kDiagnosticRungCount ? kRungs[index] : invalid;
}

bool diagnosticRungValid(int rung) { return rung >= 0 && rung < kDiagnosticRungCount; }

bool gpuTimestampDeltaToMilliseconds(std::uint64_t startTicks, std::uint64_t endTicks,
                                     std::uint32_t validBits, double periodNs,
                                     float* outMilliseconds) {
    if (outMilliseconds) *outMilliseconds = -1.0f;
    if (!outMilliseconds || validBits == 0 || validBits > 64 ||
        !std::isfinite(periodNs) || periodNs <= 0.0) return false;

    std::uint64_t ticks = endTicks - startTicks;
    if (validBits < 64) ticks &= (std::uint64_t{1} << validBits) - 1;
    const double milliseconds = static_cast<double>(ticks) * periodNs / 1000000.0;
    if (!std::isfinite(milliseconds) || milliseconds < 0.0 ||
        milliseconds > static_cast<double>(std::numeric_limits<float>::max())) return false;
    *outMilliseconds = static_cast<float>(milliseconds);
    return std::isfinite(*outMilliseconds);
}
} // namespace af
