// =============================================================================================
//  af::Engine -- RAII wrapper over the kernel handle
// =============================================================================================
//
//  Ownership model:
//
//      Engine  owns  af_engine  owns  the vertex/index/texture buffers
//
//  The engine exposes those buffers as borrowed slices, so the borrow checker enforces the ABI's
//  rule - "a pointer stays valid until the next mutating call" - at compile time. That is the
//  whole reason this crate exists rather than everyone calling af-sys directly.
//
//  Threading: `Engine` is deliberately neither `Send` nor `Sync` (it contains a raw pointer),
//  which matches afcore.h's thread-affine contract: one thread at a time. Move a whole Engine to
//  another thread before using it there; the compiler will demand it.
// =============================================================================================

use af_sys as sys;
use core::ffi::{c_char, CStr};
use std::ffi::CString;

use crate::error::{Error, Result};
use crate::job::{CallbackBridge, JobHandle, Progress, Status};
use crate::spec::Spec;

/// The eleven pipeline stages, in order. `Stage::ALL` is what a progress list in a UI iterates.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stage {
    AnalyzeInputs = 1,
    FitAnatomy = 2,
    Geometry = 3,
    UvUnwrap = 4,
    BakeTextures = 5,
    Rig = 6,
    Blendshapes = 7,
    Animation = 8,
    Export = 9,
}

impl Stage {
    pub const ALL: [Stage; 8] = [
        Stage::AnalyzeInputs,
        Stage::FitAnatomy,
        Stage::Geometry,
        Stage::UvUnwrap,
        Stage::BakeTextures,
        Stage::Rig,
        Stage::Blendshapes,
        Stage::Animation,
    ];

    pub fn name(self) -> &'static str {
        crate::job::stage_name(self as i32)
    }
}

/// One decoded reference image. The bytes must outlive the call (the kernel never retains them,
/// but it does read them synchronously).
pub struct ImageRef<'a> {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub role: i32,
    pub pixels: &'a [u8],
}

impl<'a> ImageRef<'a> {
    /// Image A of the two-image flow: identity, upper body, the engine infers the lower body.
    pub fn identity(width: i32, height: i32, channels: i32, pixels: &'a [u8]) -> Self {
        Self { width, height, channels, role: sys::AF_IMAGE_IDENTITY, pixels }
    }

    /// Image B: a garment/armour reference to be transferred onto the body.
    pub fn clothing(width: i32, height: i32, channels: i32, pixels: &'a [u8]) -> Self {
        Self { width, height, channels, role: sys::AF_IMAGE_CLOTHING, pixels }
    }

    /// An already full-body photograph (no outpainting needed).
    pub fn full_body(width: i32, height: i32, channels: i32, pixels: &'a [u8]) -> Self {
        Self { width, height, channels, role: sys::AF_IMAGE_FULL_BODY, pixels }
    }

    fn as_raw(&self) -> Result<sys::af_image> {
        if self.width <= 0 || self.height <= 0 {
            return Err(Error::InvalidArgument("image has no pixels".into()));
        }
        if self.channels != 3 && self.channels != 4 {
            return Err(Error::InvalidArgument(format!(
                "image channels must be 3 or 4, got {}",
                self.channels
            )));
        }
        let expected = (self.width as usize) * (self.height as usize) * (self.channels as usize);
        if self.pixels.len() < expected {
            return Err(Error::InvalidArgument(format!(
                "image buffer holds {} bytes, {expected} are required for {}x{}x{}",
                self.pixels.len(),
                self.width,
                self.height,
                self.channels
            )));
        }
        Ok(sys::af_image {
            width: self.width,
            height: self.height,
            channels: self.channels,
            role: self.role,
            pixels: self.pixels.as_ptr(),
        })
    }
}

/// What a ray hit: which region of the body, how far away, and where.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Pick {
    pub region: i32,
    pub distance: f32,
    pub point: [f32; 3],
}

/// Borrowed view of one engine texture.
pub struct TextureView<'a> {
    pub width: i32,
    pub height: i32,
    pub channels: i32,
    pub mip_levels: i32,
    pub data: &'a [u8],
}

pub struct EngineBuilder {
    spec: Spec,
    memory_budget: u64,
    keep_logs: usize,
}

impl EngineBuilder {
    pub fn new(spec: Spec) -> Self {
        Self { spec, memory_budget: 1536 * 1024 * 1024, keep_logs: 256 }
    }

