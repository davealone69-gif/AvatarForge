/*
 * ============================================================================================
 *  AvatarForge :: afcore.h  --  stable public C ABI of the on-device avatar engine kernel
 * ============================================================================================
 *
 *  This header is the *entire* contract between the C++ kernel (libafcore.so) and every
 *  consumer: the Kotlin/JNI app layer, the Rust `af-sys` crate, and third-party AIDL clients.
 *
 *  Design rules (non-negotiable, enforced by review):
 *   1. No network. This header exposes no socket, HTTP, DNS or IPC-over-network primitive.
 *   2. No hidden allocation ownership: every pointer returned is owned by the engine handle
 *      and stays valid until the next mutation call on that handle or af_engine_destroy().
 *   3. Exceptions never cross the ABI. Every entry point returns af_result.
 *   4. Deterministic: identical af_spec + identical seed  =>  byte-identical geometry.
 *   5. Thread affine: an af_engine handle may be used from exactly one thread at a time.
 *      All heavy work runs on caller-provided worker threads *inside* a single call.
 *
 *  ABI version is checked by af_engine_abi_version(); consumers must refuse to run against
 *  a mismatch rather than risk struct layout drift.
 */
#ifndef AFCORE_H
#define AFCORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_ABI_VERSION 1
/* Canonical stature used whenever the caller does not supply a valid override. All public height
 * fields are metres; centimetres are converted at their input boundary before reaching af_spec. */
#define AF_DEFAULT_HEIGHT_M 1.75f
/* Human-readable kernel version, reported by the SDK health check and written into every export. */
#define AF_ENGINE_VERSION "0.10.1"

/* ------------------------------------------------------------------ opaque handles ------ */
typedef struct af_engine af_engine;

/* ------------------------------------------------------------------ result codes --------- */
typedef enum af_result {
    AF_OK = 0,
    AF_ERR_INVALID_ARG        = -1,
    AF_ERR_OUT_OF_MEMORY      = -2,
    AF_ERR_UNSUPPORTED        = -3,
    AF_ERR_NOT_READY          = -4,   /* a prerequisite stage has not run yet            */
    AF_ERR_IO                 = -5,
    AF_ERR_CANCELLED          = -6,
    AF_ERR_INTERNAL           = -7,
    AF_ERR_ABI_MISMATCH       = -8,
    AF_ERR_BUDGET_EXCEEDED    = -9    /* would exceed the configured memory ceiling      */
} af_result;

/* ------------------------------------------------------------------ stages & progress ---- */
typedef enum af_stage {
    AF_STAGE_IDLE = 0,
    AF_STAGE_ANALYZE_INPUTS = 1,   /* interpret the prompt, analyse reference images -> af_spec   */
    AF_STAGE_FIT_ANATOMY = 2,      /* anthropometric solve -> metric template                     */
    AF_STAGE_GEOMETRY = 3,         /* field sample -> marching tets -> refine -> topology repair
                                      -> UV unwrap is NOT here; cloth, hair and equipment are
                                      built inside this same unified field, which is why there is
                                      no separate garment/hair stage (see af_field.h)              */
    AF_STAGE_UV_UNWRAP = 4,        /* charting + atlas packing                                    */
    AF_STAGE_BAKE_TEXTURES = 5,    /* procedural PBR maps baked in UV space                       */
    AF_STAGE_RIG = 6,              /* skeleton + per-vertex skin weights                          */
    AF_STAGE_BLENDSHAPES = 7,      /* 52 ARKit-compatible facial targets                          */
    AF_STAGE_ANIMATION = 8,        /* clip library sampled and cached for playback                */
    AF_STAGE_EXPORT = 9,           /* GLB written to app-scoped storage                           */
    AF_STAGE_DONE = 10,
    AF_STAGE_COUNT = 11
} af_stage;

typedef enum af_severity { AF_LOG_DEBUG = 0, AF_LOG_INFO = 1, AF_LOG_WARN = 2, AF_LOG_ERROR = 3 } af_severity;

/* Ambient-temperature event sink. Calls happen synchronously on the worker thread. */
typedef struct af_callbacks {
    void* user;
    void (*on_progress)(void* user, int stage, float fraction, const char* message);
    void (*on_log)(void* user, int severity, const char* message);
    /* Return non-zero to request cancellation. Polled at safe points only. */
    int  (*should_cancel)(void* user);
} af_callbacks;

/* ------------------------------------------------------------------ input specification -- */
typedef enum af_sex { AF_SEX_UNSPECIFIED = 0, AF_SEX_MALE = 1, AF_SEX_FEMALE = 2 } af_sex;

