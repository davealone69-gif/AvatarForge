#include "../../src/render/af_diagnostics.h"
#include <cassert>
#include <cstring>
#include <limits>

int main() {
    for (int i = 0; i < af::kDiagnosticRungCount; ++i) {
        assert(af::diagnosticRungValid(i));
        const auto& info = af::diagnosticRungInfo(static_cast<af::DiagnosticRung>(i));
        assert(info.name && std::strlen(info.name) > 0);
        assert(info.passCriterion && std::strlen(info.passCriterion) > 0);
    }
    assert(!af::diagnosticRungValid(-1));
    assert(!af::diagnosticRungValid(af::kDiagnosticRungCount));
    assert(std::strcmp(af::diagnosticRungInfo(static_cast<af::DiagnosticRung>(99)).name,
                       "invalid") == 0);
    const af::GpuTimingResult unknown{};
    assert(!unknown.capability.supported && unknown.frameMs == -1.0f);
    float milliseconds = 99.0f;
    assert(af::gpuTimestampDeltaToMilliseconds(0xfffffffeu, 1u, 32, 1000000.0, &milliseconds));
    assert(milliseconds == 3.0f);
    assert(af::gpuTimestampDeltaToMilliseconds(100u, 125u, 64, 1000.0, &milliseconds));
    assert(milliseconds == 0.025f);
    assert(!af::gpuTimestampDeltaToMilliseconds(0u, 1u, 0, 1000000.0, &milliseconds));
    assert(milliseconds == -1.0f);
    assert(!af::gpuTimestampDeltaToMilliseconds(0u, 1u, 64,
                                                std::numeric_limits<double>::quiet_NaN(),
                                                &milliseconds));
    assert(milliseconds == -1.0f);
    return 0;
}
