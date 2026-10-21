/*
 * af_texture_compression.cpp -- ASTC LDR and ETC2/EAC atlas encoders plus semantic mip filtering.
 */
#include "af_texture_compression.h"

#include "astcenc.h"
#include "Etc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace af {
namespace {

constexpr int kMaximumMipLevels = 12;
constexpr int kAstcBlock = 6;
constexpr int kEtcBlock = 4;

struct RawMip {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> bytes;
};

uint64_t pixelBytes(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) return 0;
    const uint64_t w = static_cast<uint64_t>(width);
    const uint64_t h = static_cast<uint64_t>(height);
    const uint64_t c = static_cast<uint64_t>(channels);
    if (w > std::numeric_limits<uint64_t>::max() / h / c) return 0;
    return w * h * c;
}

uint64_t blockPayloadBytes(int width, int height, int blockWidth, int blockHeight,
                           int bytesPerBlock) {
    if (width <= 0 || height <= 0 || blockWidth <= 0 || blockHeight <= 0 || bytesPerBlock <= 0)
        return 0;
    const uint64_t columns = (static_cast<uint64_t>(width) + blockWidth - 1u) / blockWidth;
    const uint64_t rows = (static_cast<uint64_t>(height) + blockHeight - 1u) / blockHeight;
    if (columns > std::numeric_limits<uint64_t>::max() / rows / static_cast<uint64_t>(bytesPerBlock))
        return 0;
    return columns * rows * static_cast<uint64_t>(bytesPerBlock);
}

float srgbToLinear(uint8_t value) {
    const float c = static_cast<float>(value) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t linearToSrgbByte(float value) {
    const float c = std::max(0.0f, std::min(1.0f, value));
    const float encoded = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, encoded * 255.0f + 0.5f)));
}

float areaOverlap(float a0, float a1, int cell) {
    return std::max(0.0f, std::min(a1, static_cast<float>(cell + 1)) -
                           std::max(a0, static_cast<float>(cell)));
}

bool downsample(const RawMip& src, int slot, RawMip* dst) {
    if (!dst || src.width <= 0 || src.height <= 0 || (src.channels != 1 && src.channels != 4)) return false;
    dst->width = std::max(1, src.width / 2);
    dst->height = std::max(1, src.height / 2);
    dst->channels = src.channels;
    const uint64_t size = pixelBytes(dst->width, dst->height, dst->channels);
    if (!size || size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return false;
    dst->bytes.assign(static_cast<size_t>(size), 0);

    const bool srgbColor = slot == AF_TEX_BASE_COLOR && src.channels == 4;
    const bool normalMap = slot == AF_TEX_NORMAL && src.channels == 4;
    for (int y = 0; y < dst->height; ++y) {
        const float y0 = static_cast<float>(y) * src.height / dst->height;
        const float y1 = static_cast<float>(y + 1) * src.height / dst->height;
        const int sy0 = std::max(0, static_cast<int>(std::floor(y0)));
        const int sy1 = std::min(src.height, static_cast<int>(std::ceil(y1)));
        for (int x = 0; x < dst->width; ++x) {
            const float x0 = static_cast<float>(x) * src.width / dst->width;
            const float x1 = static_cast<float>(x + 1) * src.width / dst->width;
            const int sx0 = std::max(0, static_cast<int>(std::floor(x0)));
            const int sx1 = std::min(src.width, static_cast<int>(std::ceil(x1)));
            double sums[4] = {0.0, 0.0, 0.0, 0.0};
            double totalWeight = 0.0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const float wy = areaOverlap(y0, y1, sy);
                for (int sx = sx0; sx < sx1; ++sx) {
                    const float weight = wy * areaOverlap(x0, x1, sx);
                    if (weight <= 0.0f) continue;
                    const size_t at = (static_cast<size_t>(sy) * src.width + sx) * src.channels;
                    if (normalMap) {
                        sums[0] += (static_cast<double>(src.bytes[at]) / 127.5 - 1.0) * weight;
                        sums[1] += (static_cast<double>(src.bytes[at + 1]) / 127.5 - 1.0) * weight;
                        sums[2] += (static_cast<double>(src.bytes[at + 2]) / 127.5 - 1.0) * weight;
                        sums[3] += (static_cast<double>(src.bytes[at + 3]) / 255.0) * weight;
                    } else {
                        for (int c = 0; c < src.channels; ++c) {
                            const float value = srgbColor && c < 3 ? srgbToLinear(src.bytes[at + c])
                                                                    : static_cast<float>(src.bytes[at + c]) / 255.0f;
                            sums[c] += static_cast<double>(value) * weight;
                        }
                    }
                    totalWeight += weight;
                }
            }
            if (totalWeight <= 0.0) return false;
            const size_t outAt = (static_cast<size_t>(y) * dst->width + x) * dst->channels;
            if (normalMap) {
                float nx = static_cast<float>(sums[0] / totalWeight);
                float ny = static_cast<float>(sums[1] / totalWeight);
                float nz = static_cast<float>(sums[2] / totalWeight);
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 1e-6f) { nx /= length; ny /= length; nz /= length; }
                else { nx = 0.0f; ny = 0.0f; nz = 1.0f; }
                dst->bytes[outAt] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (nx * 0.5f + 0.5f) * 255.0f + 0.5f)));
                dst->bytes[outAt + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (ny * 0.5f + 0.5f) * 255.0f + 0.5f)));
                dst->bytes[outAt + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, (nz * 0.5f + 0.5f) * 255.0f + 0.5f)));
                dst->bytes[outAt + 3] = static_cast<uint8_t>(std::max(0.0, std::min(255.0, sums[3] / totalWeight * 255.0 + 0.5)));
            } else {
                for (int c = 0; c < dst->channels; ++c) {
                    const float average = static_cast<float>(sums[c] / totalWeight);
                    dst->bytes[outAt + c] = srgbColor && c < 3 ? linearToSrgbByte(average)
                        : static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, average * 255.0f + 0.5f)));
                }
            }
        }
    }
    return true;
}