typedef enum af_build { AF_BUILD_AVERAGE = 0, AF_BUILD_SLIM = 1, AF_BUILD_ATHLETIC = 2,
                        AF_BUILD_MUSCULAR = 3, AF_BUILD_HEAVY = 4 } af_build;

typedef enum af_hair_style {
    AF_HAIR_NONE = 0, AF_HAIR_BUZZ = 1, AF_HAIR_SHORT = 2, AF_HAIR_MEDIUM = 3,
    AF_HAIR_LONG = 4, AF_HAIR_PONYTAIL = 5, AF_HAIR_BUN = 6, AF_HAIR_BRAIDS = 7,
    AF_HAIR_AFRO = 8, AF_HAIR_MOHAWK = 9
} af_hair_style;

typedef enum af_outfit {
    AF_OUTFIT_NONE = 0,        /* real unclothed base surface; adult-only (age >= 18)          */
    AF_OUTFIT_TSHIRT = 1,
    AF_OUTFIT_DRESS = 2,
    AF_OUTFIT_SUIT = 3,        /* jacket + trousers                                          */
    AF_OUTFIT_JEANS_JACKET = 4,
    AF_OUTFIT_ARMOR_PLATE = 5, /* segmented plate cuirass + pauldrons + greaves + sabatons   */
    AF_OUTFIT_ARMOR_LEATHER = 6,
    AF_OUTFIT_SPACESUIT = 7,
    AF_OUTFIT_ATHLETIC = 8,
    AF_OUTFIT_COAT = 9
} af_outfit;

typedef enum af_footwear { AF_FOOTWEAR_NONE = 0, AF_FOOTWEAR_SNEAKER = 1, AF_FOOTWEAR_BOOT = 2,
                           AF_FOOTWEAR_SABATON = 3, AF_FOOTWEAR_HEEL = 4 } af_footwear;

typedef struct af_spec {
    int32_t   struct_size;      /* = sizeof(af_spec); forwards-compatible struct growth    */
    int32_t   sex;              /* af_sex                                                  */
    int32_t   build;            /* af_build                                                */
    int32_t   hair_style;       /* af_hair_style                                           */
    int32_t   outfit;           /* af_outfit                                               */
    int32_t   footwear;         /* af_footwear                                             */
    int32_t   seed;             /* RNG seed: identical seed => identical avatar            */

    float     height_m;         /* metres, 1.40 .. 2.20; af_spec_init => 1.75              */
    float     age_years;        /* 16 .. 70 (drives skin micro-detail & facial volume)     */
    float     muscle;           /* 0..1 lean mass index                                    */
    float     body_fat;         /* 0..1 (0.10 = shredded, 0.45 = heavy)                    */
    float     bust;             /* 0..1 feminine chest volume                              */

    /* Tape measurements in centimetres. 0 = "derive from height/build"; any supplied
       measurement is honoured exactly (perimeter is preserved to <0.5% - see tests). */
    float     neck_cm, chest_cm, underbust_cm, waist_cm, hip_cm;
    float     thigh_cm, calf_cm, bicep_cm, forearm_cm, wrist_cm;
    float     shoulder_width_cm, inseam_cm, head_circumference_cm;

    float     skin_tone[3];     /* linear-sRGB base melanin-free reflectance, 0 => auto     */
    float     skin_melanin;     /* 0..1                                                     */
    float     hair_tone[3];     /* 0 => derived from skin_melanin                           */
    float     cloth_tone[3];    /* primary garment colour,     0 => neutral grey            */
    float     gear_tone[3];     /* armour / equipment colour,  0 => steel                   */
    float     cloth_roughness;  /* 0..1 override,  <0 => material default                   */
    float     gear_metalness;   /* 0..1 override,  <0 => material default                   */

    /* Text prompt that produced this spec (informational; stored in the GLB as metadata). */
    char      prompt[512];
} af_spec;

void af_spec_init(af_spec* spec);

typedef enum af_image_role {
    /* Names are ABI-stable. They describe caller intent, not capabilities the current classical
       analyser does not have: it samples coarse framing/proportions/colours and does not reconstruct
       a face, copy a garment, or fuse calibrated views. */
    AF_IMAGE_IDENTITY = 0,   /* Image A: primary appearance/proportion reference             */
    AF_IMAGE_CLOTHING = 1,   /* Image B: garment colour/material/outfit-class hint           */
    AF_IMAGE_FULL_BODY = 2,  /* full-body framing hint for stature/proportion analysis        */
    AF_IMAGE_EXTRA_VIEW = 3  /* additional independently analysed view; no geometric fusion   */
} af_image_role;

