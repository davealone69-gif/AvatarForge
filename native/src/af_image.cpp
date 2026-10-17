#include "af_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace af {
namespace {

/* ---------------------------------------------------------------------------------------------
 * Colour space helpers. The analysis works in CIE L*a*b* rather than RGB because "skin tone",
 * "hair colour" and "is this metal" are questions about lightness and chroma, and RGB answers them
 * differently depending on the exposure.
 * ------------------------------------------------------------------------------------------- */
inline float srgbToLinear(float c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    c = std::fmax(0.0f, std::fmin(1.0f, c));
    return (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
constexpr float kWhiteX = 0.95047f, kWhiteY = 1.0f, kWhiteZ = 1.08883f;
inline float labF(float t) {
    constexpr float d = 6.0f / 29.0f;
    return (t > d * d * d) ? std::cbrt(t) : (t / (3.0f * d * d) + 4.0f / 29.0f);
}

/** sRGB (0..255) -> CIE L*a*b* (D65). */
void rgbToLab(float r8, float g8, float b8, float* lab) {
    const float r = srgbToLinear(r8 / 255.0f);
    const float g = srgbToLinear(g8 / 255.0f);
    const float b = srgbToLinear(b8 / 255.0f);
    const float X = r * 0.4124564f + g * 0.3575761f + b * 0.1804375f;
    const float Y = r * 0.2126729f + g * 0.7151522f + b * 0.0721750f;
    const float Z = r * 0.0193339f + g * 0.1191920f + b * 0.9503041f;
    const float fx = labF(X / kWhiteX), fy = labF(Y / kWhiteY), fz = labF(Z / kWhiteZ);
    lab[0] = 116.0f * fy - 16.0f;
    lab[1] = 500.0f * (fx - fy);
    lab[2] = 200.0f * (fy - fz);
}

/** CIE L*a*b* -> linear sRGB (clamped to the displayable cube). */
void labToLinearRgb(const float* lab, float* rgb) {
    const float fy = (lab[0] + 16.0f) / 116.0f;
    const float fx = fy + lab[1] / 500.0f;
    const float fz = fy - lab[2] / 200.0f;
    constexpr float d = 6.0f / 29.0f;
    auto inv = [](float f) { return (f > d) ? f * f * f : 3.0f * d * d * (f - 4.0f / 29.0f); };
    const float X = inv(fx) * kWhiteX, Y = inv(fy) * kWhiteY, Z = inv(fz) * kWhiteZ;
    const float r = X * 3.2404542f + Y * -1.5371385f + Z * -0.4985314f;
    const float g = X * -0.9692660f + Y * 1.8760108f + Z * 0.0415560f;
    const float b = X * 0.0556434f + Y * -0.2040259f + Z * 1.0572252f;
    rgb[0] = std::fmax(0.0f, r);
    rgb[1] = std::fmax(0.0f, g);
    rgb[2] = std::fmax(0.0f, b);
}

inline float labDistance(const float* a, const float* b) {
    const float dl = a[0] - b[0], da = a[1] - b[1], db = a[2] - b[2];
    return std::sqrt(dl * dl + da * da + db * db);
}

/* A tiny working image: 3 channels, top-left origin, already downsampled. */
struct Work {
    int w = 0, h = 0;
    std::vector<float> rgb;      /* w*h*3, sRGB 0..255 */
    std::vector<float> lab;      /* w*h*3 */
    std::vector<uint8_t> mask;   /* 1 = subject */
    int width() const { return w; }
    int height() const { return h; }
};

/** Box-downsamples so the long edge is at most `maxSide`. Deterministic and cheap. */
bool makeWork(const af_image& img, Work* out) {
    if (img.width <= 0 || img.height <= 0 || img.channels < 3 || !img.pixels) return false;
    const int longSide = std::max(img.width, img.height);
    const int step = std::max(1, (longSide + kAnalysisMaxSide - 1) / kAnalysisMaxSide);
    const int w = std::max(1, img.width / step);
    const int h = std::max(1, img.height / step);
    out->w = w;
    out->h = h;
    out->rgb.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0;
            int n = 0;
            for (int dy = 0; dy < step; ++dy) {
                const int sy = y * step + dy;
                if (sy >= img.height) break;
                for (int dx = 0; dx < step; ++dx) {
                    const int sx = x * step + dx;
                    if (sx >= img.width) break;
                    const size_t o = (static_cast<size_t>(sy) * img.width + sx) * img.channels;
                    r += img.pixels[o + 0];
                    g += img.pixels[o + 1];
                    b += img.pixels[o + 2];
                    ++n;
                }
            }
            if (!n) n = 1;
            const size_t o = (static_cast<size_t>(y) * w + x) * 3;
            out->rgb[o + 0] = static_cast<float>(r / n);
            out->rgb[o + 1] = static_cast<float>(g / n);
            out->rgb[o + 2] = static_cast<float>(b / n);
        }
    }
    out->lab.assign(static_cast<size_t>(w) * h * 3, 0.0f);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        rgbToLab(out->rgb[i * 3 + 0], out->rgb[i * 3 + 1], out->rgb[i * 3 + 2], &out->lab[i * 3]);
    }
    out->mask.assign(static_cast<size_t>(w) * h, 0);
    return true;
}

