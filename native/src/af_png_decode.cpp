/*
 * af_png_decode.cpp -- zlib inflate + PNG reconstruction.
 *
 * The inflate half is a straight RFC 1950/1951 decoder: canonical Huffman codes built as
 * (count,first-symbol,index) tables, LSB-first bit reading, and a 32 KB sliding window. It is not
 * tuned for speed - a 2048x2048 texture inflates in a few tens of milliseconds, once, during
 * import - it is tuned for being correct and bounded, because the bytes come from a file the user
 * supplied.
 *
 * Every length is checked against the destination before it is written, which is what makes a
 * truncated or hostile stream produce an error instead of an overrun. That property is exercised
 * by the harness: it feeds the decoder truncated streams and corrupted code tables and only
 * accepts "decoded" or "failed cleanly", never a crash.
 */
#include "af_png_decode.h"

#include <cstring>

namespace af {
namespace {

constexpr int MAX_BITS = 15;

/* ------------------------------------------------------------------------------ inflate ---- */
struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;      /* byte position */
    uint32_t bitBuf = 0;
    int bitCount = 0;
    bool ok = true;

    uint32_t bits(int need) {
        while (bitCount < need) {
            if (pos >= size) {
                ok = false;
                return 0;
            }
            bitBuf |= static_cast<uint32_t>(data[pos++]) << bitCount;
            bitCount += 8;
        }
        const uint32_t v = bitBuf & ((1u << need) - 1u);
        bitBuf >>= need;
        bitCount -= need;
        return v;
    }
    void alignByte() {
        const int drop = bitCount & 7;
        bitBuf >>= drop;
        bitCount -= drop;
    }
};

struct Huffman {
    uint16_t count[MAX_BITS + 1];
    uint16_t symbol[320];

    /* Builds canonical decoding tables from code lengths. `n` is the alphabet size. */
    bool build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof(count));
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        if (count[0] == n) return true; /* no codes: legal, never decodes */
        int left = 1;
        for (int len = 1; len <= MAX_BITS; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; /* over-subscribed */
        }
        uint16_t offs[MAX_BITS + 1];
        offs[1] = 0;
        for (int len = 1; len < MAX_BITS; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int i = 0; i < n; ++i) {
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        }
        return true;
    }

    /* Decodes one symbol, walking bit by bit. Simple, and provably bounded: at most MAX_BITS
       reads before it either succeeds or fails. */
    int decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= MAX_BITS; ++len) {
            code |= static_cast<int>(in.bits(1));
            if (!in.ok) return -1;
            const int cnt = count[len];
            if (code - first < cnt) return symbol[index + (code - first)];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -1;
    }
};

/* Length and distance bases/extras from RFC 1951 section 3.2.5. */
const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
                                16385, 24577};