/* A decoded, upright, sRGB-8 image. The engine never retains the pointer. */
typedef struct af_image {
    int32_t width;
    int32_t height;
    int32_t channels;      /* 3 (RGB) or 4 (RGBA)                                     */
    int32_t role;          /* af_image_role                                            */
    const uint8_t* pixels; /* tightly packed, top-left origin, row stride = w*channels */
} af_image;

/* Number of floats in the JNI/Kotlin wire representation of af_image_analysis. The wire order is
   the struct's semantic field order below, with is_full_body converted to float. Keep this one
   native constant as the packer's source of truth; Kotlin verifies it at runtime before allocating. */
#define AF_IMAGE_ANALYSIS_FLOAT_COUNT 28

/* Analysis report produced by af_engine_analyze_images(). Purely observational. */
typedef struct af_image_analysis {
    float   subject_coverage;      /* fraction of pixels belonging to the subject          */
    float   head_box[4];           /* normalised x,y,w,h of the detected head              */
    float   shoulder_y;            /* normalised vertical position of shoulder line        */
    float   hip_y;
    float   ankle_y;
    float   knee_y;
    int32_t is_full_body;          /* 1 when ankle_y is a real detection, not a guess       */
    float   pose_confidence;
    float   skin_lab[3];           /* median skin colour of the subject, CIE L*a*b*         */
    float   hair_lab[3];
    float   garment_lab[3];
    float   metal_confidence;      /* 0..1 probability the clothing reference is hard-surface */
    float   key_light[3];          /* estimated dominant light direction (unit vector)     */
    float   key_light_intensity;
    float   background_lab[3];
} af_image_analysis;

/* ------------------------------------------------------------------ animation ------------ */
typedef enum af_clip {
    AF_CLIP_REST = 0, AF_CLIP_IDLE, AF_CLIP_BREATHE, AF_CLIP_WALK, AF_CLIP_RUN,
    AF_CLIP_JOG, AF_CLIP_SQUAT, AF_CLIP_JUMP, AF_CLIP_WAVE, AF_CLIP_PUNCH,
    AF_CLIP_KICK, AF_CLIP_T_POSE, AF_CLIP_A_POSE, AF_CLIP_SIT, AF_CLIP_TURN_360,
    AF_CLIP_PUSHUP,          /* appended: keeps every previously published id stable */
    AF_CLIP_COUNT
} af_clip;

/* A hierarchical pose sample: local rotation per joint, quaternion XYZW. */
typedef struct af_pose {
    const float* quats;   /* joint_count * 4                                             */
    int32_t      joint_count;
} af_pose;

/* ------------------------------------------------------------------ rig introspection ---- */
typedef enum af_joint_flags { AF_JOINT_TWIST = 1, AF_JOINT_FINGER = 2, AF_JOINT_FACE = 4 } af_joint_flags;

typedef struct af_joint_info {
    char    name[64];
    int32_t parent;        /* -1 = root                                                     */
    int32_t flags;         /* af_joint_flags                                               */
    float   rest_translation[3];
    float   rest_rotation[4];
} af_joint_info;

/* ------------------------------------------------------------------ engine lifetime ------ */
int32_t af_engine_abi_version(void);

/* Configures the memory ceiling. Default is 1536 MB, which is comfortably inside the
   per-app heap of every Android 8+ device with >=4 GB RAM. Field grids, textures and
   intermediate buffers are all allocated against this budget; exceeding it fails with
   AF_ERR_BUDGET_EXCEEDED rather than triggering an OOM kill. */
int32_t af_engine_set_memory_budget(af_engine* engine, uint64_t bytes);

int32_t af_engine_create(const af_spec* spec, const af_callbacks* callbacks, af_engine** out_engine);
void    af_engine_destroy(af_engine* engine);
int32_t af_engine_reset(af_engine* engine, const af_spec* spec);

/* ------------------------------------------------------------------ pipeline stages ----- */
/* Each stage is callable independently; the orchestrator af_engine_run_all() chains them
   in order. Stage calls are idempotent: re-running a stage rebuilds its outputs and
   invalidates downstream ones (reported via af_engine_dirty_mask()). */

int32_t af_engine_analyze_images(af_engine* engine, const af_image* images, int32_t image_count,
                                 af_image_analysis* out_analyses, int32_t max_analyses);

