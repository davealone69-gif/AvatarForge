/*
 * af_gpu_profile.cpp -- conservative vendor/model hints gated by measured API capabilities.
 */
#include "af_gpu_profile.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace af {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

int digitsAfter(const std::string& text, const std::string& token) {
    const size_t at = text.find(token);
    if (at == std::string::npos) return -1;
    size_t pos = at + token.size();
    while (pos < text.size() && !std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos == text.size()) return -1;
    int value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        value = std::min(100000, value * 10 + (text[pos] - '0'));
        ++pos;
    }
    return value;
}

GpuTier capabilityTier(const GpuDeviceCaps& caps) {
    const uint32_t dimension = caps.maxTextureDimension2D ? caps.maxTextureDimension2D : 4096u;
    const uint32_t samples = std::max(1u, caps.maxColorSamples);
    if (dimension >= 16384u && samples >= 4u && caps.supportsFloatHdr) return GpuTier::HIGH;
    if (dimension >= 8192u && samples >= 2u) return GpuTier::BALANCED;
    return GpuTier::LOW;
}

uint32_t floorSampleCount(uint32_t reported, uint32_t wanted) {
    const uint32_t cap = std::min(std::max(1u, reported), wanted);
    for (uint32_t candidate : {8u, 4u, 2u, 1u}) {
        if (candidate <= cap) return candidate;
    }
    return 1u;
}

uint32_t floorPowerOfTwo(uint32_t value) {
    uint32_t result = 1;
    while (result <= value / 2u && result < 0x80000000u) result <<= 1u;
    return result;
}

} // namespace

GpuProfile makeGpuProfile(const GpuDeviceCaps& caps) {
    const std::string device = lower(caps.deviceName);
    const std::string vendor = lower(caps.vendorName);
    const std::string combined = vendor + " " + device + " " + lower(caps.driver);
    std::string family = "generic";
    GpuTier tier = capabilityTier(caps);
    bool modelClassified = false;

    if (caps.softwareRenderer || combined.find("swiftshader") != std::string::npos ||
        combined.find("llvmpipe") != std::string::npos ||
        combined.find("software raster") != std::string::npos) {
        family = "software";
        tier = GpuTier::LOW;
        modelClassified = true;
    } else if (caps.vendorId == 0x5143u || combined.find("qualcomm") != std::string::npos ||
               combined.find("adreno") != std::string::npos) {
        family = "Adreno";
        const int model = digitsAfter(device, "adreno");
        if (model >= 0) {
            tier = model >= 700 ? GpuTier::HIGH : model >= 600 ? GpuTier::BALANCED : GpuTier::LOW;
            modelClassified = true;
        }
    } else if (caps.vendorId == 0x13B5u || combined.find("arm") != std::string::npos ||
               combined.find("mali") != std::string::npos ||
               combined.find("immortalis") != std::string::npos) {
        family = combined.find("immortalis") != std::string::npos ? "Immortalis" : "Mali";
        int model = digitsAfter(device, "immortalis-g");
        if (model < 0) model = digitsAfter(device, "mali-g");
        if (model >= 0) {
            tier = (model >= 700 || (model >= 70 && model < 100)) ? GpuTier::HIGH
                 : (model >= 55 || (model >= 500 && model < 700)) ? GpuTier::BALANCED
                 : GpuTier::LOW;
            modelClassified = true;
        } else {
            const int legacy = digitsAfter(device, "mali-t");
            if (legacy >= 0) {
                tier = legacy >= 880 ? GpuTier::BALANCED : GpuTier::LOW;
                modelClassified = true;
            }
        }
    } else if (caps.vendorId == 0x1010u || combined.find("imagination") != std::string::npos ||
               combined.find("powervr") != std::string::npos) {
        family = "PowerVR";
    } else if (caps.vendorId == 0x10DEu || combined.find("nvidia") != std::string::npos) {
        family = "NVIDIA";
    } else if (caps.vendorId == 0x8086u || combined.find("intel") != std::string::npos) {
        family = "Intel";
    }

    /* Model hints are not allowed to override a hard capability limit. */
    const uint32_t maxTexture = caps.maxTextureDimension2D ? caps.maxTextureDimension2D : 2048u;
    const uint32_t maxSamples = std::max(1u, caps.maxColorSamples);
    if (maxTexture < 4096u || maxSamples < 2u) {
        tier = GpuTier::LOW;
    } else if (maxTexture < 8192u && tier == GpuTier::HIGH) {
        tier = GpuTier::BALANCED;
    }
    if (!caps.supportsFloatHdr && tier == GpuTier::HIGH) tier = GpuTier::BALANCED;
    if (!modelClassified && family == "generic") tier = capabilityTier(caps);

    GpuProfile profile;
    profile.tier = tier;
    profile.maxTextureDimension2D = maxTexture;
    profile.maxQualityLevel = tier == GpuTier::HIGH ? 3 : tier == GpuTier::BALANCED ? 2 : 1;
    profile.maxShadowMapSize = tier == GpuTier::HIGH ? 4096 : tier == GpuTier::BALANCED ? 2048 : 1024;
    profile.maxShadowMapSize = static_cast<int>(std::min<uint32_t>(
        static_cast<uint32_t>(profile.maxShadowMapSize), floorPowerOfTwo(maxTexture)));
    profile.maxShadowMapSize = std::max(512, profile.maxShadowMapSize);
    profile.maxBloomLevels = tier == GpuTier::HIGH ? 5 : tier == GpuTier::BALANCED ? 4 : 2;
    profile.lodDistanceScale = tier == GpuTier::HIGH ? 1.12f : tier == GpuTier::BALANCED ? 1.0f : 0.82f;
    profile.maxMsaaSamples = floorSampleCount(maxSamples, tier == GpuTier::HIGH ? 4u
                                                    : tier == GpuTier::BALANCED ? 2u : 1u);
    profile.astcQuality = tier == GpuTier::HIGH ? 30.0f : tier == GpuTier::BALANCED ? 10.0f : 0.0f;
    profile.etc2Effort = tier == GpuTier::HIGH ? 40.0f : tier == GpuTier::BALANCED ? 20.0f : 0.0f;
    profile.encoderThreads = tier == GpuTier::HIGH ? 3u : tier == GpuTier::BALANCED ? 2u : 1u;

    if (caps.supportsAstcLdr) profile.atlasCompression = AtlasCompression::ASTC_6X6;
    else if (caps.supportsEtc2) profile.atlasCompression = AtlasCompression::ETC2;
    else profile.atlasCompression = AtlasCompression::NONE;

    profile.name = family + " " + gpuTierName(tier);
    return profile;
}

const char* gpuTierName(GpuTier tier) {
    switch (tier) {
        case GpuTier::LOW: return "low";
        case GpuTier::BALANCED: return "balanced";
        case GpuTier::HIGH: return "high";
        default: return "low";
    }
}

const char* atlasCompressionName(AtlasCompression compression) {
    switch (compression) {
        case AtlasCompression::ASTC_6X6: return "ASTC 6x6";
        case AtlasCompression::ETC2: return "ETC2/EAC";
        case AtlasCompression::NONE: return "RGBA8/R8";
        default: return "RGBA8/R8";
    }
}

} // namespace af
