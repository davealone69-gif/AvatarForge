/*
 * af_image_pack.h -- checked wire encoding for af_image_analysis.
 *
 * Android/JNI cannot safely expose a C struct directly because Kotlin has no compatible struct
 * layout and C alignment is platform-dependent. The bridge therefore transfers a flat float
 * record. This helper is deliberately platform-neutral so the desktop harness tests the exact
 * writer used by JNI, including its guard behaviour and all three background-Lab components.
 */
#ifndef AF_IMAGE_PACK_H
#define AF_IMAGE_PACK_H

#include "../include/afcore.h"

namespace af {

constexpr int kImageAnalysisFloatCount = AF_IMAGE_ANALYSIS_FLOAT_COUNT;
static_assert(kImageAnalysisFloatCount == 28,
              "af_image_analysis wire schema changed: update the packer and Kotlin codec together");

/**
 * Packs one analysis in the documented wire order.
 *
 * Returns the number of floats written, or 0 without writing anything when `out` is null or the
 * capacity is too small. Callers can therefore put guard values around the destination in tests and
 * can never partially overwrite an undersized Java array.
 */
inline int packImageAnalysis(const af_image_analysis& a, float* out, int capacity) {
    if (!out || capacity < kImageAnalysisFloatCount) return 0;

    int k = 0;
    out[k++] = a.subject_coverage;
    for (int j = 0; j < 4; ++j) out[k++] = a.head_box[j];
    out[k++] = a.shoulder_y;
    out[k++] = a.hip_y;
    out[k++] = a.ankle_y;
    out[k++] = a.knee_y;
    out[k++] = static_cast<float>(a.is_full_body);
    out[k++] = a.pose_confidence;
    for (int j = 0; j < 3; ++j) out[k++] = a.skin_lab[j];
    for (int j = 0; j < 3; ++j) out[k++] = a.hair_lab[j];
    for (int j = 0; j < 3; ++j) out[k++] = a.garment_lab[j];
    out[k++] = a.metal_confidence;
    for (int j = 0; j < 3; ++j) out[k++] = a.key_light[j];
    out[k++] = a.key_light_intensity;
    for (int j = 0; j < 3; ++j) out[k++] = a.background_lab[j];

    return k;
}

} // namespace af

#endif /* AF_IMAGE_PACK_H */