/* Interprets a text prompt. Deterministic keyword/measurement parsing, not a language model:
   the matched attributes are reported through the callbacks so the UI can show what was
   understood rather than guessing. Returns the number of attributes recognised (>= 0). */
int32_t af_engine_analyze_prompt(af_engine* engine, const char* prompt, af_spec* out_spec);

int32_t af_engine_fit_anatomy(af_engine* engine);
int32_t af_engine_build_geometry(af_engine* engine);   /* field, extraction, topology, cloth, hair */
int32_t af_engine_unwrap_uv(af_engine* engine);
int32_t af_engine_bake_textures(af_engine* engine);
int32_t af_engine_rig(af_engine* engine);              /* skeleton + skin weights                  */
int32_t af_engine_bake_blendshapes(af_engine* engine);
int32_t af_engine_bake_animation(af_engine* engine);
int32_t af_engine_run_all(af_engine* engine);
int32_t af_engine_dirty_mask(af_engine* engine);

/* Posing / skinning - evaluates the skeleton on the CPU and writes the deformed vertex
   buffer that the renderer uploads directly. Returns AF_ERR_NOT_READY before rigging. */
int32_t af_engine_apply_pose(af_engine* engine, const af_pose* pose);
int32_t af_engine_sample_clip(af_engine* engine, int32_t clip, float time_seconds, af_pose* out_pose);
int32_t af_engine_apply_clip(af_engine* engine, int32_t clip, float time_seconds);

/* Clip metadata for the UI's animation picker. Both are pure introspection; duration reads only
   the immutable built-in clip table, so it may be queried with a null engine handle while a
   renderer owns the live engine lease. `af_engine_clip_duration` returns 0 for AF_CLIP_REST (a
   static pose, not a timed clip) and for ids outside the built-in library. `af_engine_clip_name`
   writes the canonical authored name
   ("walk", "t_pose", ...) and returns its length, or 0 if the id is unknown. */
float   af_engine_clip_duration(const af_engine* engine, int32_t clip);
int32_t af_engine_clip_name(int32_t clip, char* out_name, int32_t max_bytes);

/* Facial animation: ARKit-compatible blendshape weights, 52 named channels, 0..1. */
int32_t af_engine_apply_blendshapes(af_engine* engine, const float* weights, int32_t count);
int32_t af_engine_blendshape_names(char names_out[][64], int32_t max_names);

/* ------------------------------------------------------------------ geometry access ----- */
/* Vertex layout, interleaved, for direct upload to a Vulkan/GLES vertex buffer:
 *   float pos[3]; float normal[3]; float tangent[4]; float uv[2]; float color[4];
 *   uint16 joints[4]; float weights[4];          => 88 bytes per vertex (byte-exact, see af_vertex below)
 */
typedef struct af_vertex {
    float   pos[3];
    float   normal[3];
    float   tangent[4];   /* xyz + handedness sign in w                                */
    float   uv[2];
    float   color[4];     /* base colour multiplier, linear                             */
    uint16_t joints[4];
    float   weights[4];
} af_vertex;

typedef struct af_submesh {
    int32_t         region;         /* af_region                                        */
    int32_t         vertex_offset;  /* index into the engine vertex buffer              */
    int32_t         vertex_count;
    int32_t         index_offset;   /* index into the engine index buffer               */
    int32_t         index_count;
    int32_t         material_index; /* into af_material table                           */
    float           bounds_min[3];
    float           bounds_max[3];
} af_submesh;

typedef struct af_material {
    char  name[48];
    float base_color_factor[4];
    float metallic;
    float roughness;
    float ior;
    float sheen;
    float sheen_tint;
    float subsurface;
    float anisotropic;
    int32_t has_base_color_tex;
    int32_t has_normal_tex;
    int32_t has_mr_tex;
    int32_t has_ao_tex;
} af_material;

int32_t af_engine_vertex_count(const af_engine* engine);
int32_t af_engine_index_count(const af_engine* engine);
const af_vertex*  af_engine_vertices(const af_engine* engine);
const uint32_t*   af_engine_indices(const af_engine* engine);
int32_t af_engine_submesh_count(const af_engine* engine);
const af_submesh* af_engine_submeshes(const af_engine* engine);
int32_t af_engine_material_count(const af_engine* engine);
const af_material* af_engine_materials(const af_engine* engine);

/* Rest-pose (bind) copies, needed for skinning on the GPU and for export. */
const af_vertex* af_engine_vertices_rest(const af_engine* engine);

