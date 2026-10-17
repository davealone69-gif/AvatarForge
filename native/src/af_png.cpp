#include "af_png.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace af {
namespace {

const char* g_lastError = "";

inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
}

inline int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

inline int absSum(const std::vector<uint8_t>& row) {
    int s = 0;
    for (uint8_t b : row) s += (b < 128) ? b : (256 - b);
    return s;
}

/** One PNG chunk: length, type, payload, CRC32 over type+payload. */
void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    put32(out, static_cast<uint32_t>(len));
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    const uLong crc = crc32(0L, out.data() + crcStart, static_cast<uInt>(out.size() - crcStart));
    put32(out, static_cast<uint32_t>(crc));
}

} // namespace

const char* encodePngLastError() { return g_lastError; }

bool encodePng(const uint8_t* pixels, int width, int height, int channels, std::vector<uint8_t>* out) {
    g_lastError = "";
    if (!pixels || !out) { g_lastError = "null argument"; return false; }
    if (width <= 0 || height <= 0) { g_lastError = "empty image"; return false; }
    if (channels != 1 && channels != 3 && channels != 4) { g_lastError = "channels must be 1, 3 or 4"; return false; }
    /* PNG's 2^31-1 limit is not the practical one; the workspace limit is a 4096 map. */
    if (width > 8192 || height > 8192) { g_lastError = "image too large"; return false; }

    const size_t stride = static_cast<size_t>(width) * static_cast<size_t>(channels);
    const size_t bpp = static_cast<size_t>(channels);   /* bytes per pixel, for the filters */

    /* ---- filtered scanlines ---------------------------------------------------------------- */
    std::vector<uint8_t> filtered;
    filtered.reserve((stride + 1) * static_cast<size_t>(height) + 64);
    std::vector<uint8_t> best((stride + 1) * 0 + stride, 0), cand(stride, 0), prev(stride, 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* cur = pixels + static_cast<size_t>(y) * stride;
        int bestType = 0;
        int bestScore = -1;
        for (int type = 0; type < 5; ++type) {
            for (size_t i = 0; i < stride; ++i) {
                const int a = (i >= bpp) ? cur[i - bpp] : 0;                       /* left        */
                const int b = (y > 0) ? prev[i] : 0;                                /* up          */
                const int c = (y > 0 && i >= bpp) ? prev[i - bpp] : 0;              /* up-left     */
                int v = 0;
                switch (type) {
                    case 0: v = cur[i]; break;
                    case 1: v = cur[i] - a; break;
                    case 2: v = cur[i] - b; break;
                    case 3: v = cur[i] - ((a + b) >> 1); break;
                    default: v = cur[i] - paeth(a, b, c); break;
                }
                cand[i] = static_cast<uint8_t>(v & 0xFF);
            }
            const int score = absSum(cand);
            if (bestScore < 0 || score < bestScore) {
                bestScore = score;
                bestType = type;
                best = cand;
            }
            /* Filter type 0 has zero cost to produce; if the row is already smooth there is no
               point testing the rest, and on a flat region this is most rows. */
            if (type == 0 && score == 0) break;
        }
        filtered.push_back(static_cast<uint8_t>(bestType));
        filtered.insert(filtered.end(), best.begin(), best.end());
        std::memcpy(prev.data(), cur, stride);
    }

    /* ---- zlib stream ------------------------------------------------------------------------ */
    uLongf compressedLen = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<uint8_t> compressed(compressedLen);
    const int rc = compress2(compressed.data(), &compressedLen, filtered.data(),
                             static_cast<uLong>(filtered.size()), 6);
    if (rc != Z_OK) {
        g_lastError = "zlib compression failed";
        return false;
    }
    compressed.resize(compressedLen);

    /* ---- assemble --------------------------------------------------------------------------- */
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out->insert(out->end(), kSignature, kSignature + 8);

    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>((width >> 24) & 0xFF); ihdr[1] = static_cast<uint8_t>((width >> 16) & 0xFF);
    ihdr[2] = static_cast<uint8_t>((width >> 8) & 0xFF);  ihdr[3] = static_cast<uint8_t>(width & 0xFF);
    ihdr[4] = static_cast<uint8_t>((height >> 24) & 0xFF); ihdr[5] = static_cast<uint8_t>((height >> 16) & 0xFF);
    ihdr[6] = static_cast<uint8_t>((height >> 8) & 0xFF);  ihdr[7] = static_cast<uint8_t>(height & 0xFF);
    ihdr[8] = 8;                                              /* bit depth                */
    ihdr[9] = (channels == 1) ? 0 : ((channels == 3) ? 2 : 6); /* colour type 0 / 2 / 6    */
    ihdr[10] = 0;                                             /* deflate                  */
    ihdr[11] = 0;                                             /* adaptive filtering       */
    ihdr[12] = 0;                                             /* no interlace             */
    chunk(*out, "IHDR", ihdr, sizeof(ihdr));

    /* IDAT is split so that no single chunk exceeds the 2^31-1 spec limit; 1 MiB is comfortable. */
    const size_t kMaxIdat = 1u << 20;
    for (size_t off = 0; off < compressed.size(); off += kMaxIdat) {
        const size_t n = std::min(kMaxIdat, compressed.size() - off);
        chunk(*out, "IDAT", compressed.data() + off, n);
    }
    chunk(*out, "IEND", nullptr, 0);
    return true;
}

} // namespace af
