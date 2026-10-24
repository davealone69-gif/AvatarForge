// =============================================================================================
//  AvatarForge :: af-sys  --  raw FFI bindings to the kernel's public C ABI (libafcore.so)
// =============================================================================================
//
//  Hand-written from native/include/afcore.h. There is no bindgen step on purpose: the header is
//  the contract, the ABI version is checked at runtime, and a hand-written binding set is
//  reviewable in one sitting. Every struct here mirrors its C counterpart field for field, in
//  order, with the same integer widths, and `const _: () = assert!(...)` blocks below pin the
//  sizes so a layout drift breaks the build instead of corrupting memory at runtime.
//
//  Safety contract (identical to afcore.h):
//    * an `af_engine` handle is used from one thread at a time;
//    * pointers returned by the engine stay valid until the next mutating call on that handle,
//      or until the handle is destroyed - the safe wrapper in the `af` crate encodes this.
// =============================================================================================

#![allow(non_camel_case_types)]
#![allow(clippy::missing_safety_doc)]

use core::ffi::{c_char, c_float, c_int, c_void};

/* ------------------------------------------------------------------ opaque handle -------- */
#[repr(C)]
pub struct af_engine {
    _private: [u8; 0],
}

/* ------------------------------------------------------------------ result codes --------- */
pub type af_result = c_int;

pub const AF_OK: af_result = 0;
pub const AF_ERR_INVALID_ARG: af_result = -1;
pub const AF_ERR_OUT_OF_MEMORY: af_result = -2;
pub const AF_ERR_UNSUPPORTED: af_result = -3;
pub const AF_ERR_NOT_READY: af_result = -4;
pub const AF_ERR_IO: af_result = -5;
pub const AF_ERR_CANCELLED: af_result = -6;
pub const AF_ERR_INTERNAL: af_result = -7;
pub const AF_ERR_ABI_MISMATCH: af_result = -8;
pub const AF_ERR_BUDGET_EXCEEDED: af_result = -9;

/* ------------------------------------------------------------------ stages ---------------- */
pub const AF_STAGE_IDLE: c_int = 0;
pub const AF_STAGE_ANALYZE_INPUTS: c_int = 1;
pub const AF_STAGE_FIT_ANATOMY: c_int = 2;
pub const AF_STAGE_GEOMETRY: c_int = 3;
pub const AF_STAGE_UV_UNWRAP: c_int = 4;
pub const AF_STAGE_BAKE_TEXTURES: c_int = 5;
pub const AF_STAGE_RIG: c_int = 6;
pub const AF_STAGE_BLENDSHAPES: c_int = 7;
pub const AF_STAGE_ANIMATION: c_int = 8;
pub const AF_STAGE_EXPORT: c_int = 9;
pub const AF_STAGE_DONE: c_int = 10;
pub const AF_STAGE_COUNT: c_int = 11;

pub const AF_LOG_DEBUG: c_int = 0;
pub const AF_LOG_INFO: c_int = 1;
pub const AF_LOG_WARN: c_int = 2;
pub const AF_LOG_ERROR: c_int = 3;

pub type af_progress_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, stage: c_int, fraction: c_float, message: *const c_char)>;
pub type af_log_fn = Option<unsafe extern "C" fn(user: *mut c_void, severity: c_int, message: *const c_char)>;
pub type af_cancel_fn = Option<unsafe extern "C" fn(user: *mut c_void) -> c_int>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_callbacks {
    pub user: *mut c_void,
    pub on_progress: af_progress_fn,
    pub on_log: af_log_fn,
    pub should_cancel: af_cancel_fn,
}

impl Default for af_callbacks {
    fn default() -> Self {
        Self { user: core::ptr::null_mut(), on_progress: None, on_log: None, should_cancel: None }
    }
}

/* ------------------------------------------------------------------ specification --------- */
pub const AF_SEX_UNSPECIFIED: c_int = 0;
pub const AF_SEX_MALE: c_int = 1;
pub const AF_SEX_FEMALE: c_int = 2;

pub const AF_BUILD_AVERAGE: c_int = 0;
pub const AF_BUILD_SLIM: c_int = 1;
pub const AF_BUILD_ATHLETIC: c_int = 2;
pub const AF_BUILD_MUSCULAR: c_int = 3;
pub const AF_BUILD_HEAVY: c_int = 4;