/* ------------------------------------------------------------------ skeleton access ----- */
int32_t af_engine_joint_count(const af_engine* engine);
int32_t af_engine_joint_info(const af_engine* engine, int32_t joint, af_joint_info* out);
int32_t af_engine_joint_matrices(const af_engine* engine, float* out_matrices4x4, int32_t max_joints);

/* ------------------------------------------------------------------ textures ------------- */
typedef struct af_texture {
    int32_t  width, height;
    int32_t  channels;
    int32_t  mip_levels;     /* 1 = none; higher mips are appended after level 0          */
    uint64_t byte_size;
    const uint8_t* data;     /* mip 0 first, then successively halved levels              */
} af_texture;

typedef enum af_texture_slot {
    AF_TEX_BASE_COLOR = 0, AF_TEX_NORMAL, AF_TEX_METALLIC_ROUGHNESS,
    AF_TEX_OCCLUSION, AF_TEX_SUBSURFACE, AF_TEX_COUNT
} af_texture_slot;

int32_t af_engine_texture(const af_engine* engine, int32_t slot, af_texture* out);

/* ------------------------------------------------------------------ export -------------- */
/* Writes a self-contained binary glTF 2.0 (.glb): mesh, skeleton, skin, ARKit blendshape
   targets as morph targets, materials, PBR textures, animation clips, and an extras block
   with the originating prompt + spec + image analyses. No external file references. */
int32_t af_engine_export_glb(af_engine* engine, const char* absolute_path);
int32_t af_engine_export_glb_to_memory(af_engine* engine, uint8_t** out_bytes, size_t* out_size);
void    af_arena_free(void* ptr);

/* ------------------------------------------------------------------ import --------------- */
/*
 * Loads a binary glTF 2.0 produced by this engine - or by any writer that stays inside the
 * supported subset - and replaces the engine's renderable geometry with it.
 *
 * WHAT IS APPLIED: vertex data (position, normal, tangent, uv, colour, four joint indices and
 * weights), index ranges as submeshes, materials (base colour, metallic, roughness) and the base
 * colour texture when the file embeds a decodable PNG. Degenerate or hostile files are rejected
 * with an error and leave the engine exactly as it was: the import is built beside the live state
 * and swapped in only once it is complete.
 *
 * WHAT IS NOT APPLIED, deliberately and visibly: the file's skin, joint hierarchy and animation
 * clips. Posing runs on the engine's own rig, which is built from the anthropometric solve; an
 * imported file cannot silently half-adopt a foreign skeleton, because a figure that looks rigged
 * and does not animate is exactly the kind of plausible fake this project forbids. When the file
 * does carry a skeleton or clips, the import succeeds, an AF_LOG_WARN names what was skipped, and
 * the engine's joint count reads 0 until it is re-rigged.
 *
 * Returns AF_OK, or AF_ERR_INVALID_ARG / AF_ERR_UNSUPPORTED / AF_ERR_IO / AF_ERR_OUT_OF_MEMORY.
 * `af_engine_last_error` carries the reader's specific reason.
 */
int32_t af_engine_import_glb(af_engine* engine, const char* absolute_path);
int32_t af_engine_import_glb_from_memory(af_engine* engine, const uint8_t* bytes, size_t size);

/* ------------------------------------------------------------------ diagnostics ---------- */
/* Last failure recorded on the handle ("" when the last call succeeded) and the engine version.
   Both are needed by an SDK that reports health without throwing. */
const char* af_engine_last_error(const af_engine* engine);
const char* af_engine_version(void);
/* Human-readable name of an af_region value, for UI labels. */
const char* af_engine_region_name(int32_t region);

/* Statistics for the UI: triangle count, vertex count, texture memory, stage timings (ms). */
typedef struct af_stats {
    int32_t vertex_count, index_count, triangle_count, submesh_count, joint_count, blendshape_count;
    uint64_t texture_bytes, geometry_bytes, peak_field_bytes;
    float fit_anatomy_ms, field_ms, isosurface_ms, garment_ms, hair_ms, uv_ms, texture_ms, rig_ms, export_ms;
    char gpu_hint[64];      /* "vulkan" | "gles31" | "software" - decided by the app layer */
} af_stats;

int32_t af_engine_stats(const af_engine* engine, af_stats* out);

/* The renderer runs in the app layer (it needs the Android window), so it reports the backend it
   actually initialised: "vulkan", "gles31" or "software". Purely informational - the engine never
   reads it - but it is what the SDK's health check reports to the user. */
int32_t af_engine_set_gpu_hint(af_engine* engine, const char* hint);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AFCORE_H */