bool buildMipChain(const af_texture& source, int slot, std::vector<RawMip>* mips,
                   std::string* error) {
    if (!mips) return false;
    mips->clear();
    if (!source.data || source.width <= 0 || source.height <= 0 ||
        (source.channels != 1 && source.channels != 4)) {
        if (error) *error = "invalid atlas texture descriptor";
        return false;
    }
    if (source.width > 65535 || source.height > 65535) {
        if (error) *error = "atlas dimensions exceed encoder limits";
        return false;
    }
    const uint64_t baseBytes = pixelBytes(source.width, source.height, source.channels);
    if (!baseBytes || source.byte_size < baseBytes || baseBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        if (error) *error = "atlas byte size is smaller than its base level";
        return false;
    }

    RawMip base;
    base.width = source.width;
    base.height = source.height;
    base.channels = source.channels;
    base.bytes.assign(source.data, source.data + static_cast<size_t>(baseBytes));
    mips->push_back(std::move(base));
    while ((mips->back().width > 1 || mips->back().height > 1) &&
           static_cast<int>(mips->size()) < kMaximumMipLevels) {
        RawMip next;
        if (!downsample(mips->back(), slot, &next)) {
            if (error) *error = "failed to build semantic atlas mip level";
            mips->clear();
            return false;
        }
        mips->push_back(std::move(next));
    }
    return true;
}

std::vector<uint8_t> expandToRgba(const RawMip& mip) {
    const uint64_t count = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height);
    std::vector<uint8_t> rgba(static_cast<size_t>(count * 4u));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        if (mip.channels == 1) {
            rgba[i * 4] = mip.bytes[i];
            rgba[i * 4 + 1] = 0;
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        } else {
            std::copy_n(mip.bytes.data() + i * 4, 4, rgba.data() + i * 4);
        }
    }
    return rgba;
}

AtlasTextureEncoding astcEncodingForSlot(int slot) {
    return slot == AF_TEX_BASE_COLOR ? AtlasTextureEncoding::ASTC_6X6_SRGB
                                     : AtlasTextureEncoding::ASTC_6X6_UNORM;
}

AtlasTextureEncoding etc2EncodingForSlot(int slot) {
    if (slot == AF_TEX_BASE_COLOR) return AtlasTextureEncoding::ETC2_RGBA8_SRGB;
    if (slot == AF_TEX_OCCLUSION) return AtlasTextureEncoding::ETC2_R11_UNORM;
    return AtlasTextureEncoding::ETC2_RGBA8_UNORM;
}