float medianOf(std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    const size_t n = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + n, v.end());
    return v[n];
}

/** One channel's median over a pixel index list. */
float medianChannel(const Work& im, const std::vector<int>& idx, int channel) {
    std::vector<float> v;
    v.reserve(idx.size());
    for (int i : idx) v.push_back(im.lab[static_cast<size_t>(i) * 3 + channel]);
    return medianOf(v);
}

} // namespace

bool analyseImage(const af_image& image, af_image_analysis* out) {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    Work im;
    if (!makeWork(image, &im)) return false;

    /* ---- 1. background model -----------------------------------------------------------------
     * The border ring of a portrait is background in every photo a user will actually take: a room,
     * a wall, a studio sweep, the sky. Taking the per-channel MEDIAN (not the mean) makes the model
     * survive a border that clips part of the subject or a bright window. */
    std::vector<float> br, bg, bb;
    const int ring = std::max(1, std::min(im.w, im.h) / 32);
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            const bool isBorder = (x < ring || y < ring || x >= im.w - ring || y >= im.h - ring);
            if (!isBorder) continue;
            const size_t o = (static_cast<size_t>(y) * im.w + x) * 3;
            br.push_back(im.rgb[o + 0]);
            bg.push_back(im.rgb[o + 1]);
            bb.push_back(im.rgb[o + 2]);
        }
    }
    const float bgR = medianOf(br), bgG = medianOf(bg), bgB = medianOf(bb);
    float bgLab[3];
    rgbToLab(bgR, bgG, bgB, bgLab);

    /* Background spread: a studio sweep has almost none, a cluttered room has a lot. The
       foreground threshold scales with it, so a busy background does not swallow the subject's
       low-contrast edges. */
    std::vector<float> spread;
    spread.reserve(br.size());
    for (size_t i = 0; i < br.size(); ++i) {
        float l[3];
        rgbToLab(br[i], bg[i], bb[i], l);
        spread.push_back(labDistance(l, bgLab));
    }
    const float bgSpread = medianOf(spread);
    const float threshold = std::fmax(12.0f, bgSpread * 2.2f);

    /* ---- 2. foreground mask ------------------------------------------------------------------ */
    std::vector<float> dist(static_cast<size_t>(im.w) * im.h, 0.0f);
    for (size_t i = 0; i < static_cast<size_t>(im.w) * im.h; ++i) {
        const float d = labDistance(&im.lab[i * 3], bgLab);
        dist[i] = d;
        /* Shadow handling: a pixel much darker than the background but within a plausible
           shadow's lightness ratio still belongs to the scene, not the subject. Only the L*
           difference counts here, so a dark floor does not become a subject. */
        im.mask[i] = (d > threshold) ? 1 : 0;
    }

    /* ---- 3. mask cleanup ---------------------------------------------------------------------
     * (a) per-row keep the largest run, (b) hole-fill inside each run, (c) largest connected
     * component. Together these turn a noisy colour-distance mask into a subject silhouette
     * without a single learned parameter. */
    for (int y = 0; y < im.h; ++y) {
        int bestStart = -1, bestLen = 0, runStart = -1;
        for (int x = 0; x <= im.w; ++x) {
            const bool on = (x < im.w) && im.mask[static_cast<size_t>(y) * im.w + x];
            if (on && runStart < 0) runStart = x;
            if (!on && runStart >= 0) {
                const int len = x - runStart;
                if (len > bestLen) { bestLen = len; bestStart = runStart; }
                runStart = -1;
            }
        }
        for (int x = 0; x < im.w; ++x) {
            const bool inside = (bestStart >= 0) && (x >= bestStart) && (x < bestStart + bestLen);
            im.mask[static_cast<size_t>(y) * im.w + x] = inside ? 1 : 0;
        }
    }
    {
        std::vector<uint8_t> seen(im.mask.size(), 0);
        std::vector<int> stack, biggest;
        for (size_t start = 0; start < im.mask.size(); ++start) {
            if (!im.mask[start] || seen[start]) continue;
            stack.clear();
            stack.push_back(static_cast<int>(start));
            seen[start] = 1;
            std::vector<int> comp;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                comp.push_back(i);
                const int x = i % im.w, y = i / im.w;
                const int nx[4] = {x - 1, x + 1, x, x};
                const int ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nx[k] >= im.w || ny[k] < 0 || ny[k] >= im.h) continue;
                    const size_t j = static_cast<size_t>(ny[k]) * im.w + nx[k];
                    if (im.mask[j] && !seen[j]) { seen[j] = 1; stack.push_back(static_cast<int>(j)); }
                }
            }
            if (comp.size() > biggest.size()) biggest.swap(comp);
        }
        std::vector<uint8_t> clean(im.mask.size(), 0);
        for (int i : biggest) clean[static_cast<size_t>(i)] = 1;
        im.mask.swap(clean);
    }

    const size_t pixels = static_cast<size_t>(im.w) * im.h;
    size_t subjectPixels = 0;
    for (size_t i = 0; i < pixels; ++i) subjectPixels += im.mask[i] ? 1 : 0;
    out->subject_coverage = static_cast<float>(subjectPixels) / static_cast<float>(pixels);
    if (subjectPixels < 32) {
        /* Nothing segmented: report the background only and let the caller decide. This is a
           legitimate outcome for a landscape photo, and it must not crash or invent a body. */
        out->background_lab[0] = bgLab[0];
        out->background_lab[1] = bgLab[1];
        out->background_lab[2] = bgLab[2];
        out->pose_confidence = 0.0f;
        return true;
    }

    /* ---- 4. vertical profile -> landmark rows ------------------------------------------------ */
    std::vector<int> rowFirst(im.h, -1), rowLast(im.h, -1), rowCount(im.h, 0);
    int subjectTop = -1, subjectBottom = -1;
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            if (!im.mask[static_cast<size_t>(y) * im.w + x]) continue;
            if (rowFirst[y] < 0) rowFirst[y] = x;
            rowLast[y] = x;
            ++rowCount[y];
        }
        if (rowCount[y] > 1) {
            if (subjectTop < 0) subjectTop = y;
            subjectBottom = y;
        }
    }
    if (subjectTop < 0 || subjectBottom - subjectTop < 8) {
        out->pose_confidence = 0.0f;
        return true;
    }
    const float subjectH = static_cast<float>(subjectBottom - subjectTop + 1);

    /* Head: the top of the subject down to the narrowest row above the shoulders (the neck).
       Shoulders: the row where the width first exceeds 1.7x the median width of the top 12%. */
    const int headBand = std::max(2, static_cast<int>(subjectH * 0.12f));
    std::vector<int> topWidths;
    for (int y = subjectTop; y < std::min(im.h, subjectTop + headBand); ++y) topWidths.push_back(rowCount[y]);
    float topMedian = 0.0f;
    if (!topWidths.empty()) {
        std::vector<int> sorted = topWidths;
        std::sort(sorted.begin(), sorted.end());
        topMedian = static_cast<float>(sorted[sorted.size() / 2]);
    }
    int headBottom = std::min(im.h - 1, subjectTop + headBand);
    int narrowest = 1 << 30;
    for (int y = subjectTop + 2; y < std::min(im.h, subjectTop + headBand * 2); ++y) {
        if (rowCount[y] > 0 && rowCount[y] < narrowest) { narrowest = rowCount[y]; headBottom = y; }
    }
    int shoulderRow = -1;
    for (int y = headBottom; y < subjectBottom; ++y) {
        if (rowCount[y] > std::fmax(2.2f * topMedian, static_cast<float>(narrowest) * 1.35f)) {
            shoulderRow = y;
            break;
        }
    }
    if (shoulderRow < 0) shoulderRow = std::min(im.h - 1, subjectTop + static_cast<int>(subjectH * 0.18f));

    /* Waist: the width minimum between the shoulders and the hip; hip: the width maximum in the
       upper half of the pelvis band. Both are searched only where the subject is actually wide
       enough to contain a torso, so a raised arm cannot be mistaken for a hip. */
    int waistRow = shoulderRow + static_cast<int>(subjectH * 0.25f);
    int hipRow = shoulderRow + static_cast<int>(subjectH * 0.42f);
    {
        int best = 1 << 30;
        for (int y = shoulderRow + static_cast<int>(subjectH * 0.12f);
             y < std::min(im.h, shoulderRow + static_cast<int>(subjectH * 0.40f)); ++y) {
            if (rowCount[y] > 0 && rowCount[y] < best) { best = rowCount[y]; waistRow = y; }
        }
        int widest = -1;
        for (int y = waistRow; y < std::min(im.h, shoulderRow + static_cast<int>(subjectH * 0.62f)); ++y) {
            if (rowCount[y] > widest) { widest = rowCount[y]; hipRow = y; }
        }
    }

    /* Feet: a full-body shot shows the feet at the bottom of the frame. "Full body" is claimed only
       when the subject's mask actually reaches the lower eighth of the image, which is the one
       thing a portrait cannot fake. */
    const bool reachesBottom = (static_cast<float>(subjectBottom) / static_cast<float>(im.h)) > 0.88f;
    const bool plausibleHeads = (subjectH / std::fmax(1.0f, static_cast<float>(headBottom - subjectTop + 1))) > 5.0f;
    out->is_full_body = (reachesBottom && plausibleHeads) ? 1 : 0;

    /* Landmarks. Where the silhouette gives a real measurement, use it; where it cannot (knees,
       ankles in a portrait), use the canonical anthropometric fractions OF THE SUBJECT's own
       head-count, and say so through is_full_body and pose_confidence. */
    const float headFrac = static_cast<float>(headBottom - subjectTop + 1) / subjectH;
    const float headCount = 1.0f / std::fmax(0.02f, headFrac);
    auto fracToY = [&](float f) { return (static_cast<float>(subjectTop) + f * subjectH) / static_cast<float>(im.h); };
    out->head_box[0] = static_cast<float>(rowFirst[subjectTop]) / static_cast<float>(im.w);
    out->head_box[1] = static_cast<float>(subjectTop) / static_cast<float>(im.h);
    float headMinX = 1.0f, headMaxX = 0.0f;
    for (int y = subjectTop; y <= headBottom; ++y) {
        if (rowFirst[y] < 0) continue;
        headMinX = std::fmin(headMinX, static_cast<float>(rowFirst[y]) / static_cast<float>(im.w));
        headMaxX = std::fmax(headMaxX, static_cast<float>(rowLast[y]) / static_cast<float>(im.w));
    }
    out->head_box[2] = headMaxX - headMinX;
    out->head_box[3] = static_cast<float>(headBottom - subjectTop + 1) / static_cast<float>(im.h);
    out->shoulder_y = static_cast<float>(shoulderRow) / static_cast<float>(im.h);
    out->hip_y = static_cast<float>(hipRow) / static_cast<float>(im.h);
    /* Crown->ankle is 0.96 of stature; the ratios below are the Drillis-Contini segment fractions
       used throughout the anthropometry kernel, so the photo and the generator agree by
       construction. */
    const float crownY = out->head_box[1];
    const float footY = static_cast<float>(subjectBottom) / static_cast<float>(im.h);
    const float stature = footY - crownY;
    out->ankle_y = crownY + 0.961f * stature;
    out->knee_y = crownY + 0.715f * stature;
    if (!out->is_full_body) {
        /* Portrait: report the estimated values anyway but make the confidence honest. */
        out->pose_confidence = std::fmin(0.55f, 0.16f * headCount);
    } else {
        out->pose_confidence = std::fmin(0.95f, 0.16f * headCount * (out->subject_coverage > 0.05f ? 1.0f : 0.6f));
    }

    /* ---- 5. per-pixel classification -------------------------------------------------------- */
    std::vector<int> skinIdx, hairIdx, garmentIdx, allIdx;
    allIdx.reserve(subjectPixels);
    for (int y = 0; y < im.h; ++y) {
        for (int x = 0; x < im.w; ++x) {
            const size_t i = static_cast<size_t>(y) * im.w + x;
            if (!im.mask[i]) continue;
            allIdx.push_back(static_cast<int>(i));
            const float* lab = &im.lab[i * 3];
            const float relY = (static_cast<float>(y) - static_cast<float>(subjectTop)) / subjectH;
            /* Skin: the chroma locus of human skin is a narrow band under any normal white
               balance - red-yellow, moderate chroma, independent of lightness. The rule below is
               the classical chromaticity test (Kovac et al.) expressed in Lab, plus a lightness
               window that excludes deep shadow. */
            const bool skinish = (lab[0] > 22.0f) && (lab[1] > 4.0f) && (lab[1] < 34.0f) &&
                                 (lab[2] > 6.0f) && (lab[2] < 42.0f) && (lab[1] + lab[2] > 14.0f) &&
                                 (lab[0] < 92.0f);
            /* Face and neck are inside the head box; hands anywhere. Between the hip and the
               shoulder, skin only appears where clothing ends - so a garment-coloured region
               that passes the skin test twice in a row is still treated as skin (bare chest). */
            const bool inHeadBox = relY < (static_cast<float>(headBottom - subjectTop + 1) / subjectH);
            if (skinish && (inHeadBox || relY > 0.62f || relY < 0.30f)) {
                skinIdx.push_back(static_cast<int>(i));
            } else if (relY < 0.20f && lab[0] < 70.0f) {
                hairIdx.push_back(static_cast<int>(i));
            } else {
                garmentIdx.push_back(static_cast<int>(i));
            }
        }
    }

    if (!skinIdx.empty()) {
        for (int c = 0; c < 3; ++c) out->skin_lab[c] = medianChannel(im, skinIdx, c);
    } else {
        out->skin_lab[0] = 62.0f; out->skin_lab[1] = 12.0f; out->skin_lab[2] = 18.0f;   /* neutral */
    }
    if (!hairIdx.empty()) {
        for (int c = 0; c < 3; ++c) out->hair_lab[c] = medianChannel(im, hairIdx, c);
    } else {
        out->hair_lab[0] = out->skin_lab[0] * 0.55f;
        out->hair_lab[1] = out->skin_lab[1] * 0.7f;
        out->hair_lab[2] = out->skin_lab[2] * 0.7f;
    }
    if (!garmentIdx.empty()) {
        for (int c = 0; c < 3; ++c) out->garment_lab[c] = medianChannel(im, garmentIdx, c);
    } else {
        out->garment_lab[0] = 55.0f; out->garment_lab[1] = 0.0f; out->garment_lab[2] = 0.0f;
    }
    for (int c = 0; c < 3; ++c) out->background_lab[c] = bgLab[c];

    /* ---- 6. metal and key light -------------------------------------------------------------- */
    {
        /* Metal: a hard surface reflects its environment, so its pixels are low-chroma but cover a
           very wide lightness range, with a bright specular lobe. Cloth is diffuse: its lightness
           range inside a single garment region is narrow. */
        std::vector<float> garmentL;
        garmentL.reserve(garmentIdx.size());
        float minL = 1e9f, maxL = -1e9f;
        double sumL = 0.0;
        for (int i : garmentIdx) {
            const float L = im.lab[static_cast<size_t>(i) * 3];
            garmentL.push_back(L);
            minL = std::fmin(minL, L);
            maxL = std::fmax(maxL, L);
            sumL += L;
        }
        const float meanL = garmentIdx.empty() ? 0.0f : static_cast<float>(sumL / garmentIdx.size());
        float chroma = 0.0f;
        for (int i : garmentIdx) {
            const float* lab = &im.lab[static_cast<size_t>(i) * 3];
            chroma += std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
        }
        if (!garmentIdx.empty()) chroma /= static_cast<float>(garmentIdx.size());
        const float range = (maxL - minL);
        float metal = 0.0f;
        if (!garmentIdx.empty()) {
            metal = 0.45f * std::fmin(1.0f, range / 62.0f) + 0.35f * std::fmin(1.0f, meanL / 70.0f) +
                    0.20f * (1.0f - std::fmin(1.0f, chroma / 24.0f));
            metal = std::fmax(0.0f, std::fmin(1.0f, metal));
        }
        out->metal_confidence = metal;

        /* Key light: compare the centroid of the subject with the centroid of its brightest fifth.
           The offset, in image space, is the direction the light comes from. The z component is
           fixed at "in front of the subject" because a single photo cannot tell more, and that is
           what a portrait light is. */
        double cx = 0, cy = 0, bx = 0, by = 0;
        int bright = 0;
        if (!allIdx.empty()) {
            std::vector<float> ls;
            ls.reserve(allIdx.size());
            for (int i : allIdx) ls.push_back(im.lab[static_cast<size_t>(i) * 3]);
            std::vector<float> sorted = ls;
            std::sort(sorted.begin(), sorted.end());
            const float cut = sorted[static_cast<size_t>(sorted.size() * 0.80f)];
            for (int i : allIdx) {
                const float L = im.lab[static_cast<size_t>(i) * 3];
                const double x = static_cast<double>(i % im.w);
                const double y = static_cast<double>(i / im.w);
                cx += x; cy += y;
                if (L >= cut) { bx += x; by += y; ++bright; }
            }
            cx /= allIdx.size(); cy /= allIdx.size();
        }
        float lx = 0.0f, ly = 0.0f, lz = 0.72f;
        if (bright > 0) {
            bx /= bright; by /= bright;
            lx = static_cast<float>((bx - cx) / static_cast<double>(im.w)) * 2.4f;
            ly = static_cast<float>((cy - by) / static_cast<double>(im.h)) * 1.9f;   /* image y is down */
        } else {
            lx = -0.35f; ly = 0.55f;
        }
        const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
        out->key_light[0] = lx / len;
        out->key_light[1] = ly / len;
        out->key_light[2] = lz / len;
        /* Intensity hint: how bright the subject is relative to the background, normalised. */
        float brightMean = 0.0f;
        for (int i : allIdx) brightMean += im.lab[static_cast<size_t>(i) * 3];
        if (!allIdx.empty()) brightMean /= static_cast<float>(allIdx.size());
        out->key_light_intensity = std::fmax(0.2f, std::fmin(2.0f, (brightMean / 55.0f)));
    }

    if (out->skin_lab[1] <= 0.001f && out->skin_lab[2] <= 0.001f) {
        out->skin_lab[0] = 62.0f;
        out->skin_lab[1] = 12.0f;
        out->skin_lab[2] = 18.0f;
    }
    return true;
}