    /// Peak-RAM ceiling. The kernel allocates field grids, textures and scratch buffers against
    /// this number and fails with `Error::BudgetExceeded` rather than being OOM-killed.
    pub fn memory_budget(mut self, bytes: u64) -> Self {
        self.memory_budget = bytes;
        self
    }

    /// How many log lines the job snapshot keeps for the UI (older lines are dropped).
    pub fn keep_logs(mut self, count: usize) -> Self {
        self.keep_logs = count;
        self
    }

    pub fn build(self) -> Result<Engine> {
        self.spec.validate()?;

        // Fail before creating anything if the loaded library is a different ABI than these
        // bindings were compiled against: the struct layouts below would silently disagree.
        let abi = unsafe { sys::af_engine_abi_version() };
        if abi != sys::AF_ABI_VERSION {
            return Err(Error::AbiMismatch { expected: sys::AF_ABI_VERSION, found: abi });
        }

        let mut bridge = CallbackBridge::new(self.keep_logs);
        let mut handle: *mut sys::af_engine = core::ptr::null_mut();
        let code = unsafe { sys::af_engine_create(self.spec.as_raw(), &bridge.table, &mut handle) };
        if code != sys::AF_OK || handle.is_null() {
            // The bridge's user-data blob must be released on this path too.
            unsafe { bridge.release() };
            return Err(error_from_code(code, "af_engine_create"));
        }
        let code = unsafe { sys::af_engine_set_memory_budget(handle, self.memory_budget) };
        if code != sys::AF_OK {
            let error = error_from_code(code, "af_engine_set_memory_budget");
            unsafe {
                sys::af_engine_destroy(handle);
                bridge.release();
            }
            return Err(error);
        }

        Ok(Engine { handle, bridge, spec: self.spec, last_status: Status::Idle })
    }
}

pub struct Engine {
    handle: *mut sys::af_engine,
    bridge: CallbackBridge,
    spec: Spec,
    last_status: Status,
}

impl Engine {
    /// Creates an engine with default memory budget and log depth.
    pub fn new(spec: Spec) -> Result<Self> {
        EngineBuilder::new(spec).build()
    }

    pub fn builder(spec: Spec) -> EngineBuilder {
        EngineBuilder::new(spec)
    }

    pub fn spec(&self) -> &Spec {
        &self.spec
    }

    /// Raw handle for the optional renderer binding. The renderer API requires the caller to keep
    /// this engine alive and avoid mutating it while attached.
    #[cfg(feature = "renderer")]
    pub(crate) fn as_raw_handle(&self) -> *mut sys::af_engine {
        self.handle
    }

    /// A cheap, cloneable progress/cancel handle for this engine.
    pub fn job(&self) -> JobHandle {
        JobHandle { state: std::sync::Arc::clone(&self.bridge.state) }
    }

    /// Requests cancellation of the running stage. The kernel polls it at safe points, so this
    /// returns immediately and the job reports `Status::Cancelled` once it has stopped.
    pub fn cancel(&self) {
        self.bridge.request_cancel();
    }

    /// Status of the most recent operation driven through this wrapper.
    pub fn status(&self) -> Status {
        self.last_status
    }

    pub fn progress(&self) -> Progress {
        self.bridge.state.snapshot()
    }

    /// The kernel's message for the last failure ("" when the last call succeeded).
    pub fn last_error(&self) -> String {
        let ptr = unsafe { sys::af_engine_last_error(self.handle) };
        if ptr.is_null() {
            return String::new();
        }
        unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
    }

