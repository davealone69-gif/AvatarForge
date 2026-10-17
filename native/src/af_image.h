/*
 * af_image.h -- reference-image analysis.
 *
 * This is the on-device half of "generate from a photo". There is no neural network here and there
 * is not going to be one: a segmentation model large enough to be worth shipping would blow the
 * APK budget, need a GPU delegate that half the target devices lack, and make the result
 * non-reproducible. What the generator actually needs from a reference photo is a small set of
 * robust measurements - is this a full-body shot or just a portrait, how many heads tall is the
 * subject, what is the skin tone, what colour is the hair and the clothing, is the clothing cloth
 * or metal, and where is the key light. Every one of those is answerable with classical
 * segmentation on the colour channels, and the answers are auditable: `af_image_analysis` carries
 * the confidence of each estimate, and the app is expected to show them and let the user correct
 * them.
 *
 * The pipeline, per image:
 *   1. box-downsample to a working grid (<= 256 px on the long edge) - O(1) per pixel beyond that;
 *   2. background model from the border ring (median + spread), foreground = colour distance;
 *   3. subject mask cleanup: per-row runs, largest connected component, hole fill;
 *   4. vertical width profile -> head box, shoulder/hip/knee/ankle rows, head-count, full-body flag;
 *   5. per-pixel classification into skin / hair / garment by chromaticity + position;
 *   6. median CIE L*a*b* per class, melanin inversion, metal and key-light estimates.
 *
 * Everything is deterministic: the same bytes in produce the same analysis out, bit for bit.
 */
#ifndef AF_IMAGE_H
#define AF_IMAGE_H

#include "../include/afcore.h"

namespace af {

/** Working resolution cap for analysis (the long edge). */
constexpr int kAnalysisMaxSide = 256;

/**
 * Analyses one decoded image. `out` is filled with the report described in afcore.h.
 * Returns false only for a null/empty/garbage image (the caller reports that as a bad input, not
 * as a failed generation).
 */
bool analyseImage(const af_image& image, af_image_analysis* out);

/**
 * Turns an analysis pair (primary appearance/proportion hint plus optional garment/material hint)
 * into the coarse spec the procedural generator will build. The ABI-stable parameter names remain
 * `identity` and `clothing`, but this function performs no face reconstruction or garment transfer.
 *
 * The height estimate is the interesting one: an image carries no absolute scale, so stature comes
 * from the subject's own proportions - a human is 7.4 heads tall on average and a head is about
 * 0.235 m, so height is read off the head-to-body ratio in the frame. That is scale-free and works
 * on any camera, which is exactly what a phone has.
 */
void specFromAnalysis(const af_image_analysis& identity, const af_image_analysis* clothing,
                      af_spec* spec, float* outConfidence);

} // namespace af
#endif /* AF_IMAGE_H */
