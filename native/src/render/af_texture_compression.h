/*
 * af_texture_compression.h -- CPU block encoding and mip-chain preparation for GPU atlas uploads.
 *
 * The public af_texture C descriptor remains raw and stable. EncodedTexture is renderer-private;
 * callers keep the engine alive and immutable until AsyncAtlasCompression completes or is joined.
 */
#ifndef AF_TEXTURE_COMPRESSION_H
#define AF_TEXTURE_COMPRESSION_H

#include "af_gpu_profile.h"
#include "../../include/afcore.h"

#include <array>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace af {

enum class AtlasTextureEncoding : uint8_t {
    ASTC_6X6_UNORM,
    ASTC_6X6_SRGB,
    ETC2_RGBA8_SRGB,
    ETC2_RGBA8_UNORM,
    ETC2_R11_UNORM
};

struct EncodedMip {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bytes;
};

struct EncodedTexture {
    AtlasTextureEncoding encoding = AtlasTextureEncoding::ASTC_6X6_UNORM;
    int width = 0;
    int height = 0;
    std::vector<EncodedMip> levels;

    uint64_t byteSize() const;
    bool valid() const { return width > 0 && height > 0 && !levels.empty(); }
};

struct EncodedAtlas {
    std::array<EncodedTexture, AF_TEX_COUNT> textures{};

    uint64_t byteSize() const;
    bool anyValid() const;
};

/* Encodes one raw atlas slot and builds a complete mip chain (up to 12 levels). */
bool compressAtlasTexture(const af_texture& source, int slot, const GpuProfile& profile,
                          EncodedTexture* out, std::string* error = nullptr);

/*
 * Compression runs away from the render thread. The raw descriptors passed to start() are copied,
 * but their data pointers remain owned by the engine; that engine must remain alive and its texture
 * storage immutable until poll() returns a completed result or waitDiscard() has joined the worker.
 */
class AsyncAtlasCompression {
public:
    AsyncAtlasCompression() = default;
    ~AsyncAtlasCompression();
    AsyncAtlasCompression(const AsyncAtlasCompression&) = delete;
    AsyncAtlasCompression& operator=(const AsyncAtlasCompression&) = delete;

    bool start(const std::array<af_texture, AF_TEX_COUNT>& sources, const GpuProfile& profile);
    /* Returns true once a job has completed, including a failed/empty encode result. */
    bool poll(EncodedAtlas* out);
    void waitDiscard();
    bool running() const { return future_.valid(); }

private:
    std::future<EncodedAtlas> future_;
};

} // namespace af
#endif /* AF_TEXTURE_COMPRESSION_H */
