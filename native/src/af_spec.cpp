/*
 * af_spec.cpp -- default initialisation for the public C structs.
 *
 * Kept separate from af_engine.cpp so that the host test harness and the AIDL/SDK layers can
 * link the spec initialiser without dragging in the whole engine.
 */
#include "../include/afcore.h"
#include <cstring>

extern "C" {

void af_spec_init(af_spec* spec) {
    if (!spec) return;
    std::memset(spec, 0, sizeof(af_spec));
    spec->struct_size   = static_cast<int32_t>(sizeof(af_spec));
    spec->sex           = AF_SEX_UNSPECIFIED;
    spec->build         = AF_BUILD_AVERAGE;
    spec->hair_style    = AF_HAIR_MEDIUM;
    spec->outfit        = AF_OUTFIT_TSHIRT;
    spec->footwear      = AF_FOOTWEAR_NONE;
    spec->seed          = 1337;

    /* Height is resolved here, at the root of every C/JNI/Rust request. Keeping a zero sentinel
       in the public value made the UI report 0.00 m and allowed later stages to disagree about
       whether they had received a stature. Legacy callers may still pass zero explicitly; the
       engine normalises that to this same value before fitting anatomy. */
    spec->height_m      = AF_DEFAULT_HEIGHT_M;
    spec->age_years     = 30.0f;
    spec->muscle        = 0.45f;
    spec->body_fat      = 0.22f;
    spec->bust          = 0.45f;

    spec->neck_cm = spec->chest_cm = spec->underbust_cm = spec->waist_cm = 0.0f;
    spec->hip_cm = spec->thigh_cm = spec->calf_cm = spec->bicep_cm = 0.0f;
    spec->forearm_cm = spec->wrist_cm = spec->shoulder_width_cm = 0.0f;
    spec->inseam_cm = spec->head_circumference_cm = 0.0f;

    /* Colour overrides are "auto" when zero (or negative for the scalars). */
    spec->skin_tone[0] = spec->skin_tone[1] = spec->skin_tone[2] = 0.0f;
    spec->skin_melanin = 0.35f;
    spec->hair_tone[0] = spec->hair_tone[1] = spec->hair_tone[2] = 0.0f;
    spec->cloth_tone[0] = spec->cloth_tone[1] = spec->cloth_tone[2] = 0.0f;
    spec->gear_tone[0] = spec->gear_tone[1] = spec->gear_tone[2] = 0.0f;
    spec->cloth_roughness = -1.0f;
    spec->gear_metalness = -1.0f;
    spec->prompt[0] = '\0';
}

} /* extern "C" */
