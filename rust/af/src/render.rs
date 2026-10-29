// =============================================================================================
//  af::render -- optional Rust binding for the custom renderer (feature = "renderer")
// =============================================================================================
//
//  The renderer is C++ (native/src/render/) and needs a real Android window plus a GLES 3.1 driver, so this module is off by default: desktop builds and unit tests of the kernel
//  do not link it, and an Android build can turn it on to drive the exact same renderer the
//  Kotlin app uses.
//
//  The C surface it binds to is af_render_capi.cpp; the handle is opaque and single-threaded,
//  exactly like the engine. `Renderer` therefore implements Drop and is not Sync.
// =============================================================================================

use af_sys::af_engine;
use core::ffi::{c_char, c_void, CStr};

use crate::error::{Error, Result};

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(clippy::upper_case_acronyms)]
pub enum Quality {
    Auto = 0,
    Low = 1,
    Medium = 2,
    High = 3,
}

/// Mirror of `af::RendererOptions`. `Default` matches the C defaults, so a caller can tweak a
/// single field.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RenderOptions {
    pub quality: i32,
    pub enable_shadows: i32,
    pub enable_bloom: i32,
    pub enable_sss: i32,
    pub msaa_samples: i32,
    pub shadow_map_size: i32,
    pub bloom_levels: i32,
    pub sun_azimuth_deg: f32,
    pub sun_elevation_deg: f32,
    pub sun_intensity: f32,
    pub turbidity: f32,
    pub ground_albedo: f32,
    pub exposure: f32,
    pub environment_intensity: f32,
    pub shadow_softness: f32,
    pub ground_plane: i32,
}

