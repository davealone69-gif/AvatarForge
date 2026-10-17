// ==============================================================================================
//  af-core :: abi.rs  --  the C ABI as Rust types
// ==============================================================================================
//
//  A hand-written mirror of `native/include/afcore.h`. Hand-written rather than generated on
//  purpose: the layouts here are the contract, and a build-time generator that silently follows a
//  changed header would hide exactly the change a human needs to review. `abi/afcore.abi.json`
//  records what the C compiler actually produced (sizes and field offsets), and
//  `tools/abi_conformance.py` fails if this file ever drifts from it.
//
//  Every struct is `#[repr(C)]` and every field keeps the C name and order. `af_spec` is 672
//  bytes: 7 x i32, then 33 x f32, then the 512-byte prompt, with no padding anywhere because all
//  members are 4-byte aligned and the struct itself is 4-byte aligned (asserted in the tests).
// ==============================================================================================

use core::ffi::{c_char, c_void};

/* ------------------------------------------------------------------ result codes ---------- */
pub const AF_OK: i32 = 0;
pub const AF_ERR_INVALID_ARG: i32 = -1;
pub const AF_ERR_OUT_OF_MEMORY: i32 = -2;
pub const AF_ERR_UNSUPPORTED: i32 = -3;
pub const AF_ERR_NOT_READY: i32 = -4;
pub const AF_ERR_IO: i32 = -5;
pub const AF_ERR_CANCELLED: i32 = -6;
pub const AF_ERR_INTERNAL: i32 = -7;
pub const AF_ERR_ABI_MISMATCH: i32 = -8;
pub const AF_ERR_BUDGET_EXCEEDED: i32 = -9;

/* ------------------------------------------------------------------ stages ---------------- */
pub const AF_STAGE_IDLE: i32 = 0;
pub const AF_STAGE_ANALYZE_INPUTS: i32 = 1;
pub const AF_STAGE_FIT_ANATOMY: i32 = 2;
pub const AF_STAGE_GEOMETRY: i32 = 3;
pub const AF_STAGE_UV_UNWRAP: i32 = 4;
pub const AF_STAGE_BAKE_TEXTURES: i32 = 5;
pub const AF_STAGE_RIG: i32 = 6;
pub const AF_STAGE_BLENDSHAPES: i32 = 7;
pub const AF_STAGE_ANIMATION: i32 = 8;
pub const AF_STAGE_EXPORT: i32 = 9;
pub const AF_STAGE_DONE: i32 = 10;
pub const AF_STAGE_COUNT: i32 = 11;
pub const AF_LOG_DEBUG: i32 = 0;
pub const AF_LOG_INFO: i32 = 1;
pub const AF_LOG_WARN: i32 = 2;
pub const AF_LOG_ERROR: i32 = 3;

/* ------------------------------------------------------------------ spec enums ------------ */
pub const AF_SEX_UNSPECIFIED: i32 = 0;
pub const AF_SEX_MALE: i32 = 1;
pub const AF_SEX_FEMALE: i32 = 2;

pub const AF_BUILD_AVERAGE: i32 = 0;
pub const AF_BUILD_SLIM: i32 = 1;
pub const AF_BUILD_ATHLETIC: i32 = 2;
pub const AF_BUILD_MUSCULAR: i32 = 3;
pub const AF_BUILD_HEAVY: i32 = 4;

pub const AF_HAIR_NONE: i32 = 0;
pub const AF_HAIR_BUZZ: i32 = 1;
pub const AF_HAIR_SHORT: i32 = 2;
pub const AF_HAIR_MEDIUM: i32 = 3;
pub const AF_HAIR_LONG: i32 = 4;
pub const AF_HAIR_PONYTAIL: i32 = 5;
pub const AF_HAIR_BUN: i32 = 6;
pub const AF_HAIR_BRAIDS: i32 = 7;
pub const AF_HAIR_AFRO: i32 = 8;
pub const AF_HAIR_MOHAWK: i32 = 9;

