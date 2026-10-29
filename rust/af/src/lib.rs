// =============================================================================================
//  af  --  the safe Rust face of the AvatarForge engine
// =============================================================================================
//
//  ```no_run
//  use af::{Engine, Spec, Sex, Outfit, HairStyle, Stage};
//
//  let spec = Spec::new()
//      .sex(Sex::Female)
//      .height_m(1.72)
//      .hair(HairStyle::Ponytail)
//      .outfit(Outfit::ArmorPlate)
//      .prompt("1.72 m woman in plate armour, ponytail");
//
//  let mut engine = Engine::new(spec)?;
//  engine.run_all()?;                       // anatomy -> mesh -> UV -> bake -> rig -> anim
//  engine.apply_clip(af_sys::AF_CLIP_WALK, 1.25)?;
//  engine.export_glb("/sdcard/Download/avatar.glb")?;
//  # Ok::<(), af::Error>(())
//  ```
//
//  What this crate is *not*: a wrapper around someone else's avatar library, a client for a
//  service, or a stub. Every call below the surface ends in the C++ kernel that does the work
//  on-device (see native/). It also has no external crates, so it builds offline - which is the
//  point of the whole application.
//
//  Modules
//  -------
//      error   typed failure codes + messages
//      spec    the avatar specification (typed, with a builder)
//      engine  the kernel handle: pipeline, mesh access, rig, animation, export, picking
//      job     progress/cancellation plumbing shared with the Kotlin SDK's JobHandle
//      render  optional binding to the custom renderer (feature = "renderer")
// =============================================================================================

#![deny(unsafe_op_in_unsafe_fn)]

pub mod error;
pub mod job;
pub mod spec;

mod engine;

#[cfg(feature = "renderer")]
pub mod render;

pub use engine::{Engine, EngineBuilder, ImageRef, Pick, Stage, TextureView};
pub use error::{Error, Result};
pub use job::{stage_name, JobHandle, LogLine, Progress, Status};
pub use spec::{Build, Footwear, HairStyle, Outfit, Sex, Spec};

/// Re-export of the raw bindings, for callers that need a C pointer (e.g. to hand the engine
/// handle to the renderer through JNI) or a symbolic constant such as `AF_CLIP_WALK`.
pub use af_sys;

/// Kernel ABI version these bindings were generated for. `Engine::new` refuses to run against a
/// loaded library that reports anything else.
pub const ABI_VERSION: i32 = af_sys::AF_ABI_VERSION;

/// Version string of the kernel currently loaded, e.g. "0.10.1".
pub fn kernel_version() -> &'static str {
    Engine::version()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stage_span_matches_stage_count() {
        assert_eq!(job::STAGE_SPAN as i32, af_sys::AF_STAGE_DONE);
    }

    #[test]
    fn clip_names_round_trip_without_an_engine() {
        // Pure introspection - no handle needed, which is exactly why the app can draw the
        // animation picker before the first generation finishes.
        assert_eq!(Engine::clip_name(af_sys::AF_CLIP_REST), "rest");
        assert_eq!(Engine::clip_name(af_sys::AF_CLIP_WALK), "walk");
        assert_eq!(Engine::clip_name(af_sys::AF_CLIP_T_POSE), "t_pose");
        assert_eq!(Engine::clip_name(9999), "rest");
    }

    #[test]
    fn blendshape_names_are_the_arkit_set() {
        let names = Engine::blendshape_names();
        assert_eq!(names.len(), 52, "expected the 52-channel ARKit set");
        assert!(names.iter().any(|n| n == "eyeBlinkLeft"));
        assert!(names.iter().any(|n| n == "jawOpen"));
    }

    #[test]
    fn region_names_are_available_for_the_ui() {
        assert_eq!(Engine::region_name(0), "skin");
        assert!(!Engine::region_name(-1).is_empty());
    }
}