impl Default for RenderOptions {
    fn default() -> Self {
        let mut options = Self {
            quality: Quality::Auto as i32,
            enable_shadows: 1,
            enable_bloom: 0,
            enable_sss: 1,
            msaa_samples: 4,
            shadow_map_size: 2048,
            bloom_levels: 4,
            sun_azimuth_deg: 42.0,
            sun_elevation_deg: 38.0,
            sun_intensity: 1.5,
            turbidity: 2.6,
            ground_albedo: 0.28,
            exposure: 0.65,
            environment_intensity: 0.65,
            shadow_softness: 1.0,
            ground_plane: 1,
        };
        unsafe { af_render_options_defaults(&mut options) };
        options
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Camera {
    pub yaw_deg: f32,
    pub pitch_deg: f32,
    pub distance: f32,
    pub target_y: f32,
    pub fov_deg: f32,
    pub roll_deg: f32,
}

impl Default for Camera {
    fn default() -> Self {
        Self { yaw_deg: 180.0, pitch_deg: 4.0, distance: 3.5, target_y: 0.95, fov_deg: 32.0, roll_deg: 0.0 }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderInfo {
    pub backend: [c_char; 32],
    pub device_name: [c_char; 128],
    pub api_version: [c_char; 64],
    pub driver: [c_char; 96],
    pub supports_shadows: i32,
    pub supports_msaa: i32,
    pub supports_float_hdr: i32,
    pub supports_anisotropy: i32,
}

impl Default for RenderInfo {
    fn default() -> Self {
        Self {
            backend: [0; 32],
            device_name: [0; 128],
            api_version: [0; 64],
            driver: [0; 96],
            supports_shadows: 0,
            supports_msaa: 0,
            supports_float_hdr: 0,
            supports_anisotropy: 0,
        }
    }
}

impl RenderInfo {
    /// Actual GLES backend identifier reported by the renderer.
    pub fn backend(&self) -> String {
        cstr(&self.backend)
    }

    pub fn device_name(&self) -> String {
        cstr(&self.device_name)
    }

    pub fn api_version(&self) -> String {
        cstr(&self.api_version)
    }

    pub fn driver(&self) -> String {
        cstr(&self.driver)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RenderStats {
    pub frame_ms: f32,
    pub cpu_ms: f32,
    pub fps: f32,
    /// -1 means unavailable; never present as measured GPU time.
    pub gpu_ms: f32,
    pub draw_calls: i32,
    pub triangles: i32,
    pub vertices: i32,
    pub shadow_map_size: i32,
    pub msaa_samples: i32,
    pub bloom_levels: i32,
    pub quality_level: i32,
    pub frames: i32,
    pub frame_attempts: i32,
    pub frame_submissions: i32,
    pub present_requests_accepted: i32,
    pub texture_bytes: u64,
}

impl Default for RenderStats {
    fn default() -> Self {
        Self {
            frame_ms: 0.0, cpu_ms: 0.0, fps: 0.0, gpu_ms: -1.0,
            draw_calls: 0, triangles: 0, vertices: 0, shadow_map_size: 0,
            msaa_samples: 0, bloom_levels: 0, quality_level: 0, frames: 0,
            frame_attempts: 0, frame_submissions: 0, present_requests_accepted: 0,
            texture_bytes: 0,
        }
    }
}

const _: () = assert!(core::mem::size_of::<RenderOptions>() == 64);
const _: () = assert!(core::mem::size_of::<RenderStats>() == 72);
const _: () = assert!(core::mem::size_of::<Camera>() == 24);

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unknown_gpu_time_and_camera_defaults_are_explicit() {
        assert_eq!(RenderStats::default().gpu_ms, -1.0);
        assert_eq!(Camera::default().distance, 3.5);
    }
}

fn cstr<const N: usize>(bytes: &[c_char; N]) -> String {
    let end = bytes.iter().position(|c| *c == 0).unwrap_or(N);
    let raw: Vec<u8> = bytes[..end].iter().map(|c| *c as u8).collect();
    String::from_utf8_lossy(&raw).into_owned()
}

extern "C" {
    fn af_render_options_defaults(out: *mut RenderOptions);

    fn af_renderer_create(native_window: *mut c_void, options: *const RenderOptions) -> *mut c_void;
    fn af_renderer_destroy(handle: *mut c_void);
    fn af_renderer_last_error() -> *const c_char;
    fn af_renderer_resize(handle: *mut c_void, width: i32, height: i32) -> i32;
    fn af_renderer_attach_engine(handle: *mut c_void, engine: *mut af_engine) -> i32;
    fn af_renderer_invalidate_mesh(handle: *mut c_void);
    fn af_renderer_set_camera(handle: *mut c_void, camera: *const Camera);
    fn af_renderer_set_options(handle: *mut c_void, options: *const RenderOptions);
    fn af_renderer_set_overlays(handle: *mut c_void, skeleton: i32, flat: i32, highlight_region: i32);
    fn af_renderer_set_clip(handle: *mut c_void, clip: i32, speed: f32, looped: i32);
    fn af_renderer_current_clip(handle: *mut c_void) -> i32;
    fn af_renderer_clip_time(handle: *mut c_void) -> f32;
    fn af_renderer_frame(handle: *mut c_void, delta_seconds: f32);
    fn af_renderer_wait_idle(handle: *mut c_void);
    fn af_renderer_info(handle: *mut c_void, out: *mut RenderInfo) -> i32;
    fn af_renderer_stats(handle: *mut c_void, out: *mut RenderStats) -> i32;
    fn af_render_pick_region(
        engine: *const af_engine,
        ray_origin: *const f32,
        ray_direction: *const f32,
        out_distance: *mut f32,
        out_point: *mut f32,
    ) -> i32;
    fn af_render_screen_ray(
        camera: *const Camera,
        ndc_x: f32,
        ndc_y: f32,
        width: i32,
        height: i32,
        out_origin: *mut f32,
        out_direction: *mut f32,
    );
}

/// Owns one GLES backend instance for one Android window.
pub struct Renderer {
    handle: *mut c_void,
}

impl Renderer {
    /// `native_window` is an `ANativeWindow*` - on Android, the pointer behind a `Surface`.
    /// Returns the reason string from `Renderer::create` on failure, never a null renderer.
    pub fn create(native_window: *mut c_void, options: RenderOptions) -> Result<Self> {
        if native_window.is_null() {
            return Err(Error::InvalidArgument("native_window is null".into()));
        }
        let handle = unsafe { af_renderer_create(native_window, &options) };
        if handle.is_null() {
            let message = unsafe {
                let ptr = af_renderer_last_error();
                if ptr.is_null() {
                    "renderer creation failed".to_string()
                } else {
                    CStr::from_ptr(ptr).to_string_lossy().into_owned()
                }
            };
            return Err(Error::Unsupported(message));
        }
        Ok(Self { handle })
    }

    pub fn resize(&self, width: i32, height: i32) -> Result<()> {
        let code = unsafe { af_renderer_resize(self.handle, width, height) };
        if code == af_sys::AF_OK {
            Ok(())
        } else {
            Err(Error::Internal(format!("resize failed with code {code}")))
        }
    }

    /// Binds an engine handle, whose lifetime must exceed the renderer's.
    pub fn attach_engine(&self, engine: &crate::Engine) -> Result<()> {
        // SAFETY: the caller guarantees the engine outlives the renderer (documented on
        // Renderer::create and enforced by the app's ownership structure).
        let code = unsafe { af_renderer_attach_engine(self.handle, engine.as_raw_handle()) };
        if code == af_sys::AF_OK {
            Ok(())
        } else {
            Err(Error::Internal(format!("attach_engine failed with code {code}")))
        }
    }

    pub fn invalidate_mesh(&self) {
        unsafe { af_renderer_invalidate_mesh(self.handle) };
    }

    pub fn set_camera(&self, camera: Camera) {
        unsafe { af_renderer_set_camera(self.handle, &camera) };
    }

    pub fn set_options(&self, options: RenderOptions) {
        unsafe { af_renderer_set_options(self.handle, &options) };
    }

    pub fn set_overlays(&self, skeleton: bool, flat_shading: bool, highlight_region: i32) {
        unsafe {
            af_renderer_set_overlays(
                self.handle,
                skeleton as i32,
                flat_shading as i32,
                highlight_region,
            )
        };
    }

    /// Starts a clip. The renderer owns the playback clock from here on.
    pub fn set_clip(&self, clip: i32, speed: f32, looped: bool) {
        unsafe { af_renderer_set_clip(self.handle, clip, speed, looped as i32) };
    }

    pub fn current_clip(&self) -> i32 {
        unsafe { af_renderer_current_clip(self.handle) }
    }

    pub fn clip_time(&self) -> f32 {
        unsafe { af_renderer_clip_time(self.handle) }
    }

    /// Renders and presents one frame. `delta_seconds` feeds the quality controller.
    pub fn frame(&self, delta_seconds: f32) {
        unsafe { af_renderer_frame(self.handle, delta_seconds) };
    }

    pub fn wait_idle(&self) {
        unsafe { af_renderer_wait_idle(self.handle) };
    }

    pub fn info(&self) -> RenderInfo {
        let mut info = RenderInfo::default();
        unsafe { af_renderer_info(self.handle, &mut info) };
        info
    }

    pub fn stats(&self) -> RenderStats {
        let mut stats = RenderStats::default();
        unsafe { af_renderer_stats(self.handle, &mut stats) };
        stats
    }
}

impl Drop for Renderer {
    fn drop(&mut self) {
        unsafe { af_renderer_destroy(self.handle) };
    }
}

/// Converts a tap position into a world-space ray, using the same camera the renderer draws with.
/// `ndc_x`/`ndc_y` are in -1..1 with +Y up (the convention Android's MotionEvent needs flipping
/// into - see the app's touch handler).
pub fn screen_ray(camera: Camera, ndc_x: f32, ndc_y: f32, width: i32, height: i32) -> ([f32; 3], [f32; 3]) {
    let mut origin = [0.0f32; 3];
    let mut direction = [0.0f32; 3];
    unsafe {
        af_render_screen_ray(
            &camera,
            ndc_x,
            ndc_y,
            width,
            height,
            origin.as_mut_ptr(),
            direction.as_mut_ptr(),
        )
    };
    (origin, direction)
}

/// CPU ray-cast against the engine's deformed mesh; returns the `af_region` hit, if any.
pub fn pick_region(engine: &crate::Engine, origin: [f32; 3], direction: [f32; 3]) -> Option<crate::Pick> {
    let mut distance = 0.0f32;
    let mut point = [0.0f32; 3];
    let region = unsafe {
        af_render_pick_region(
            engine.as_raw_handle(),
            origin.as_ptr(),
            direction.as_ptr(),
            &mut distance,
            point.as_mut_ptr(),
        )
    };
    if region < 0 {
        None
    } else {
        Some(crate::Pick { region, distance, point })
    }
}
