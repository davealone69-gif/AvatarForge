/*
 * af_png_decode.h -- PNG decoding for the glTF importer.
 *
 * af_png.h only encodes (that is all the exporter ever needed). Loading a .glb means decoding the
 * images inside it, and glTF's mandatory texture format is PNG, so this is the decoder the
 * *import* path uses. It deliberately covers the subset a glTF export actually produces and says so
 * precisely when it meets anything else:
 *
 *   colour types   0 (grey), 2 (RGB), 3 (palette), 4 (grey+alpha), 6 (RGBA)
 *   bit depths     8, and 16 down-scaled to 8 by taking the high byte (exact for 8-bit sources
 *                  widened by a writer); 1/2/4-bit palettes are expanded
 *   interlacing    none; Adam7 is rejected with a clear reason, never guessed at
 *   chunks         IHDR, PLTE, tRNS, IDAT (multiple, concatenated), IEND; unknown critical chunks
 *                  are rejected, unknown ancillary chunks are skipped as the spec requires
 *
 * Output is always tight RGBA8, which is what both the CPU texture stores and the GPU upload path
 * in the renderer already speak.
 */
#ifndef AF_PNG_DECODE_H
#define AF_PNG_DECODE_H

#include <cstdint>
#include <string>
#include <vector>

namespace af {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   /* width * height * 4 */
};

/**
 * Decodes `data` (a complete PNG file) into RGBA8. Returns false on malformed or unsupported input
 * and fills `error` with a specific reason. Never throws; `out` is left empty on failure.
 */
bool decodePng(const uint8_t* data, size_t size, DecodedImage* out, std::string* error);

/** True when `data` begins with the 8-byte PNG signature. */
bool looksLikePng(const uint8_t* data, size_t size);

} /* namespace af */
#endif /* AF_PNG_DECODE_H */