    pub fn version() -> &'static str {
        let ptr = unsafe { sys::af_engine_version() };
        if ptr.is_null() {
            return "unknown";
        }
        unsafe { CStr::from_ptr(ptr) }.to_str().unwrap_or("unknown")
    }

    pub fn region_name(region: i32) -> String {
        let ptr = unsafe { sys::af_engine_region_name(region) };
        if ptr.is_null() {
            return format!("region {region}");
        }
        unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned()
    }

    /* ---------------------------------------------------------------- driving the pipeline --- */

    /// Runs every stage in order. This is what the app's "Generate" button calls; stages are also
    /// individually callable for incremental re-generation (change the hair, rebuild geometry,
    /// re-export - without re-rigging).
    pub fn run_all(&mut self) -> Result<()> {
        self.bridge.clear_cancel();
        self.bridge.begin();
        let code = unsafe { sys::af_engine_run_all(self.handle) };
        self.finish(code, "af_engine_run_all")
    }

    pub fn run_stage(&mut self, stage: Stage) -> Result<()> {
        self.bridge.begin();
        let code = match stage {
            Stage::AnalyzeInputs => {
                // Nothing to do without a prompt or an image; the typed entry points are
                // analyze_prompt() / analyze_images(). Report it instead of silently succeeding.
                self.bridge.finish(Status::Failed);
                return Err(Error::InvalidArgument(
                    "analyze_inputs needs a prompt or an image; call analyze_prompt()".into(),
                ));
            }
            Stage::FitAnatomy => unsafe { sys::af_engine_fit_anatomy(self.handle) },
            Stage::Geometry => unsafe { sys::af_engine_build_geometry(self.handle) },
            Stage::UvUnwrap => unsafe { sys::af_engine_unwrap_uv(self.handle) },
            Stage::BakeTextures => unsafe { sys::af_engine_bake_textures(self.handle) },
            Stage::Rig => unsafe { sys::af_engine_rig(self.handle) },
            Stage::Blendshapes => unsafe { sys::af_engine_bake_blendshapes(self.handle) },
            Stage::Animation => unsafe { sys::af_engine_bake_animation(self.handle) },
            Stage::Export => unsafe { sys::af_engine_run_all(self.handle) },
        };
        self.finish(code, stage.name())
    }

    /// Which outputs are stale relative to the current spec (bitmask, see `DirtyMask`).
    pub fn dirty_mask(&self) -> i32 {
        unsafe { sys::af_engine_dirty_mask(self.handle) }
    }

    /// Reports which GPU backend the app's renderer actually initialised. Informational: it ends
    /// up in `stats().gpu_hint` and in the export's extras block.
    pub fn set_gpu_hint(&mut self, hint: &str) -> Result<()> {
        let text = CString::new(hint).map_err(|_| Error::InvalidArgument("hint contains a NUL".into()))?;
        let code = unsafe { sys::af_engine_set_gpu_hint(self.handle, text.as_ptr()) };
        self.check(code, "af_engine_set_gpu_hint")
    }

    /* ---------------------------------------------------------------- inputs ----------------- */

    /// Parses a text prompt into a spec. Returns the spec and the number of attributes the
    /// parser recognised, so the UI can show exactly what was understood.
    pub fn analyze_prompt(&mut self, prompt: &str) -> Result<(Spec, i32)> {
        let text = CString::new(prompt).map_err(|_| Error::InvalidArgument("prompt contains a NUL".into()))?;
        let mut raw: sys::af_spec = unsafe { core::mem::zeroed() };
        unsafe { sys::af_spec_init(&mut raw) };
        let matched = unsafe { sys::af_engine_analyze_prompt(self.handle, text.as_ptr(), &mut raw) };
        if matched < 0 {
            return Err(error_from_code(matched, "af_engine_analyze_prompt"));
        }
        Ok((Spec::from_raw(raw), matched))
    }

    /// Analyses one or more reference images (A = identity, B = clothing/equipment). Purely
    /// observational: it returns what the kernel measured, and the app decides what to do with it.
    pub fn analyze_images(&mut self, images: &[ImageRef<'_>]) -> Result<Vec<sys::af_image_analysis>> {
        if images.is_empty() {
            return Err(Error::InvalidArgument("analyze_images needs at least one image".into()));
        }
        let mut raws = Vec::with_capacity(images.len());
        for image in images {
            raws.push(image.as_raw()?);
        }
        let mut analyses = vec![sys::af_image_analysis::default(); raws.len()];
        let count = unsafe {
            sys::af_engine_analyze_images(
                self.handle,
                raws.as_ptr(),
                raws.len() as i32,
                analyses.as_mut_ptr(),
                analyses.len() as i32,
            )
        };
        if count < 0 {
            return Err(error_from_code(count, "af_engine_analyze_images"));
        }
        analyses.truncate(count as usize);
        Ok(analyses)
    }

    /* ---------------------------------------------------------------- geometry views --------- */

    /// Deformed (posed) vertex stream - exactly what the renderer uploads.
    pub fn vertices(&self) -> &[sys::af_vertex] {
        let count = unsafe { sys::af_engine_vertex_count(self.handle) }.max(0) as usize;
        let ptr = unsafe { sys::af_engine_vertices(self.handle) };
        if ptr.is_null() || count == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(ptr, count) }
    }

    /// Bind-pose vertex stream (before any pose is applied).
    pub fn vertices_rest(&self) -> &[sys::af_vertex] {
        let count = unsafe { sys::af_engine_vertex_count(self.handle) }.max(0) as usize;
        let ptr = unsafe { sys::af_engine_vertices_rest(self.handle) };
        if ptr.is_null() || count == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(ptr, count) }
    }

    pub fn indices(&self) -> &[u32] {
        let count = unsafe { sys::af_engine_index_count(self.handle) }.max(0) as usize;
        let ptr = unsafe { sys::af_engine_indices(self.handle) };
        if ptr.is_null() || count == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(ptr, count) }
    }

    /// One entry per material island. `submesh.region` is the anatomical label
    /// (`Engine::region_name`) and `submesh.index_offset/count` slice `indices()`.
    pub fn submeshes(&self) -> &[sys::af_submesh] {
        let count = unsafe { sys::af_engine_submesh_count(self.handle) }.max(0) as usize;
        let ptr = unsafe { sys::af_engine_submeshes(self.handle) };
        if ptr.is_null() || count == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(ptr, count) }
    }

    pub fn materials(&self) -> &[sys::af_material] {
        let count = unsafe { sys::af_engine_material_count(self.handle) }.max(0) as usize;
        let ptr = unsafe { sys::af_engine_materials(self.handle) };
        if ptr.is_null() || count == 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(ptr, count) }
    }

    /// Material name as a Rust string (the C field is a fixed char array).
    pub fn material_name(&self, material: &sys::af_material) -> String {
        let bytes: Vec<u8> = material.name.iter().take_while(|c| **c != 0).map(|c| *c as u8).collect();
        String::from_utf8_lossy(&bytes).into_owned()
    }

    pub fn texture(&self, slot: i32) -> Result<TextureView<'_>> {
        let mut tex = sys::af_texture::default();
        let code = unsafe { sys::af_engine_texture(self.handle, slot, &mut tex) };
        if code != sys::AF_OK {
            return Err(error_from_code(code, "af_engine_texture"));
        }
        let len = tex.byte_size as usize;
        let data = if tex.data.is_null() || len == 0 {
            &[][..]
        } else {
            unsafe { core::slice::from_raw_parts(tex.data, len) }
        };
        Ok(TextureView {
            width: tex.width,
            height: tex.height,
            channels: tex.channels,
            mip_levels: tex.mip_levels,
            data,
        })
    }

    /* ---------------------------------------------------------------- rig and animation ------ */

    pub fn joint_count(&self) -> i32 {
        unsafe { sys::af_engine_joint_count(self.handle) }
    }

    pub fn joint_info(&self, joint: i32) -> Result<sys::af_joint_info> {
        let mut info = sys::af_joint_info::default();
        let code = unsafe { sys::af_engine_joint_info(self.handle, joint, &mut info) };
        if code != sys::AF_OK {
            return Err(error_from_code(code, "af_engine_joint_info"));
        }
        Ok(info)
    }

    /// Joint name as a Rust string.
    pub fn joint_name(info: &sys::af_joint_info) -> String {
        unsafe { CStr::from_ptr(info.name.as_ptr()) }.to_string_lossy().into_owned()
    }

    /// World-space joint matrices (column-major 4x4 each), what a GPU skinning path would upload.
    pub fn joint_matrices(&self) -> Result<Vec<f32>> {
        let joints = self.joint_count().max(0) as usize;
        let mut out = vec![0.0f32; joints * 16];
        let code = unsafe { sys::af_engine_joint_matrices(self.handle, out.as_mut_ptr(), joints as i32) };
        if code != sys::AF_OK {
            return Err(error_from_code(code, "af_engine_joint_matrices"));
        }
        Ok(out)
    }

    /// Applies an explicit local-rotation pose (4 floats per joint, quaternion XYZW).
    pub fn apply_pose(&mut self, quats: &[f32]) -> Result<()> {
        if quats.len() % 4 != 0 {
            return Err(Error::InvalidArgument("pose must be a whole number of quaternions".into()));
        }
        let pose = sys::af_pose { quats: quats.as_ptr(), joint_count: (quats.len() / 4) as i32 };
        let code = unsafe { sys::af_engine_apply_pose(self.handle, &pose) };
        self.check(code, "af_engine_apply_pose")
    }

    /// Samples one of the 15 authored clips at an absolute time, without touching the vertex
    /// buffer: useful for baking the pose into another animation system.
    pub fn sample_clip(&self, clip: i32, time_seconds: f32) -> Result<Vec<f32>> {
        let joints = self.joint_count().max(0) as usize;
        if joints == 0 {
            return Err(Error::NotReady("the avatar is not rigged yet".into()));
        }
        let mut quats = vec![0.0f32; joints * 4];
        let mut pose = sys::af_pose { quats: quats.as_mut_ptr(), joint_count: joints as i32 };
        let code = unsafe { sys::af_engine_sample_clip(self.handle, clip, time_seconds, &mut pose) };
        if code != sys::AF_OK {
            return Err(error_from_code(code, "af_engine_sample_clip"));
        }
        Ok(quats)
    }

    /// Plays a clip by deforming the engine's vertex buffer. `AF_CLIP_REST` (0) restores the bind
    /// pose. The renderer's own clock calls this once per frame; a headless pipeline calls it in a
    /// loop and reads `vertices()`.
    pub fn apply_clip(&mut self, clip: i32, time_seconds: f32) -> Result<()> {
        let code = unsafe { sys::af_engine_apply_clip(self.handle, clip, time_seconds) };
        self.check(code, "af_engine_apply_clip")
    }

    /// Seconds of playback for a clip; 0 for `AF_CLIP_REST` and for unknown ids.
    pub fn clip_duration(&self, clip: i32) -> f32 {
        unsafe { sys::af_engine_clip_duration(self.handle, clip) }
    }

    pub fn clip_name(clip: i32) -> String {
        let mut buffer = [0 as c_char; 64];
        let written = unsafe { sys::af_engine_clip_name(clip, buffer.as_mut_ptr(), buffer.len() as i32) };
        if written <= 0 {
            return "rest".to_string();
        }
        unsafe { CStr::from_ptr(buffer.as_ptr()) }.to_string_lossy().into_owned()
    }

    /// The 52 ARKit-compatible blendshape channels, in the order `apply_blendshapes` expects.
    pub fn blendshape_names() -> Vec<String> {
        let mut names = [[0 as c_char; 64]; 64];
        let count = unsafe { sys::af_engine_blendshape_names(names.as_mut_ptr(), names.len() as i32) };
        (0..count.max(0) as usize)
            .map(|i| unsafe { CStr::from_ptr(names[i].as_ptr()) }.to_string_lossy().into_owned())
            .collect()
    }

    /// Weights are 0..1 per channel, in `blendshape_names()` order; extra channels are ignored by
    /// the kernel, missing ones stay at 0.
    pub fn apply_blendshapes(&mut self, weights: &[f32]) -> Result<()> {
        let code = unsafe {
            sys::af_engine_apply_blendshapes(self.handle, weights.as_ptr(), weights.len() as i32)
        };
        self.check(code, "af_engine_apply_blendshapes")
    }

    /* ---------------------------------------------------------------- export ----------------- */

    /// Writes a self-contained .glb (mesh + skin + morph targets + clips + textures) to a path.
    /// On Android the path must be inside the app's scoped storage.
    pub fn export_glb(&mut self, path: &str) -> Result<()> {
        let text = CString::new(path).map_err(|_| Error::InvalidArgument("path contains a NUL".into()))?;
        let code = unsafe { sys::af_engine_export_glb(self.handle, text.as_ptr()) };
        self.check(code, "af_engine_export_glb")
    }

    /// Same export, returned in memory: what a ContentProvider/FileProvider or a unit test wants.
    /// The kernel allocates in its own arena; this copies into a Vec and frees the arena.
    pub fn export_glb_bytes(&mut self) -> Result<Vec<u8>> {
        let mut bytes: *mut u8 = core::ptr::null_mut();
        let mut size: usize = 0;
        let code = unsafe { sys::af_engine_export_glb_to_memory(self.handle, &mut bytes, &mut size) };
        if code != sys::AF_OK || bytes.is_null() {
            return Err(error_from_code(code, "af_engine_export_glb_to_memory"));
        }
        let out = unsafe { core::slice::from_raw_parts(bytes, size) }.to_vec();
        unsafe { sys::af_arena_free(bytes as *mut core::ffi::c_void) };
        Ok(out)
    }

    pub fn stats(&self) -> Result<sys::af_stats> {
        let mut stats = sys::af_stats::default();
        let code = unsafe { sys::af_engine_stats(self.handle, &mut stats) };
        if code != sys::AF_OK {
            return Err(error_from_code(code, "af_engine_stats"));
        }
        Ok(stats)
    }

    /* ---------------------------------------------------------------- picking ---------------- */

    /// Ray-casts the deformed mesh. The renderer uses the same algorithm through
    /// `af_render_pick_region`, so a tap gives identical answers in Kotlin and in Rust.
    pub fn pick_region(&self, origin: [f32; 3], direction: [f32; 3]) -> Option<Pick> {
        let vertices = self.vertices();
        let indices = self.indices();
        if vertices.is_empty() || indices.len() < 3 {
            return None;
        }
        let normalized = {
            let length = (direction[0] * direction[0] + direction[1] * direction[1]
                + direction[2] * direction[2])
                .sqrt();
            if length <= 0.0 {
                return None;
            }
            [direction[0] / length, direction[1] / length, direction[2] / length]
        };

        let mut best = f32::INFINITY;
        let mut best_region = -1i32;
        for submesh in self.submeshes() {
            let start = (submesh.index_offset / 3).max(0) as usize;
            let count = (submesh.index_count / 3).max(0) as usize;
            for triangle in start..start + count {
                let base = triangle * 3;
                if base + 2 >= indices.len() {
                    break;
                }
                let p0 = vertices[indices[base] as usize].pos;
                let p1 = vertices[indices[base + 1] as usize].pos;
                let p2 = vertices[indices[base + 2] as usize].pos;
                if let Some(distance) = ray_triangle(origin, normalized, p0, p1, p2) {
                    if distance < best {
                        best = distance;
                        best_region = submesh.region;
                    }
                }
            }
        }
        if best_region < 0 {
            return None;
        }
        Some(Pick {
            region: best_region,
            distance: best,
            point: [
                origin[0] + normalized[0] * best,
                origin[1] + normalized[1] * best,
                origin[2] + normalized[2] * best,
            ],
        })
    }

    /* ---------------------------------------------------------------- internals -------------- */

    fn check(&mut self, code: i32, what: &str) -> Result<()> {
        if code == sys::AF_OK {
            self.last_status = Status::Succeeded;
            return Ok(());
        }
        self.last_status = if code == sys::AF_ERR_CANCELLED { Status::Cancelled } else { Status::Failed };
        Err(error_from_code(code, what))
    }

    fn finish(&mut self, code: i32, what: &str) -> Result<()> {
        let result = self.check(code, what);
        let status = self.last_status;
        self.bridge.finish(status);
        result.map_err(|error| {
            let _ = what;
            error
        })
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        unsafe {
            // Order matters: the handle goes first, so no callback can fire after this point,
            // then the callback user-data blob is released.
            sys::af_engine_destroy(self.handle);
            self.bridge.release();
        }
    }
}