const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>* out, std::string* error) {
    BitReader in{data, size};
    std::vector<uint8_t> window;  /* grows with output; LZ77 back-references address it directly */
    window.reserve(size * 4 + 1024);

    bool last = false;
    while (!last) {
        last = in.bits(1) != 0;
        const uint32_t type = in.bits(2);
        if (!in.ok) {
            if (error) *error = "deflate: truncated block header";
            return false;
        }
        if (type == 0) {
            in.alignByte();
            const uint32_t len = in.bits(16);
            const uint32_t nlen = in.bits(16);
            if (!in.ok || (len ^ 0xFFFFu) != nlen) {
                if (error) *error = "deflate: bad stored-block length";
                return false;
            }
            for (uint32_t i = 0; i < len; ++i) {
                const uint32_t b = in.bits(8);
                if (!in.ok) {
                    if (error) *error = "deflate: truncated stored block";
                    return false;
                }
                window.push_back(static_cast<uint8_t>(b));
            }
            continue;
        }
        if (type == 3) {
            if (error) *error = "deflate: reserved block type 3";
            return false;
        }

        Huffman lit, dist;
        if (type == 1) {
            uint8_t lengths[288];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            if (!lit.build(lengths, 288)) {
                if (error) *error = "deflate: bad fixed literal table";
                return false;
            }
            uint8_t d[30];
            for (int i = 0; i < 30; ++i) d[i] = 5;
            if (!dist.build(d, 30)) {
                if (error) *error = "deflate: bad fixed distance table";
                return false;
            }
        } else {
            const int hlit = static_cast<int>(in.bits(5)) + 257;
            const int hdist = static_cast<int>(in.bits(5)) + 1;
            const int hclen = static_cast<int>(in.bits(4)) + 4;
            if (!in.ok) {
                if (error) *error = "deflate: truncated dynamic header";
                return false;
            }
            if (hlit > 286 || hdist > 30) {
                if (error) *error = "deflate: dynamic table too large";
                return false;
            }
            static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t clen[19] = {0};
            for (int i = 0; i < hclen; ++i) clen[ORDER[i]] = static_cast<uint8_t>(in.bits(3));
            if (!in.ok) {
                if (error) *error = "deflate: truncated code-length table";
                return false;
            }
            Huffman cl;
            if (!cl.build(clen, 19)) {
                if (error) *error = "deflate: bad code-length table";
                return false;
            }
            uint8_t lengths[288 + 30];
            int n = 0;
            while (n < hlit + hdist) {
                const int sym = cl.decode(in);
                if (sym < 0 || !in.ok) {
                    if (error) *error = "deflate: bad code-length symbol";
                    return false;
                }
                if (sym < 16) {
                    lengths[n++] = static_cast<uint8_t>(sym);
                } else if (sym == 16) {
                    if (n == 0) {
                        if (error) *error = "deflate: repeat with no previous length";
                        return false;
                    }
                    const int rep = 3 + static_cast<int>(in.bits(2));
                    const uint8_t prev = lengths[n - 1];
                    if (n + rep > hlit + hdist) {
                        if (error) *error = "deflate: length repeat overruns table";
                        return false;
                    }
                    for (int i = 0; i < rep; ++i) lengths[n++] = prev;
                } else if (sym == 17) {
                    const int rep = 3 + static_cast<int>(in.bits(3));
                    if (n + rep > hlit + hdist) {
                        if (error) *error = "deflate: zero repeat overruns table";
                        return false;
                    }
                    for (int i = 0; i < rep; ++i) lengths[n++] = 0;
                } else {
                    const int rep = 11 + static_cast<int>(in.bits(7));
                    if (n + rep > hlit + hdist) {
                        if (error) *error = "deflate: long zero repeat overruns table";
                        return false;
                    }
                    for (int i = 0; i < rep; ++i) lengths[n++] = 0;
                }
                if (!in.ok) {
                    if (error) *error = "deflate: truncated length table";
                    return false;
                }
            }
            if (lengths[256] == 0) {
                if (error) *error = "deflate: no end-of-block code";
                return false;
            }
            if (!lit.build(lengths, hlit)) {
                if (error) *error = "deflate: bad literal table";
                return false;
            }
            if (!dist.build(lengths + hlit, hdist)) {
                if (error) *error = "deflate: bad distance table";
                return false;
            }
        }

        while (true) {
            const int sym = lit.decode(in);
            if (sym < 0 || !in.ok) {
                if (error) *error = "deflate: bad literal symbol";
                return false;
            }
            if (sym < 256) {
                window.push_back(static_cast<uint8_t>(sym));
                continue;
            }
            if (sym == 256) break; /* end of block */
            const int li = sym - 257;
            if (li >= 29) {
                if (error) *error = "deflate: length symbol out of range";
                return false;
            }
            const int length = LEN_BASE[li] + static_cast<int>(in.bits(LEN_EXTRA[li]));
            const int dsym = dist.decode(in);
            if (dsym < 0 || dsym >= 30 || !in.ok) {
                if (error) *error = "deflate: bad distance symbol";
                return false;
            }
            const int distance = DIST_BASE[dsym] + static_cast<int>(in.bits(DIST_EXTRA[dsym]));
            if (distance <= 0 || static_cast<size_t>(distance) > window.size()) {
                if (error) *error = "deflate: distance beyond start of output";
                return false;
            }
            /* Back-reference; may overlap the write position, which is how runs are encoded. */
            const size_t from = window.size() - static_cast<size_t>(distance);
            for (int i = 0; i < length; ++i) window.push_back(window[from + static_cast<size_t>(i)]);
        }
    }
    in.alignByte();
    *out = std::move(window);
    return true;
}

bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>* out, std::string* error) {
    if (size < 6) {
        if (error) *error = "zlib: stream shorter than a header";
        return false;
    }
    const uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8) {
        if (error) *error = "zlib: not a deflate stream";
        return false;
    }
    if (((static_cast<uint32_t>(cmf) << 8) | flg) % 31 != 0) {
        if (error) *error = "zlib: bad header checksum";
        return false;
    }
    if (flg & 0x20) {
        if (error) *error = "zlib: preset dictionaries are not supported";
        return false;
    }
    if (!inflateRaw(data + 2, size - 2, out, error)) return false;
    /* Adler-32 over the output, when the trailer is present. A truncated trailer is tolerated
       (some writers omit it); a wrong one is a hard failure, because the texture would be
       silently corrupt. */
    if (size >= 6) {
        const uint8_t* tail = data + size - 4;
        uint32_t want = (static_cast<uint32_t>(tail[0]) << 24) | (static_cast<uint32_t>(tail[1]) << 16) |
                        (static_cast<uint32_t>(tail[2]) << 8) | tail[3];
        uint32_t a = 1, b = 0;
        for (uint8_t v : *out) {
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        const uint32_t got = (b << 16) | a;
        if (want != got) {
            if (error) *error = "zlib: adler32 mismatch";
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------------- PNG ---- */
uint32_t readU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

} /* namespace */

bool looksLikePng(const uint8_t* data, size_t size) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data && size >= 8 && std::memcmp(data, SIG, 8) == 0;
}

bool decodePng(const uint8_t* data, size_t size, DecodedImage* out, std::string* error) {
    if (error) error->clear();
    if (out) {
        out->width = out->height = 0;
        out->rgba.clear();
    }
    if (!data || !out) {
        if (error) *error = "png: null input";
        return false;
    }
    if (!looksLikePng(data, size)) {
        if (error) *error = "png: bad signature";
        return false;
    }

    int width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
    std::vector<uint8_t> palette;      /* RGB triples */
    std::vector<uint8_t> paletteAlpha;
    std::vector<uint8_t> idat;

    size_t pos = 8;
    bool sawIhdr = false, sawIend = false;
    while (pos + 8 <= size && !sawIend) {
        const uint32_t len = readU32(data + pos);
        const char* type = reinterpret_cast<const char*>(data + pos + 4);
        if (len > 0x7FFFFFFFu || pos + 12 + static_cast<size_t>(len) > size) {
            if (error) *error = "png: chunk length past end of file";
            return false;
        }
        const uint8_t* body = data + pos + 8;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) {
                if (error) *error = "png: malformed IHDR";
                return false;
            }
            width = static_cast<int>(readU32(body));
            height = static_cast<int>(readU32(body + 4));
            bitDepth = body[8];
            colorType = body[9];
            if (body[10] != 0 || body[11] != 0) {
                if (error) *error = "png: unsupported compression or filter method";
                return false;
            }
            interlace = body[12];
            sawIhdr = true;
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
                if (error) *error = "png: unreasonable dimensions";
                return false;
            }
            if (interlace != 0) {
                if (error) *error = "png: Adam7 interlacing is not supported";
                return false;
            }
            const bool depthOk = (colorType == 0 && (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                                                     bitDepth == 8 || bitDepth == 16)) ||
                                 (colorType == 3 && (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                                                     bitDepth == 8)) ||
                                 ((colorType == 2 || colorType == 4 || colorType == 6) &&
                                  (bitDepth == 8 || bitDepth == 16));
            if (!depthOk) {
                if (error) *error = "png: unsupported colour type / bit depth combination";
                return false;
            }
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(body, body + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            if (colorType == 3) paletteAlpha.assign(body, body + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            sawIend = true;
        } else if (!(type[0] & 0x20)) {
            /* Unknown *critical* chunk: the decoder cannot know what it changes. */
            if (error) {
                *error = std::string("png: unknown critical chunk '") + std::string(type, 4) + "'";
            }
            return false;
        }
        pos += 12 + static_cast<size_t>(len);
    }
    if (!sawIhdr || idat.empty()) {
        if (error) *error = "png: missing IHDR or IDAT";
        return false;
    }

    int channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default:
            if (error) *error = "png: unsupported colour type";
            return false;
    }

    std::vector<uint8_t> raw;
    if (!inflateZlib(idat.data(), idat.size(), &raw, error)) return false;

    /* Bytes per pixel used for the filter's "left" neighbour: ceil(bits per pixel / 8), never
       less than 1. */
    const int bitsPerPixel = channels * bitDepth;
    const size_t bpp = static_cast<size_t>((bitsPerPixel + 7) / 8);
    const size_t rowBytes = (static_cast<size_t>(width) * static_cast<size_t>(bitsPerPixel) + 7) / 8;
    const size_t expected = (rowBytes + 1) * static_cast<size_t>(height);
    if (raw.size() < expected) {
        if (error) *error = "png: image data shorter than the header says";
        return false;
    }

    /* Unfilter in place into a contiguous pixel buffer. */
    std::vector<uint8_t> pixels(rowBytes * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = raw.data() + static_cast<size_t>(y) * (rowBytes + 1);
        const uint8_t filter = src[0];
        const uint8_t* line = src + 1;
        uint8_t* dst = pixels.data() + static_cast<size_t>(y) * rowBytes;
        const uint8_t* prev = (y > 0) ? pixels.data() + static_cast<size_t>(y - 1) * rowBytes : nullptr;
        for (size_t x = 0; x < rowBytes; ++x) {
            const int a = (x >= bpp) ? dst[x - bpp] : 0;
            const int b = prev ? prev[x] : 0;
            const int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = line[x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += paeth(a, b, c); break;
                default:
                    if (error) *error = "png: unknown row filter";
                    return false;
            }
            dst[x] = static_cast<uint8_t>(v);
        }
    }

    /* Expand to RGBA8. */
    out->width = width;
    out->height = height;
    out->rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    auto sample = [&](size_t index, int channel, bool sixteen) -> uint8_t {
        if (bitDepth == 8) return pixels[index * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
        if (sixteen) return pixels[index * static_cast<size_t>(channels) * 2 + static_cast<size_t>(channel) * 2];
        /* sub-byte depths (only for grey / palette) */
        const size_t bitPos = index * static_cast<size_t>(bitDepth);
        const uint8_t byte = pixels[bitPos / 8];
        const int shift = 8 - bitDepth - static_cast<int>(bitPos % 8);
        const int mask = (1 << bitDepth) - 1;
        return static_cast<uint8_t>((byte >> shift) & mask);
    };

    const bool sixteen = (bitDepth == 16);
    if (colorType == 3) {
        if (palette.empty()) {
            if (error) *error = "png: palette image without PLTE";
            return false;
        }
        const size_t entries = palette.size() / 3;
        for (size_t i = 0; i < pixelCount; ++i) {
            int idx = 0;
            if (bitDepth == 8) {
                idx = pixels[i];
            } else {
                const size_t bitPos = i * static_cast<size_t>(bitDepth);
                const int shift = 8 - bitDepth - static_cast<int>(bitPos % 8);
                idx = (pixels[bitPos / 8] >> shift) & ((1 << bitDepth) - 1);
            }
            if (static_cast<size_t>(idx) >= entries) {
                if (error) *error = "png: palette index out of range";
                return false;
            }
            out->rgba[i * 4 + 0] = palette[static_cast<size_t>(idx) * 3 + 0];
            out->rgba[i * 4 + 1] = palette[static_cast<size_t>(idx) * 3 + 1];
            out->rgba[i * 4 + 2] = palette[static_cast<size_t>(idx) * 3 + 2];
            out->rgba[i * 4 + 3] =
                (static_cast<size_t>(idx) < paletteAlpha.size()) ? paletteAlpha[static_cast<size_t>(idx)] : 255;
        }
        return true;
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        switch (colorType) {
            case 0: {
                const uint8_t g = sample(i, 0, sixteen);
                out->rgba[i * 4 + 0] = g;
                out->rgba[i * 4 + 1] = g;
                out->rgba[i * 4 + 2] = g;
                out->rgba[i * 4 + 3] = 255;
                break;
            }
            case 2:
                out->rgba[i * 4 + 0] = sample(i, 0, sixteen);
                out->rgba[i * 4 + 1] = sample(i, 1, sixteen);
                out->rgba[i * 4 + 2] = sample(i, 2, sixteen);
                out->rgba[i * 4 + 3] = 255;
                break;
            case 4: {
                const uint8_t g = sample(i, 0, sixteen);
                out->rgba[i * 4 + 0] = g;
                out->rgba[i * 4 + 1] = g;
                out->rgba[i * 4 + 2] = g;
                out->rgba[i * 4 + 3] = sample(i, 1, sixteen);
                break;
            }
            case 6:
                out->rgba[i * 4 + 0] = sample(i, 0, sixteen);
                out->rgba[i * 4 + 1] = sample(i, 1, sixteen);
                out->rgba[i * 4 + 2] = sample(i, 2, sixteen);
                out->rgba[i * 4 + 3] = sample(i, 3, sixteen);
                break;
            default:
                break;
        }
    }
    return true;
}

} /* namespace af */