pub const AF_HAIR_NONE: c_int = 0;
pub const AF_HAIR_BUZZ: c_int = 1;
pub const AF_HAIR_SHORT: c_int = 2;
pub const AF_HAIR_MEDIUM: c_int = 3;
pub const AF_HAIR_LONG: c_int = 4;
pub const AF_HAIR_PONYTAIL: c_int = 5;
pub const AF_HAIR_BUN: c_int = 6;
pub const AF_HAIR_BRAIDS: c_int = 7;
pub const AF_HAIR_AFRO: c_int = 8;
pub const AF_HAIR_MOHAWK: c_int = 9;

pub const AF_OUTFIT_NONE: c_int = 0;
pub const AF_OUTFIT_TSHIRT: c_int = 1;
pub const AF_OUTFIT_DRESS: c_int = 2;
pub const AF_OUTFIT_SUIT: c_int = 3;
pub const AF_OUTFIT_JEANS_JACKET: c_int = 4;
pub const AF_OUTFIT_ARMOR_PLATE: c_int = 5;
pub const AF_OUTFIT_ARMOR_LEATHER: c_int = 6;
pub const AF_OUTFIT_SPACESUIT: c_int = 7;
pub const AF_OUTFIT_ATHLETIC: c_int = 8;
pub const AF_OUTFIT_COAT: c_int = 9;

pub const AF_FOOTWEAR_NONE: c_int = 0;
pub const AF_FOOTWEAR_SNEAKER: c_int = 1;
pub const AF_FOOTWEAR_BOOT: c_int = 2;
pub const AF_FOOTWEAR_SABATON: c_int = 3;
pub const AF_FOOTWEAR_HEEL: c_int = 4;

