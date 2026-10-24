#pragma once
#include "../src/render/af_gpu_profile.h"
#include <algorithm>
#include <cstdlib>

namespace af {
// Mesa's real software profile intentionally disables MSAA and most bloom. Run once unchanged,
// then lift only those policy limits to cover the driver's actual MSAA / multi-level post passes.
// No API capabilities, shaders, draw commands or GPU results are mocked.
static GpuProfile rendererTestProfile(const GpuDeviceCaps& caps) {
    GpuProfile profile = makeGpuProfile(caps);
    if (std::getenv("AF_RENDER_TEST_FULL_PROFILE")) {
        profile.name = "test MSAA/bloom coverage (software-driver policy override)";
        profile.maxMsaaSamples = std::min(4u, caps.maxColorSamples);
        profile.maxBloomLevels = 5;
        profile.maxQualityLevel = 3;
    }
    return profile;
}
} // namespace af