bool encodeAstc(const std::vector<RawMip>& sourceMips, int slot, const GpuProfile& profile,
                EncodedTexture* out, std::string* error) {
    const astcenc_profile colorProfile = slot == AF_TEX_BASE_COLOR ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
    astcenc_config config{};
    const float quality = std::max(0.0f, std::min(100.0f, profile.astcQuality));
    const astcenc_error cfgError = astcenc_config_init(colorProfile, kAstcBlock, kAstcBlock, 1,
                                                       quality, ASTCENC_FLG_USE_DECODE_UNORM8, &config);
    if (cfgError != ASTCENC_SUCCESS) {
        if (error) *error = std::string("ASTC config: ") + astcenc_get_error_string(cfgError);
        return false;
    }
    astcenc_context* context = nullptr;
    const astcenc_error allocError = astcenc_context_alloc(&config, 1, &context, nullptr);
    if (allocError != ASTCENC_SUCCESS || !context) {
        if (error) *error = std::string("ASTC context: ") + astcenc_get_error_string(allocError);
        return false;
    }

    out->encoding = astcEncodingForSlot(slot);
    out->width = sourceMips.front().width;
    out->height = sourceMips.front().height;
    out->levels.clear();
    bool ok = true;
    for (const RawMip& mip : sourceMips) {
        const uint64_t payloadBytes = blockPayloadBytes(mip.width, mip.height, kAstcBlock, kAstcBlock, 16);
        if (!payloadBytes || payloadBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            if (error) *error = "ASTC output size overflow";
            ok = false;
            break;
        }
        std::vector<uint8_t> rgba = expandToRgba(mip);
        void* imageSlices[1] = {rgba.data()};
        astcenc_image image{};
        image.dim_x = static_cast<unsigned int>(mip.width);
        image.dim_y = static_cast<unsigned int>(mip.height);
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;
        image.data = imageSlices;
        const astcenc_swizzle swizzle = mip.channels == 1
            ? astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_1}
            : astcenc_swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        EncodedMip encoded;
        encoded.width = mip.width;
        encoded.height = mip.height;
        encoded.bytes.resize(static_cast<size_t>(payloadBytes));
        const astcenc_error encodeError = astcenc_compress_image(context, &image, &swizzle,
                                                                  encoded.bytes.data(),
                                                                  encoded.bytes.size(), 0);
        if (encodeError != ASTCENC_SUCCESS) {
            if (error) *error = std::string("ASTC encode: ") + astcenc_get_error_string(encodeError);
            ok = false;
            break;
        }
        out->levels.push_back(std::move(encoded));
    }
    astcenc_context_free(context);
    if (!ok) *out = EncodedTexture{};
    return ok;
}

bool encodeEtc2(const std::vector<RawMip>& sourceMips, int slot, const GpuProfile& profile,
                EncodedTexture* out, std::string* error) {
    if (sourceMips.empty()) return false;
    const Etc::Image::Format etcFormat = slot == AF_TEX_BASE_COLOR ? Etc::Image::Format::SRGBA8
        : slot == AF_TEX_OCCLUSION ? Etc::Image::Format::R11 : Etc::Image::Format::RGBA8;
    const Etc::ErrorMetric errorMetric = slot == AF_TEX_BASE_COLOR ? Etc::REC709
        : slot == AF_TEX_NORMAL ? Etc::NORMALXYZ : Etc::NUMERIC;
    const int bytesPerBlock = slot == AF_TEX_OCCLUSION ? 8 : 16;
    const uint32_t jobs = std::max(1u, std::min(8u, profile.encoderThreads));

    out->encoding = etc2EncodingForSlot(slot);
    out->width = sourceMips.front().width;
    out->height = sourceMips.front().height;
    out->levels.clear();
    for (const RawMip& mip : sourceMips) {
        const uint64_t expectedBytes = blockPayloadBytes(mip.width, mip.height, kEtcBlock, kEtcBlock,
                                                         bytesPerBlock);
        if (!expectedBytes || expectedBytes > std::numeric_limits<unsigned int>::max()) {
            if (error) *error = "ETC2 output size overflow";
            *out = EncodedTexture{};
            return false;
        }
        const uint64_t pixels = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height);
        if (pixels > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) / 4u) {
            if (error) *error = "ETC2 input size overflow";
            *out = EncodedTexture{};
            return false;
        }
        std::vector<float> rgba(static_cast<size_t>(pixels * 4u));
        for (size_t i = 0; i < static_cast<size_t>(pixels); ++i) {
            if (slot == AF_TEX_OCCLUSION) {
                rgba[i * 4] = static_cast<float>(mip.bytes[i]) / 255.0f;
                rgba[i * 4 + 1] = 0.0f;
                rgba[i * 4 + 2] = 0.0f;
                rgba[i * 4 + 3] = 1.0f;
            } else {
                for (int channel = 0; channel < 4; ++channel)
                    rgba[i * 4 + channel] = static_cast<float>(mip.bytes[i * 4 + channel]) / 255.0f;
            }
        }

        unsigned char* encodedBits = nullptr;
        unsigned int actualBytes = 0;
        unsigned int extendedWidth = 0, extendedHeight = 0;
        int encodingTime = 0;
        Etc::Encode(rgba.data(), static_cast<unsigned int>(mip.width), static_cast<unsigned int>(mip.height),
                    etcFormat, errorMetric, std::max(0.0f, std::min(100.0f, profile.etc2Effort)),
                    jobs, jobs, &encodedBits, &actualBytes, &extendedWidth, &extendedHeight,
                    &encodingTime, false);
        std::unique_ptr<unsigned char[]> owner(encodedBits);
        const uint64_t expectedExtendedWidth = ((static_cast<uint64_t>(mip.width) + 3u) / 4u) * 4u;
        const uint64_t expectedExtendedHeight = ((static_cast<uint64_t>(mip.height) + 3u) / 4u) * 4u;
        if (!owner || actualBytes != expectedBytes || extendedWidth != expectedExtendedWidth ||
            extendedHeight != expectedExtendedHeight) {
            if (error) *error = "ETC2 encoder returned an unexpected block payload";
            *out = EncodedTexture{};
            return false;
        }
        EncodedMip encoded;
        encoded.width = mip.width;
        encoded.height = mip.height;
        encoded.bytes.assign(owner.get(), owner.get() + actualBytes);
        out->levels.push_back(std::move(encoded));
    }
    return out->valid();
}

} // namespace