pub const AF_OUTFIT_NONE: i32 = 0;
pub const AF_OUTFIT_TSHIRT: i32 = 1;
pub const AF_OUTFIT_DRESS: i32 = 2;
pub const AF_OUTFIT_SUIT: i32 = 3;
pub const AF_OUTFIT_JEANS_JACKET: i32 = 4;
pub const AF_OUTFIT_ARMOR_PLATE: i32 = 5;
pub const AF_OUTFIT_ARMOR_LEATHER: i32 = 6;
pub const AF_OUTFIT_SPACESUIT: i32 = 7;
pub const AF_OUTFIT_ATHLETIC: i32 = 8;
pub const AF_OUTFIT_COAT: i32 = 9;

pub const AF_FOOTWEAR_NONE: i32 = 0;
pub const AF_FOOTWEAR_SNEAKER: i32 = 1;
pub const AF_FOOTWEAR_BOOT: i32 = 2;
pub const AF_FOOTWEAR_SABATON: i32 = 3;
pub const AF_FOOTWEAR_HEEL: i32 = 4;

pub const AF_IMAGE_IDENTITY: i32 = 0;
pub const AF_IMAGE_CLOTHING: i32 = 1;
pub const AF_IMAGE_FULL_BODY: i32 = 2;
pub const AF_IMAGE_EXTRA_VIEW: i32 = 3;

/* ------------------------------------------------------------------ clip ids -------------- */
pub const AF_CLIP_REST: i32 = 0;
pub const AF_CLIP_IDLE: i32 = 1;
pub const AF_CLIP_BREATHE: i32 = 2;
pub const AF_CLIP_WALK: i32 = 3;
pub const AF_CLIP_RUN: i32 = 4;
pub const AF_CLIP_JOG: i32 = 5;
pub const AF_CLIP_SQUAT: i32 = 6;
pub const AF_CLIP_JUMP: i32 = 7;
pub const AF_CLIP_WAVE: i32 = 8;
pub const AF_CLIP_PUNCH: i32 = 9;
pub const AF_CLIP_KICK: i32 = 10;
pub const AF_CLIP_T_POSE: i32 = 11;
pub const AF_CLIP_A_POSE: i32 = 12;
pub const AF_CLIP_SIT: i32 = 13;
pub const AF_CLIP_TURN_360: i32 = 14;
pub const AF_CLIP_PUSHUP: i32 = 15;
pub const AF_CLIP_COUNT: i32 = 16;

/* ------------------------------------------------------------------ regions ---------------- */
/* Kept as a plain table: the C++ kernel derives it from the material table, and the two are
   compared by tools/abi_conformance.py, which is a stronger check than a shared enum. */
pub const AF_REGION_COUNT: usize = 13;

pub const AF_ABI_VERSION: i32 = 1;
pub const AF_ENGINE_VERSION: &str = "0.10.0";

/* ------------------------------------------------------------------ structs ---------------- */
pub const AF_PROMPT_BYTES: usize = 512;
pub const AF_JOINT_NAME_BYTES: usize = 64;

/// The generation request. Field order and padding are part of the ABI; see the module comment.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AfSpec {
    pub struct_size: i32,
    pub sex: i32,
    pub build: i32,
    pub hair_style: i32,
    pub outfit: i32,
    pub footwear: i32,
    pub seed: i32,
    pub height_m: f32,
    pub age_years: f32,
    pub muscle: f32,
    pub body_fat: f32,
    pub bust: f32,
    pub neck_cm: f32,
    pub chest_cm: f32,
    pub underbust_cm: f32,
    pub waist_cm: f32,
    pub hip_cm: f32,
    pub thigh_cm: f32,
    pub calf_cm: f32,
    pub bicep_cm: f32,
    pub forearm_cm: f32,
    pub wrist_cm: f32,
    pub shoulder_width_cm: f32,
    pub inseam_cm: f32,
    pub head_circumference_cm: f32,
    pub skin_tone: [f32; 3],
    pub skin_melanin: f32,
    pub hair_tone: [f32; 3],
    pub cloth_tone: [f32; 3],
    pub gear_tone: [f32; 3],
    pub cloth_roughness: f32,
    pub gear_metalness: f32,
    pub prompt: [c_char; AF_PROMPT_BYTES],
}

