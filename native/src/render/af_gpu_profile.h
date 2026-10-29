/*
 * af_gpu_profile.h -- capability-first per-GPU tuning for the renderer and atlas encoder.
 *
 * Device-name rules provide a conservative starting tier; API-reported format, dimension, HDR and
 * sample-count support remain authoritative. Unknown devices therefore get a safe generic profile
 * rather than inheriting another vendor's assumptions.
 */
#ifndef AF_GPU_PROFILE_H
#define AF_GPU_PROFILE_H

#include <cstdint>
#include <string>

namespace af {

enum class GpuTier : uint8_t { LOW = 0, BALANCED = 1, HIGH = 2 };
enum class AtlasCompression : uint8_t { NONE = 0, ASTC_6X6 = 1, ETC2 = 2 };

struct GpuDeviceCaps {
    std::string backend;       /* "gles31" */
    std::string vendorName;
    std::string deviceName;
    std::string driver;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t maxTextureDimension2D = 0;
    uint32_t maxColorSamples = 1;
    bool supportsAstcLdr = false;
    bool supportsEtc2 = false;
    bool supportsFloatHdr = false;
    bool softwareRenderer = false;
};

struct GpuProfile {
    std::string name;
    GpuTier tier = GpuTier::LOW;
    AtlasCompression atlasCompression = AtlasCompression::NONE;
    uint32_t maxTextureDimension2D = 2048;
    uint32_t maxMsaaSamples = 1;
    int maxQualityLevel = 1;    /* renderer quality ladder: LOW=0 .. ULTRA=3 */
    int maxShadowMapSize = 1024;
    int maxBloomLevels = 2;
    float lodDistanceScale = 0.80f;
    float astcQuality = 0.0f;   /* ASTCENC_PRE_* effort; lower is faster */
    float etc2Effort = 0.0f;    /* Etc2Comp effort, range [0,100] */
    uint32_t encoderThreads = 1;
    float slowFrameSeconds = 0.0333f;
    float fastFrameSeconds = 0.0166f;
    uint32_t slowFramesBeforeDownshift = 30;
    uint32_t fastFramesBeforeUpshift = 240;
};

GpuProfile makeGpuProfile(const GpuDeviceCaps& caps);
const char* gpuTierName(GpuTier tier);
const char* atlasCompressionName(AtlasCompression compression);

} // namespace af
#endif /* AF_GPU_PROFILE_H */