uint64_t EncodedTexture::byteSize() const {
    uint64_t total = 0;
    for (const EncodedMip& mip : levels) {
        const uint64_t bytes = static_cast<uint64_t>(mip.bytes.size());
        if (total > std::numeric_limits<uint64_t>::max() - bytes) return std::numeric_limits<uint64_t>::max();
        total += bytes;
    }
    return total;
}

uint64_t EncodedAtlas::byteSize() const {
    uint64_t total = 0;
    for (const EncodedTexture& texture : textures) {
        const uint64_t bytes = texture.byteSize();
        if (total > std::numeric_limits<uint64_t>::max() - bytes) return std::numeric_limits<uint64_t>::max();
        total += bytes;
    }
    return total;
}

bool EncodedAtlas::anyValid() const {
    return std::any_of(textures.begin(), textures.end(), [](const EncodedTexture& texture) {
        return texture.valid();
    });
}

bool compressAtlasTexture(const af_texture& source, int slot, const GpuProfile& profile,
                          EncodedTexture* out, std::string* error) {
    if (!out || slot < 0 || slot >= AF_TEX_COUNT || profile.atlasCompression == AtlasCompression::NONE) {
        if (error) *error = "atlas compression is not available for this slot or GPU profile";
        return false;
    }
    *out = EncodedTexture{};
    try {
        std::vector<RawMip> mips;
        if (!buildMipChain(source, slot, &mips, error)) return false;
        bool encoded = false;
        if (profile.atlasCompression == AtlasCompression::ASTC_6X6)
            encoded = encodeAstc(mips, slot, profile, out, error);
        else if (profile.atlasCompression == AtlasCompression::ETC2)
            encoded = encodeEtc2(mips, slot, profile, out, error);
        if (!encoded) *out = EncodedTexture{};
        return encoded;
    } catch (const std::bad_alloc&) {
        *out = EncodedTexture{};
        if (error) *error = "out of memory while encoding atlas texture";
        return false;
    } catch (const std::exception& ex) {
        *out = EncodedTexture{};
        if (error) *error = std::string("texture encoder exception: ") + ex.what();
        return false;
    } catch (...) {
        *out = EncodedTexture{};
        if (error) *error = "unknown texture encoder failure";
        return false;
    }
}

AsyncAtlasCompression::~AsyncAtlasCompression() { waitDiscard(); }

bool AsyncAtlasCompression::start(const std::array<af_texture, AF_TEX_COUNT>& sources,
                                 const GpuProfile& profile) {
    waitDiscard();
    if (profile.atlasCompression == AtlasCompression::NONE) return false;
    try {
        future_ = std::async(std::launch::async, [sources, profile]() {
            EncodedAtlas result;
            for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
                std::string error;
                (void)compressAtlasTexture(sources[static_cast<size_t>(slot)], slot, profile,
                                           &result.textures[static_cast<size_t>(slot)], &error);
            }
            return result;
        });
        return true;
    } catch (...) {
        future_ = std::future<EncodedAtlas>{};
        return false;
    }
}

bool AsyncAtlasCompression::poll(EncodedAtlas* out) {
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    try {
        EncodedAtlas result = future_.get();
        if (out) *out = std::move(result);
    } catch (...) {
        if (out) *out = EncodedAtlas{};
    }
    return true;
}

void AsyncAtlasCompression::waitDiscard() {
    if (!future_.valid()) return;
    try {
        future_.wait();
        (void)future_.get();
    } catch (...) {
    }
}

} // namespace af