fn error_from_code(code: i32, what: &str) -> Error {
    Error::from_code(code, what)
}

/// Möller-Trumbore, double sided (the renderer culls back faces for shading, but a pick should
/// still hit an eyeball's inner surface rather than passing through it).
fn ray_triangle(origin: [f32; 3], direction: [f32; 3], a: [f32; 3], b: [f32; 3], c: [f32; 3]) -> Option<f32> {
    let edge1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    let edge2 = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    let pvec = [
        direction[1] * edge2[2] - direction[2] * edge2[1],
        direction[2] * edge2[0] - direction[0] * edge2[2],
        direction[0] * edge2[1] - direction[1] * edge2[0],
    ];
    let det = edge1[0] * pvec[0] + edge1[1] * pvec[1] + edge1[2] * pvec[2];
    if det.abs() < 1e-12 {
        return None;
    }
    let inv = 1.0 / det;
    let tvec = [origin[0] - a[0], origin[1] - a[1], origin[2] - a[2]];
    let u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inv;
    if !(-1e-6..=1.0 + 1e-6).contains(&u) {
        return None;
    }
    let qvec = [
        tvec[1] * edge1[2] - tvec[2] * edge1[1],
        tvec[2] * edge1[0] - tvec[0] * edge1[2],
        tvec[0] * edge1[1] - tvec[1] * edge1[0],
    ];
    let v = (direction[0] * qvec[0] + direction[1] * qvec[1] + direction[2] * qvec[2]) * inv;
    if v < -1e-6 || u + v > 1.0 + 1e-6 {
        return None;
    }
    let t = (edge2[0] * qvec[0] + edge2[1] * qvec[1] + edge2[2] * qvec[2]) * inv;
    if t > 1e-5 {
        Some(t)
    } else {
        None
    }
}