pub const AF_PROMPT_BYTES: usize = 512;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_spec {
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

impl Default for af_spec {
    fn default() -> Self {
        // All-zero is *not* a valid spec (0 height means "1.75 m default", but struct_size must be
        // set and several floats use 0 as a sentinel), so the default goes through the kernel.
        unsafe {
            let mut spec = core::mem::MaybeUninit::<af_spec>::zeroed().assume_init();
            af_spec_init(&mut spec);
            spec
        }
    }
}

/* ------------------------------------------------------------------ images ----------------- */
pub const AF_IMAGE_IDENTITY: c_int = 0;
pub const AF_IMAGE_CLOTHING: c_int = 1;
pub const AF_IMAGE_FULL_BODY: c_int = 2;
pub const AF_IMAGE_EXTRA_VIEW: c_int = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_image {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub role: i32,
    pub pixels: *const u8,
}

impl Default for af_image {
    fn default() -> Self {
        Self { width: 0, height: 0, channels: 0, role: AF_IMAGE_IDENTITY, pixels: core::ptr::null() }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct af_image_analysis {
    pub subject_coverage: f32,
    pub head_box: [f32; 4],
    pub shoulder_y: f32,
    pub hip_y: f32,
    pub ankle_y: f32,
    pub knee_y: f32,
    pub is_full_body: i32,
    pub pose_confidence: f32,
    pub skin_lab: [f32; 3],
    pub hair_lab: [f32; 3],
    pub garment_lab: [f32; 3],
    pub metal_confidence: f32,
    pub key_light: [f32; 3],
    pub key_light_intensity: f32,
    pub background_lab: [f32; 3],
}

/* ------------------------------------------------------------------ animation ------------- */
pub const AF_CLIP_REST: c_int = 0;
pub const AF_CLIP_IDLE: c_int = 1;
pub const AF_CLIP_BREATHE: c_int = 2;
pub const AF_CLIP_WALK: c_int = 3;
pub const AF_CLIP_RUN: c_int = 4;
pub const AF_CLIP_JOG: c_int = 5;
pub const AF_CLIP_SQUAT: c_int = 6;
pub const AF_CLIP_JUMP: c_int = 7;
pub const AF_CLIP_WAVE: c_int = 8;
pub const AF_CLIP_PUNCH: c_int = 9;
pub const AF_CLIP_KICK: c_int = 10;
pub const AF_CLIP_T_POSE: c_int = 11;
pub const AF_CLIP_A_POSE: c_int = 12;
pub const AF_CLIP_SIT: c_int = 13;
pub const AF_CLIP_TURN_360: c_int = 14;
pub const AF_CLIP_PUSHUP: c_int = 15;
pub const AF_CLIP_COUNT: c_int = 16;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_pose {
    pub quats: *const f32,
    pub joint_count: i32,
}

/* ------------------------------------------------------------------ rig ------------------- */
pub const AF_JOINT_TWIST: c_int = 1;
pub const AF_JOINT_FINGER: c_int = 2;
pub const AF_JOINT_FACE: c_int = 4;

pub const AF_JOINT_NAME_BYTES: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_joint_info {
    pub name: [c_char; AF_JOINT_NAME_BYTES],
    pub parent: i32,
    pub flags: i32,
    pub rest_translation: [f32; 3],
    pub rest_rotation: [f32; 4],
}

impl Default for af_joint_info {
    fn default() -> Self {
        Self {
            name: [0; AF_JOINT_NAME_BYTES],
            parent: -1,
            flags: 0,
            rest_translation: [0.0; 3],
            rest_rotation: [0.0, 0.0, 0.0, 1.0],
        }
    }
}

/* ------------------------------------------------------------------ geometry -------------- */
/// Interleaved vertex, byte-exact against the C `af_vertex` (88 bytes):
/// `pos[3] normal[3] tangent[4] uv[2] color[4] joints[4]u16 weights[4]`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct af_vertex {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    pub tangent: [f32; 4],
    pub uv: [f32; 2],
    pub color: [f32; 4],
    pub joints: [u16; 4],
    pub weights: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct af_submesh {
    pub region: i32,
    pub vertex_offset: i32,
    pub vertex_count: i32,
    pub index_offset: i32,
    pub index_count: i32,
    pub material_index: i32,
    pub bounds_min: [f32; 3],
    pub bounds_max: [f32; 3],
}

pub const AF_MATERIAL_NAME_BYTES: usize = 48;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_material {
    pub name: [c_char; AF_MATERIAL_NAME_BYTES],
    pub base_color_factor: [f32; 4],
    pub metallic: f32,
    pub roughness: f32,
    pub ior: f32,
    pub sheen: f32,
    pub sheen_tint: f32,
    pub subsurface: f32,
    pub anisotropic: f32,
    pub has_base_color_tex: i32,
    pub has_normal_tex: i32,
    pub has_mr_tex: i32,
    pub has_ao_tex: i32,
}

impl Default for af_material {
    fn default() -> Self {
        Self {
            name: [0; AF_MATERIAL_NAME_BYTES],
            base_color_factor: [1.0, 1.0, 1.0, 1.0],
            metallic: 0.0,
            roughness: 0.5,
            ior: 1.5,
            sheen: 0.0,
            sheen_tint: 0.0,
            subsurface: 0.0,
            anisotropic: 0.0,
            has_base_color_tex: 0,
            has_normal_tex: 0,
            has_mr_tex: 0,
            has_ao_tex: 0,
        }
    }
}

/* ------------------------------------------------------------------ textures -------------- */
pub const AF_TEX_BASE_COLOR: c_int = 0;
pub const AF_TEX_NORMAL: c_int = 1;
pub const AF_TEX_METALLIC_ROUGHNESS: c_int = 2;
pub const AF_TEX_OCCLUSION: c_int = 3;
pub const AF_TEX_SUBSURFACE: c_int = 4;
pub const AF_TEX_COUNT: c_int = 5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_texture {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub mip_levels: i32,
    pub byte_size: u64,
    pub data: *const u8,
}

impl Default for af_texture {
    fn default() -> Self {
        Self {
            width: 0,
            height: 0,
            channels: 0,
            mip_levels: 0,
            byte_size: 0,
            data: core::ptr::null(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::af_texture;

    #[test]
    fn texture_default_is_empty_and_null() {
        let texture = af_texture::default();
        assert_eq!(texture.width, 0);
        assert_eq!(texture.height, 0);
        assert_eq!(texture.channels, 0);
        assert_eq!(texture.mip_levels, 0);
        assert_eq!(texture.byte_size, 0);
        assert!(texture.data.is_null());
    }
}

/* ------------------------------------------------------------------ stats ----------------- */
pub const AF_GPU_HINT_BYTES: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct af_stats {
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
    pub gpu_hint: [c_char; AF_GPU_HINT_BYTES],
}

impl Default for af_stats {
    fn default() -> Self {
        Self {
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
            gpu_hint: [0; AF_GPU_HINT_BYTES],
        }
    }
}

/* ------------------------------------------------------------------ ABI ------------------- */
pub const AF_ABI_VERSION: c_int = 1;

extern "C" {
    pub fn af_engine_abi_version() -> c_int;
    pub fn af_engine_version() -> *const c_char;
    pub fn af_engine_set_memory_budget(engine: *mut af_engine, bytes: u64) -> af_result;
    pub fn af_engine_create(
        spec: *const af_spec,
        callbacks: *const af_callbacks,
        out_engine: *mut *mut af_engine,
    ) -> af_result;
    pub fn af_engine_destroy(engine: *mut af_engine);
    pub fn af_engine_reset(engine: *mut af_engine, spec: *const af_spec) -> af_result;

    pub fn af_spec_init(spec: *mut af_spec);

    pub fn af_engine_analyze_images(
        engine: *mut af_engine,
        images: *const af_image,
        image_count: i32,
        out_analyses: *mut af_image_analysis,
        max_analyses: i32,
    ) -> i32;
    pub fn af_engine_analyze_prompt(
        engine: *mut af_engine,
        prompt: *const c_char,
        out_spec: *mut af_spec,
    ) -> i32;

    pub fn af_engine_fit_anatomy(engine: *mut af_engine) -> af_result;
    pub fn af_engine_build_geometry(engine: *mut af_engine) -> af_result;
    pub fn af_engine_unwrap_uv(engine: *mut af_engine) -> af_result;
    pub fn af_engine_bake_textures(engine: *mut af_engine) -> af_result;
    pub fn af_engine_rig(engine: *mut af_engine) -> af_result;
    pub fn af_engine_bake_blendshapes(engine: *mut af_engine) -> af_result;
    pub fn af_engine_bake_animation(engine: *mut af_engine) -> af_result;
    pub fn af_engine_run_all(engine: *mut af_engine) -> af_result;
    pub fn af_engine_dirty_mask(engine: *mut af_engine) -> i32;

    pub fn af_engine_apply_pose(engine: *mut af_engine, pose: *const af_pose) -> af_result;
    pub fn af_engine_sample_clip(
        engine: *mut af_engine,
        clip: i32,
        time_seconds: f32,
        out_pose: *mut af_pose,
    ) -> af_result;
    pub fn af_engine_apply_clip(engine: *mut af_engine, clip: i32, time_seconds: f32) -> af_result;
    pub fn af_engine_clip_duration(engine: *const af_engine, clip: i32) -> f32;
    pub fn af_engine_clip_name(clip: i32, out_name: *mut c_char, max_bytes: i32) -> i32;

    pub fn af_engine_apply_blendshapes(
        engine: *mut af_engine,
        weights: *const f32,
        count: i32,
    ) -> af_result;
    pub fn af_engine_blendshape_names(names_out: *mut [c_char; 64], max_names: i32) -> i32;

    pub fn af_engine_vertex_count(engine: *const af_engine) -> i32;
    pub fn af_engine_index_count(engine: *const af_engine) -> i32;
    pub fn af_engine_vertices(engine: *const af_engine) -> *const af_vertex;
    pub fn af_engine_vertices_rest(engine: *const af_engine) -> *const af_vertex;
    pub fn af_engine_indices(engine: *const af_engine) -> *const u32;
    pub fn af_engine_submesh_count(engine: *const af_engine) -> i32;
    pub fn af_engine_submeshes(engine: *const af_engine) -> *const af_submesh;
    pub fn af_engine_material_count(engine: *const af_engine) -> i32;
    pub fn af_engine_materials(engine: *const af_engine) -> *const af_material;

    pub fn af_engine_joint_count(engine: *const af_engine) -> i32;
    pub fn af_engine_joint_info(engine: *const af_engine, joint: i32, out: *mut af_joint_info) -> af_result;
    pub fn af_engine_joint_matrices(
        engine: *const af_engine,
        out_matrices4x4: *mut f32,
        max_joints: i32,
    ) -> af_result;

    pub fn af_engine_texture(engine: *const af_engine, slot: i32, out: *mut af_texture) -> af_result;

    pub fn af_engine_export_glb(engine: *mut af_engine, absolute_path: *const c_char) -> af_result;
    pub fn af_engine_import_glb(engine: *mut af_engine, absolute_path: *const c_char) -> af_result;
    pub fn af_engine_import_glb_from_memory(
        engine: *mut af_engine,
        bytes: *const u8,
        size: usize,
    ) -> af_result;
    pub fn af_engine_export_glb_to_memory(
        engine: *mut af_engine,
        out_bytes: *mut *mut u8,
        out_size: *mut usize,
    ) -> af_result;
    pub fn af_arena_free(ptr: *mut c_void);

    pub fn af_engine_last_error(engine: *const af_engine) -> *const c_char;
    pub fn af_engine_region_name(region: i32) -> *const c_char;
    pub fn af_engine_stats(engine: *const af_engine, out: *mut af_stats) -> af_result;
    pub fn af_engine_set_gpu_hint(engine: *mut af_engine, hint: *const c_char) -> af_result;
}

/* ------------------------------------------------------------------ layout assertions ----- */
const _: () = assert!(core::mem::size_of::<af_vertex>() == 88);
const _: () = assert!(core::mem::align_of::<af_vertex>() == 4);
const _: () = assert!(core::mem::size_of::<af_submesh>() == 48);