/// Progress/log/cancel hooks. All three are optional (null function pointers).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AfCallbacks {
    pub user: *mut c_void,
    pub on_progress: Option<unsafe extern "C" fn(*mut c_void, i32, f32, *const c_char)>,
    pub on_log: Option<unsafe extern "C" fn(*mut c_void, i32, *const c_char)>,
    pub should_cancel: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
}

/// One vertex, 88 bytes, interleaved for direct GPU upload (asserted in the tests).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AfVertex {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    pub tangent: [f32; 4],
    pub uv: [f32; 2],
    pub color: [f32; 4],
    pub joints: [u16; 4],
    pub weights: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AfStats {
    pub vertex_count: i32,
    pub index_count: i32,
    pub triangle_count: i32,
    pub submesh_count: i32,
    pub joint_count: i32,
    pub blendshape_count: i32,
    pub texture_bytes: u64,
    pub geometry_bytes: u64,
    pub peak_field_bytes: u64,
    pub fit_anatomy_ms: f32,
    pub field_ms: f32,
    pub isosurface_ms: f32,
    pub garment_ms: f32,
    pub hair_ms: f32,
    pub uv_ms: f32,
    pub texture_ms: f32,
    pub rig_ms: f32,
    pub export_ms: f32,
    pub gpu_hint: [c_char; 64],
}

impl AfStats {
    pub const fn zeroed() -> Self {
        AfStats {
            vertex_count: 0,
            index_count: 0,
            triangle_count: 0,
            submesh_count: 0,
            joint_count: 0,
            blendshape_count: 0,
            texture_bytes: 0,
            geometry_bytes: 0,
            peak_field_bytes: 0,
            fit_anatomy_ms: 0.0,
            field_ms: 0.0,
            isosurface_ms: 0.0,
            garment_ms: 0.0,
            hair_ms: 0.0,
            uv_ms: 0.0,
            texture_ms: 0.0,
            rig_ms: 0.0,
            export_ms: 0.0,
            gpu_hint: [0; 64],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{align_of, size_of};

    /* These are the numbers `abi/afcore.abi.json` records from the C compiler. If one of them
       changes, the manifest and this test both have to be updated in the same commit - which is
       exactly the review this project wants on an ABI change. */
    #[test]
    fn struct_layouts_match_the_c_manifest() {
        assert_eq!(size_of::<AfSpec>(), 672);
        assert_eq!(align_of::<AfSpec>(), 4);
        assert_eq!(size_of::<AfVertex>(), 88);
        assert_eq!(size_of::<AfStats>(), 152);
        assert_eq!(size_of::<AfCallbacks>(), 32);
    }

    #[test]
    fn spec_field_offsets_match_the_c_manifest() {
        /* `offset_of!` is a compile-time computation, so this needs no dangling pointer (the
           earlier `(*null).field` version aborted the test process in a debug build, which is
           exactly the sort of UB this API can do without). Numbers from abi/afcore.abi.json. */
        assert_eq!(core::mem::offset_of!(AfSpec, seed), 24);
        assert_eq!(core::mem::offset_of!(AfSpec, height_m), 28);
        assert_eq!(core::mem::offset_of!(AfSpec, waist_cm), 60);
        assert_eq!(core::mem::offset_of!(AfSpec, prompt), 160);
        assert_eq!(core::mem::offset_of!(AfVertex, joints), 64);
        assert_eq!(core::mem::offset_of!(AfVertex, weights), 72);
    }
}
