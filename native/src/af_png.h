/*
 * af_png.h -- minimal PNG writer.
 *
 * The export path needs real PNG files inside the GLB (glTF's image/png is the only format every
 * viewer is guaranteed to read) and the host tools need them on disk for inspection. There is no
 * image library in the project - deliberately, because the engine has to build with nothing but a
 * C++17 compiler and zlib on every platform it targets, Android included.
 *
 * Supported: 8-bit grayscale, RGB and RGBA, non-interlaced. That is the entire requirement:
 * the bake produces exactly those four-channel (or one-channel, for ambient occlusion) maps.
 *
 * Filtering is adaptive per scanline (the standard minimum-sum-of-absolute-differences heuristic
 * over all five PNG filter types). It costs a few hundred microseconds per megapixel and typically
 * halves the file compared with "filter type 0 everywhere", which matters when the base colour,
 * normal, metallic-roughness and subsurface maps go into one GLB.
 */
#ifndef AF_PNG_H
#define AF_PNG_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace af {

/**
 * Encodes 8-bit PNG.
 *   pixels   row-major, tightly packed, `channels` bytes per pixel
 *   channels 1 (grayscale), 3 (RGB) or 4 (RGBA)
 * Appends to `out` and returns true on success.
 */
bool encodePng(const uint8_t* pixels, int width, int height, int channels, std::vector<uint8_t>* out);

/** Human-readable reason the last encodePng() failed, or "" if it succeeded. */
const char* encodePngLastError();

} // namespace af
#endif /* AF_PNG_H */