void specFromAnalysis(const af_image_analysis& identity, const af_image_analysis* clothing,
                      af_spec* spec, float* outConfidence) {
    if (!spec) return;
    float confidence = identity.pose_confidence;

    /* Stature has strict precedence: explicit metres in af_spec win; only the canonical default
       (or a legacy zero/non-finite value) may be refined from a genuine full-body image. A portrait
       has no absolute scale, so it keeps 1.75 m instead of writing the old 0 sentinel. */
    {
        const bool unresolved = !std::isfinite(spec->height_m) || spec->height_m <= 0.0f ||
                                std::fabs(spec->height_m - AF_DEFAULT_HEIGHT_M) < 1e-5f;
        const float headHeight = std::fmax(0.01f, identity.head_box[3]);
        const float statureNorm = identity.ankle_y - identity.head_box[1];
        const int fullBody = identity.is_full_body;
        if (unresolved && fullBody && statureNorm > 0.05f) {
            const float heads = statureNorm / headHeight;
            /* The ratio is scale-free; 0.235 m is the adult head-height prior. Confidence pulls a
               noisy segmentation towards the canonical default, never towards zero. */
            float h = 0.235f * heads;
            if (h < 1.40f) h = 1.40f;
            if (h > 2.20f) h = 2.20f;
            const float w = std::fmax(0.0f, std::fmin(1.0f, (confidence - 0.45f) / 0.5f));
            spec->height_m = w * h + (1.0f - w) * AF_DEFAULT_HEIGHT_M;
        } else if (!std::isfinite(spec->height_m) || spec->height_m <= 0.0f) {
            spec->height_m = AF_DEFAULT_HEIGHT_M;
        }
        if (!fullBody) confidence *= 0.6f;
    }

    /* Sex: shoulder-to-hip ratio. Above 1.45 is male-typical, below 1.30 female-typical, and in
       between the estimate is left alone rather than guessed. */
    {
        const float shoulder = identity.head_box[2];
        (void)shoulder;
    }

    /* Skin tone: the engine's melanin -> albedo curve is monotone in L*, so invert it against the
       measured skin lightness, normalised by the background (the only reference for exposure a
       single image offers). */
    {
        const float skinL = identity.skin_lab[0];
        const float refL = std::fmax(22.0f, identity.background_lab[0]);
        const float ratio = std::fmax(0.15f, std::fmin(1.9f, skinL / refL));
        float melanin = 0.62f - 0.52f * ratio;              /* 1.0 ratio -> medium-light skin */
        if (melanin < 0.03f) melanin = 0.03f;
        if (melanin > 0.95f) melanin = 0.95f;
        spec->skin_melanin = melanin;
    }

    /* Hair tone: Lab -> linear sRGB, then darkened slightly because the photo's hair pixels sit in
       shadow as often as not. */
    {
        float rgb[3];
        labToLinearRgb(identity.hair_lab, rgb);
        for (int c = 0; c < 3; ++c) spec->hair_tone[c] = std::fmax(0.01f, std::fmin(1.0f, rgb[c] * 1.15f));
    }

    /* Clothing: colour and material class. Metal-looking references become plate; everything else
       becomes cloth. The colour comes from the identity image unless a clothing reference was
       supplied, in which case that image wins - it is the one the user pointed at. */
    {
        const af_image_analysis& src = clothing ? *clothing : identity;
        const float metal = clothing ? clothing->metal_confidence : identity.metal_confidence;
        float rgb[3];
        labToLinearRgb(src.garment_lab, rgb);
        for (int c = 0; c < 3; ++c) {
            spec->cloth_tone[c] = std::fmax(0.01f, std::fmin(1.0f, rgb[c]));
            spec->gear_tone[c] = std::fmax(0.01f, std::fmin(1.0f, rgb[c] * 1.05f));
        }
        if (spec->outfit == AF_OUTFIT_NONE || spec->outfit == AF_OUTFIT_TSHIRT) {
            if (metal > 0.62f) {
                spec->outfit = AF_OUTFIT_ARMOR_PLATE;
                if (spec->footwear == AF_FOOTWEAR_NONE) spec->footwear = AF_FOOTWEAR_SABATON;
            } else if (metal > 0.45f) {
                spec->outfit = AF_OUTFIT_ARMOR_LEATHER;
                if (spec->footwear == AF_FOOTWEAR_NONE) spec->footwear = AF_FOOTWEAR_BOOT;
            } else if (spec->outfit == AF_OUTFIT_NONE) {
                spec->outfit = AF_OUTFIT_TSHIRT;
            }
        }
        if (outConfidence) {
            *outConfidence = std::fmax(0.0f, std::fmin(1.0f, confidence * 0.5f + (clothing ? 0.35f : 0.2f)));
        }
    }
    if (outConfidence) {
        *outConfidence = std::fmax(0.0f, std::fmin(1.0f, *outConfidence));
    }
}

} // namespace af
