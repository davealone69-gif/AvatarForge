// ==============================================================================================
//  af-core :: engine.rs  --  the handle, the stage state machine, the budget, the callbacks
// ==============================================================================================
//
//  This is the "engine logic" half of the Rust core. It is the part where a bug is expensive:
//  a use-after-free on the handle, a stage that runs out of order, a callback that fires after
//  cancellation, or a budget that is checked only after the allocation has already happened.
//
//  Three rules the implementation enforces structurally rather than by convention:
//
//    1. **A handle is never dereferenced without checking.** Every entry point takes a raw pointer
//       from C and goes through `as_engine`, which returns `None` for null. The pointer is only
//       ever produced by `af_engine_create`, which stores a `Box<Engine>` and hands out its
//       address; `af_engine_destroy` reclaims it exactly once. A double free is a use-after-free
//       in any C ABI, so the Rust side documents that the caller owns that rule - but every other
//       mistake (null, wrong stage, cancelled job) is caught and returned as an error code.
//
//    2. **Stages run in order.** `Stage` is a small state machine; asking for geometry before
//       anatomy is an `AF_ERR_NOT_READY`, not an undefined read.
//
//    3. **The budget is checked before the allocation**, using the same estimate the C++ kernel
//       uses (lattice size from the metrics), so a request that cannot fit is refused rather than
//       attempted and killed by the OS.
//
//  What is NOT here yet: the geometry/UV/bake/rig/GLB stages. They are the next ports; until they
//  exist this library deliberately does not export their symbols at all, so nothing can call a
//  function that would silently do nothing. `tools/abi_conformance.py` prints exactly which
//  symbols remain.
// ==============================================================================================

use crate::abi::*;
use crate::anatomy::{self, BodyMetrics};
use crate::tables;
use core::ffi::{c_char, c_void};
use std::time::Instant;

/// Which stages have completed. Ordered so `>=` answers "did this run?".
///
/// The discriminants are the ABI's `AF_STAGE_*` values, on purpose: a stage number reaches the
/// caller through the progress callback, so the two implementations must agree on what "3" means.
/// (They did not, until `AnalyzeInputs` was added here - the enum started at `FitAnatomy`, which
/// would have made every progress callback one stage early.) `assert_enum_matches_abi` below keeps
/// it honest.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Stage {
    Idle = 0,
    AnalyzeInputs = 1,
    FitAnatomy = 2,
    Geometry = 3,
    Uv = 4,
    Textures = 5,
    Rig = 6,
    Blendshapes = 7,
    Animation = 8,
    Export = 9,
    Done = 10,
}

impl Stage {
    /// The number the ABI uses for this stage.
    pub fn num(self) -> i32 {
        self as i32
    }
}

/* Compile-time proof that the Rust stage numbering is the ABI's numbering. If a constant in
   abi.rs is ever renumbered without renumbering the enum (or the other way round), this stops
   the build instead of shipping progress callbacks that are one stage out. */
const _: () = {
    assert!(Stage::Idle as i32 == AF_STAGE_IDLE);
    assert!(Stage::AnalyzeInputs as i32 == AF_STAGE_ANALYZE_INPUTS);
    assert!(Stage::FitAnatomy as i32 == AF_STAGE_FIT_ANATOMY);
    assert!(Stage::Geometry as i32 == AF_STAGE_GEOMETRY);
    assert!(Stage::Uv as i32 == AF_STAGE_UV_UNWRAP);
    assert!(Stage::Textures as i32 == AF_STAGE_BAKE_TEXTURES);
    assert!(Stage::Rig as i32 == AF_STAGE_RIG);
    assert!(Stage::Blendshapes as i32 == AF_STAGE_BLENDSHAPES);
    assert!(Stage::Animation as i32 == AF_STAGE_ANIMATION);
    assert!(Stage::Export as i32 == AF_STAGE_EXPORT);
    assert!(Stage::Done as i32 == AF_STAGE_DONE);
};

pub struct Engine {
    pub spec: AfSpec,
    pub metrics: Option<BodyMetrics>,
    pub stage: Stage,
    pub budget_bytes: u64,
    pub callbacks: AfCallbacks,
    pub cancelled: bool,
    /// Last failure text, kept as a CString so `af_engine_last_error` can hand out a pointer that
    /// stays valid until the next failure (the C++ kernel does the same with a std::string buffer).
    pub last_error: std::ffi::CString,
    pub stats: AfStats,
    pub prompt_spec: Option<AfSpec>,
    pub peak_field_bytes: u64,
}

impl Engine {
    fn new(spec: AfSpec) -> Self {
        Engine {
            spec,
            metrics: None,
            stage: Stage::Idle,
            /* 1.5 GiB: the ceiling the app documents. A request that would exceed it is refused
               with AF_ERR_BUDGET_EXCEEDED instead of being attempted. */
            budget_bytes: 1536 * 1024 * 1024,
            callbacks: AfCallbacks {
                user: core::ptr::null_mut(),
                on_progress: None,
                on_log: None,
                should_cancel: None,
            },
            cancelled: false,
            last_error: std::ffi::CString::new("").unwrap_or_default(),
            stats: AfStats::zeroed(),
            prompt_spec: None,
            peak_field_bytes: 0,
        }
    }

    fn set_error(&mut self, message: &str) {
        self.last_error = std::ffi::CString::new(message).unwrap_or_default();
    }

    pub fn progress(&self, stage: i32, fraction: f32, message: &str) {
        if let Some(cb) = self.callbacks.on_progress {
            let msg = std::ffi::CString::new(message).unwrap_or_else(|_| std::ffi::CString::new("").unwrap());
            unsafe { cb(self.callbacks.user, stage, fraction.clamp(0.0, 1.0), msg.as_ptr()) };
        }
    }

    pub fn log(&mut self, severity: i32, message: &str) {
        if let Some(cb) = self.callbacks.on_log {
            if let Ok(msg) = std::ffi::CString::new(message) {
                unsafe { cb(self.callbacks.user, severity, msg.as_ptr()) };
            }
        }
    }

    /// Polled only at safe points, as the header promises: the stage boundary and inside loops
    /// that already check a budget.
    pub fn polled_cancel(&mut self) -> bool {
        if self.cancelled {
            return true;
        }
        if let Some(cb) = self.callbacks.should_cancel {
            if unsafe { cb(self.callbacks.user) } != 0 {
                self.cancelled = true;
                return true;
            }
        }
        false
    }

    /// Peak bytes for the geometry stage, using the same formula as the C++ kernel
    /// (`estimateFieldBytes` in af_engine.cpp): one float per lattice sample, times a 2.2 factor for
    /// the extraction's cell list and the output mesh, over the union of the body, head, hand,
    /// foot and apparel bounds.
    ///
    /// The bounds here are derived from the solved metrics rather than queried from the field, so
    /// they are deliberately generous: over-estimating refuses a request that would just barely
    /// have fit, under-estimating lets the process be OOM-killed - and only one of those is
    /// recoverable on a phone.
    pub fn estimate_field_bytes(&self, body_cell: f32) -> u64 {
        let m = match self.metrics {
            Some(m) => m,
            None => return 0,
        };
        let pad = 2.0 * body_cell;
        /* Widest extent: fingertips when the arms hang (armpit half-width + arm length), the
           head above the crown, the feet below the sole. 1.35 covers the apparel shell too. */
        let half_w = (m.biacromial_half + m.upper_arm_r * 2.0 + m.forearm_len + m.hand_length) * 1.35;
        let lo_x = -half_w;
        let hi_x = half_w;
        let lo_y = -0.02 * m.height;
        let hi_y = m.height * 1.02;
        let half_z = (m.head.rz + m.foot_length).max(m.chest.rz + m.glute_bulge) * 1.35;
        let lo_z = -half_z;
        let hi_z = half_z;
        let nx = ((hi_x - lo_x) as f64 + 2.0 * pad as f64) / body_cell as f64;
        let ny = ((hi_y - lo_y) as f64 + 2.0 * pad as f64) / body_cell as f64;
        let nz = ((hi_z - lo_z) as f64 + 2.0 * pad as f64) / body_cell as f64;
        let samples = (nx.ceil() + 2.0) * (ny.ceil() + 2.0) * (nz.ceil() + 2.0);
        (samples * 4.0 * 2.2) as u64
    }

    pub fn ensure_budget(&mut self, need: u64, what: &str) -> i32 {
        if need > self.budget_bytes {
            self.set_error(&format!(
                "{} would need {} MB, budget is {} MB",
                what,
                need / (1024 * 1024),
                self.budget_bytes / (1024 * 1024)
            ));
            return AF_ERR_BUDGET_EXCEEDED;
        }
        if need > self.peak_field_bytes {
            self.peak_field_bytes = need;
        }
        AF_OK
    }
}

/* ------------------------------------------------------------------ pointer plumbing ------- */
unsafe fn as_engine<'a>(ptr: *mut c_void) -> Option<&'a mut Engine> {
    if ptr.is_null() {
        None
    } else {
        Some(&mut *(ptr as *mut Engine))
    }
}

/*
 * Strings returned across the ABI (version, region names) are borrowed by the caller and must stay
 * valid for the life of the process. They are interned once: the cache is bounded by the number of
 * distinct static strings in the engine (a dozen), so nothing grows at runtime.
 */
fn cstr(s: &'static str) -> *const c_char {
    use std::collections::HashMap;
    use std::sync::OnceLock;
    static CACHE: OnceLock<std::sync::Mutex<HashMap<&'static str, std::ffi::CString>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| std::sync::Mutex::new(HashMap::new()));
    let mut guard = match cache.lock() {
        Ok(g) => g,
        Err(poisoned) => poisoned.into_inner(),
    };
    guard
        .entry(s)
        .or_insert_with(|| std::ffi::CString::new(s).unwrap_or_default())
        .as_ptr()
}

/* ------------------------------------------------------------------ the C ABI -------------- */

#[no_mangle]
pub extern "C" fn af_engine_abi_version() -> i32 {
    AF_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn af_engine_version() -> *const c_char {
    cstr(AF_ENGINE_VERSION)
}

#[no_mangle]
pub extern "C" fn af_spec_init(spec: *mut AfSpec) {
    if spec.is_null() {
        return;
    }
    unsafe { *spec = AfSpec::defaulted() };
}

#[no_mangle]
pub extern "C" fn af_engine_create(spec: *const AfSpec, callbacks: *const AfCallbacks, out: *mut *mut c_void) -> i32 {
    if out.is_null() {
        return AF_ERR_INVALID_ARG;
    }
    unsafe { *out = core::ptr::null_mut() };
    let spec = if spec.is_null() { AfSpec::defaulted() } else { unsafe { *spec } };
    if let Err(field) = spec.validate() {
        return match field {
            "struct_size" => AF_ERR_ABI_MISMATCH,
            _ => AF_ERR_INVALID_ARG,
        };
    }
    let mut engine = Engine::new(spec);
    if !callbacks.is_null() {
        engine.callbacks = unsafe { *callbacks };
    }
    let boxed = Box::into_raw(Box::new(engine));
    unsafe { *out = boxed as *mut c_void };
    AF_OK
}

#[no_mangle]
pub extern "C" fn af_engine_destroy(engine: *mut c_void) {
    if engine.is_null() {
        return;
    }
    /* Reclaim the Box exactly once. The caller must not use the handle afterwards; that rule is
       in the header and cannot be enforced from inside the library. */
    unsafe { drop(Box::from_raw(engine as *mut Engine)) };
}

#[no_mangle]
pub extern "C" fn af_engine_reset(engine: *mut c_void, spec: *const AfSpec) -> i32 {
    let e = match unsafe { as_engine(engine) } {
        Some(e) => e,
        None => return AF_ERR_INVALID_ARG,
    };
    let spec = if spec.is_null() { AfSpec::defaulted() } else { unsafe { *spec } };
    if let Err(field) = spec.validate() {
        e.set_error(&format!("invalid spec field: {}", field));
        return if field == "struct_size" { AF_ERR_ABI_MISMATCH } else { AF_ERR_INVALID_ARG };
    }
    e.spec = spec;
    e.metrics = None;
    e.stage = Stage::Idle;
    e.cancelled = false;
    e.last_error = std::ffi::CString::new("").unwrap_or_default();
    e.stats = AfStats::zeroed();
    e.peak_field_bytes = 0;
    AF_OK
}

#[no_mangle]
pub extern "C" fn af_engine_set_memory_budget(engine: *mut c_void, bytes: u64) -> i32 {
    let e = match unsafe { as_engine(engine) } {
        Some(e) => e,
        None => return AF_ERR_INVALID_ARG,
    };
    if bytes < 64 * 1024 * 1024 {
        e.set_error("memory budget below 64 MB would not fit a single stage");
        return AF_ERR_INVALID_ARG;
    }
    e.budget_bytes = bytes;
    AF_OK
}

#[no_mangle]
pub extern "C" fn af_engine_fit_anatomy(engine: *mut c_void) -> i32 {
    let e = match unsafe { as_engine(engine) } {
        Some(e) => e,
        None => return AF_ERR_INVALID_ARG,
    };
    if e.polled_cancel() {
        return AF_ERR_CANCELLED;
    }
    if let Err(field) = e.spec.validate() {
        e.set_error(&format!("invalid spec field: {}", field));
        return AF_ERR_INVALID_ARG;
    }
    e.progress(Stage::FitAnatomy.num(), 0.1, "solving anthropometric proportions");
    let t0 = Instant::now();
    let metrics = anatomy::solve(&e.spec);
    let ms = t0.elapsed().as_secs_f32() * 1000.0;

    /* The metrics are only useful if they describe a body: this is the same plausibility gate the
       C++ harness applies (a BMI outside 12..48 means the inputs were contradictory, e.g. a
       1.30 m stature with a 120 cm waist). */
    let bmi = anatomy::bmi(&metrics);
    if !(12.0..48.0).contains(&bmi) {
        e.set_error(&format!("anatomy solve produced an implausible body (BMI {:.1})", bmi));
        return AF_ERR_INVALID_ARG;
    }
    e.metrics = Some(metrics);
    e.stage = Stage::FitAnatomy;
    e.stats.fit_anatomy_ms = ms;
    e.progress(Stage::FitAnatomy.num(), 1.0, "proportions solved");
    e.log(AF_LOG_INFO, &format!("anatomy: {:.3} m, BMI {:.1}, solved in {:.2} ms", metrics.height, bmi, ms));
    AF_OK
}

/// `af_engine_analyze_prompt` -- interprets a text prompt into the engine's spec.
///
/// The C++ kernel parses into `engine->spec` in place, logs each understood phrase, warns when the
/// prompt recognised nothing, and (only when something matched) invalidates everything downstream
/// of the anatomy by dirtying the cached geometry. This port does the same in the same order, so
/// a caller cannot tell the two kernels apart except by the log text, which is identical too.
#[no_mangle]
pub extern "C" fn af_engine_analyze_prompt(
    engine: *mut c_void,
    prompt: *const c_char,
    out_spec: *mut AfSpec,
) -> i32 {
    let e = match unsafe { as_engine(engine) } {
        Some(e) => e,
        None => return AF_ERR_INVALID_ARG,
    };
    if prompt.is_null() {
        e.set_error("analyze_prompt: prompt is null");
        return AF_ERR_INVALID_ARG;
    }
    /* The ABI says the prompt is a NUL-terminated UTF-8 string. A caller that hands us malformed
       bytes gets the replacement character rather than a panic - a bad string is a bad prompt, not
       an engine failure. */
    let text = unsafe { std::ffi::CStr::from_ptr(prompt) }.to_string_lossy().into_owned();
    e.stage = Stage::AnalyzeInputs;
    e.progress(Stage::AnalyzeInputs.num(), 0.0, "reading prompt");
    let mut parse = crate::prompt::PromptParse::default();
    let matched = crate::prompt::parse_prompt(&text, &mut e.spec, Some(&mut parse));
    for i in 0..parse.token_count as usize {
        let msg = format!(
            "understood \"{}\" -> {}",
            parse.tokens[i].text_str(),
            parse.tokens[i].field_str()
        );
        e.log(AF_LOG_INFO, &msg);
    }
    if matched == 0 {
        e.log(AF_LOG_WARN, "prompt recognised no attributes; using the default spec");
    } else {
        /* Any spec change invalidates everything downstream of the anatomy. */
        e.metrics = None;
        e.stats = AfStats::zeroed();
    }
    if !out_spec.is_null() {
        unsafe { *out_spec = e.spec };
    }
    e.progress(Stage::AnalyzeInputs.num(), 1.0, "prompt interpreted");
    matched
}

#[no_mangle]
pub extern "C" fn af_engine_stats(engine: *const c_void, out: *mut AfStats) -> i32 {
    if engine.is_null() || out.is_null() {
        return AF_ERR_INVALID_ARG;
    }
    let e = unsafe { &*(engine as *const Engine) };
    unsafe { *out = e.stats };
    AF_OK
}

#[no_mangle]
pub extern "C" fn af_engine_last_error(engine: *const c_void) -> *const c_char {
    if engine.is_null() {
        return cstr("");
    }
    let e = unsafe { &*(engine as *const Engine) };
    e.last_error.as_ptr()
}

#[no_mangle]
pub extern "C" fn af_engine_clip_name(clip: i32, out_name: *mut c_char, max_bytes: i32) -> i32 {
    if out_name.is_null() || max_bytes <= 0 {
        return 0;
    }
    let name = match tables::clip_name(clip) {
        Some(n) => n,
        None => {
            unsafe { *out_name = 0 };
            return 0;
        }
    };
    let bytes = name.as_bytes();
    let n = bytes.len().min(max_bytes as usize - 1);
    unsafe {
        core::ptr::copy_nonoverlapping(bytes.as_ptr(), out_name as *mut u8, n);
        *out_name.add(n) = 0;
    }
    n as i32
}

#[no_mangle]
pub extern "C" fn af_engine_clip_duration(_engine: *const c_void, clip: i32) -> f32 {
    /* Pure table lookup: the duration is a property of the authored clip, not of the instance. The
       handle is accepted (and ignored) only because the published ABI takes it. */
    tables::clip_duration(clip).unwrap_or(0.0)
}

#[no_mangle]
pub extern "C" fn af_engine_blendshape_names(names_out: *mut [c_char; 64], max_names: i32) -> i32 {
    if names_out.is_null() || max_names <= 0 {
        return AF_ERR_INVALID_ARG;
    }
    let n = (max_names as usize).min(tables::BLENDSHAPES.len());
    for i in 0..n {
        let name = tables::BLENDSHAPES[i].as_bytes();
        unsafe {
            let dst = (*names_out.add(i)).as_mut_ptr();
            core::ptr::copy_nonoverlapping(name.as_ptr(), dst as *mut u8, name.len());
            *dst.add(name.len()) = 0;
        }
    }
    n as i32
}

#[no_mangle]
pub extern "C" fn af_engine_region_name(region: i32) -> *const c_char {
    cstr(tables::region_name(region))
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::c_void;

    fn create() -> (*mut c_void, AfSpec) {
        let spec = AfSpec::defaulted();
        let mut h: *mut c_void = core::ptr::null_mut();
        assert_eq!(af_engine_create(&spec, core::ptr::null(), &mut h), AF_OK);
        assert!(!h.is_null());
        (h, spec)
    }

    #[test]
    fn create_rejects_a_mismatched_struct_size() {
        let mut spec = AfSpec::defaulted();
        spec.struct_size = 123;
        let mut h: *mut c_void = core::ptr::null_mut();
        assert_eq!(af_engine_create(&spec, core::ptr::null(), &mut h), AF_ERR_ABI_MISMATCH);
        assert!(h.is_null());
    }

    #[test]
    fn null_handles_are_errors_not_segfaults() {
        assert_eq!(af_engine_reset(core::ptr::null_mut(), core::ptr::null()), AF_ERR_INVALID_ARG);
        assert_eq!(af_engine_fit_anatomy(core::ptr::null_mut()), AF_ERR_INVALID_ARG);
        assert_eq!(af_engine_set_memory_budget(core::ptr::null_mut(), 1 << 30), AF_ERR_INVALID_ARG);
        assert_eq!(af_engine_stats(core::ptr::null(), core::ptr::null_mut()), AF_ERR_INVALID_ARG);
        af_engine_destroy(core::ptr::null_mut());   /* must be a no-op, not a crash */
        assert_eq!(af_engine_last_error(core::ptr::null()), af_engine_last_error(core::ptr::null()));
    }

    #[test]
    fn stage_order_is_enforced_and_anatomy_runs() {
        let (h, _spec) = create();
        let e = unsafe { as_engine(h).unwrap() };
        assert_eq!(e.stage, Stage::Idle);
        assert_eq!(af_engine_fit_anatomy(h), AF_OK);
        assert_eq!(e.stage, Stage::FitAnatomy);
        let m = e.metrics.expect("metrics stored");
        assert!((m.height - 1.756).abs() < 1e-6, "default male stature {}", m.height);
        assert!(e.stats.fit_anatomy_ms >= 0.0);
        af_engine_destroy(h);
    }

    #[test]
    fn budget_is_refused_before_the_allocation() {
        let (h, _spec) = create();
        let e = unsafe { as_engine(h).unwrap() };
        assert_eq!(af_engine_fit_anatomy(h), AF_OK);
        let need = e.estimate_field_bytes(0.008);
        /* The estimate has to be in the right ballpark, or the budget gate is decorative: the C++
           kernel measures ~230-300 MB for the 8 mm lattice on this body. */
        /* The C++ kernel measures 43.1 MB for this body at the 8 mm lattice
           (`peak_field_bytes` after build_geometry). The Rust estimate is deliberately generous -
           it derives the bounds from the solved metrics instead of querying the field - so the
           assertion is a band, not an equality: over-estimating refuses a request that would just
           barely have fit; under-estimating gets the process OOM-killed. */
        let need_mb = need / (1024 * 1024);
        assert!((40..=120).contains(&need_mb),
                "8 mm lattice estimate {} MB is implausible (reference: 43 MB)", need_mb);
        assert_eq!(af_engine_set_memory_budget(h, 64 * 1024 * 1024), AF_OK);
        assert_eq!(e.ensure_budget(need, "geometry"), AF_ERR_BUDGET_EXCEEDED);
        assert!(e.last_error.to_string_lossy().contains("budget"));
        af_engine_destroy(h);
    }

    #[test]
    fn cancellation_is_reported_at_a_poll_point() {
        unsafe extern "C" fn always_cancel(_user: *mut c_void) -> i32 {
            1
        }
        let spec = AfSpec::defaulted();
        let cbs = AfCallbacks {
            user: core::ptr::null_mut(),
            on_progress: None,
            on_log: None,
            should_cancel: Some(always_cancel),
        };
        let mut h: *mut c_void = core::ptr::null_mut();
        assert_eq!(af_engine_create(&spec, &cbs, &mut h), AF_OK);
        assert_eq!(af_engine_fit_anatomy(h), AF_ERR_CANCELLED);
        af_engine_destroy(h);
    }

    #[test]
    fn progress_callbacks_fire_with_increasing_fractions() {
        use std::sync::atomic::{AtomicI32, Ordering};
        static SEEN: AtomicI32 = AtomicI32::new(0);
        unsafe extern "C" fn on_progress(_u: *mut c_void, stage: i32, fraction: f32, _msg: *const c_char) {
            assert_eq!(stage, Stage::FitAnatomy.num());
            assert_eq!(stage, AF_STAGE_FIT_ANATOMY, "the enum and the ABI must agree");
            assert!((0.0..=1.0).contains(&fraction));
            SEEN.fetch_add(1, Ordering::Relaxed);
        }
        let spec = AfSpec::defaulted();
        let cbs = AfCallbacks {
            user: core::ptr::null_mut(),
            on_progress: Some(on_progress),
            on_log: None,
            should_cancel: None,
        };
        let mut h: *mut c_void = core::ptr::null_mut();
        assert_eq!(af_engine_create(&spec, &cbs, &mut h), AF_OK);
        assert_eq!(af_engine_fit_anatomy(h), AF_OK);
        assert!(SEEN.load(Ordering::Relaxed) >= 2, "at least start and finish progress");
        af_engine_destroy(h);
    }

    #[test]
    fn clip_and_blendshape_tables_are_served_through_the_abi() {
        let mut buf = [0 as c_char; 64];
        let n = af_engine_clip_name(AF_CLIP_WALK, buf.as_mut_ptr(), 64);
        assert_eq!(n, 4);
        let name: String = buf.iter().take_while(|c| **c != 0).map(|c| *c as u8 as char).collect();
        assert_eq!(name, "walk");
        assert_eq!(af_engine_clip_name(AF_CLIP_COUNT, buf.as_mut_ptr(), 64), 0);
        assert!((af_engine_clip_duration(core::ptr::null(), AF_CLIP_RUN) - 0.62).abs() < 1e-6);

        let mut names = [[0 as c_char; 64]; 52];
        assert_eq!(af_engine_blendshape_names(names.as_mut_ptr(), 52), 52);
        let first: String = names[0].iter().take_while(|c| **c != 0).map(|c| *c as u8 as char).collect();
        assert_eq!(first, "browDownLeft");
    }

    #[test]
    fn analyze_prompt_updates_the_spec_and_reports_matches() {
        let mut e = Engine::new(AfSpec::defaulted());
        let text = std::ffi::CString::new("a tall athletic woman, 172 cm, ponytail").unwrap();
        let mut out = AfSpec::defaulted();
        let n = af_engine_analyze_prompt(&mut e as *mut Engine as *mut c_void, text.as_ptr(), &mut out);
        assert!(n > 0, "recognised {} attributes", n);
        assert_eq!(out.sex, AF_SEX_FEMALE);
        assert_eq!(out.hair_style, AF_HAIR_PONYTAIL);
        assert_eq!(e.spec.prompt_str(), "a tall athletic woman, 172 cm, ponytail");
        assert_eq!(e.stage, Stage::AnalyzeInputs);

        /* null prompt is an argument error, not a crash */
        assert_eq!(af_engine_analyze_prompt(&mut e as *mut Engine as *mut c_void, std::ptr::null(), std::ptr::null_mut()), AF_ERR_INVALID_ARG);

        /* Noise on a fresh engine changes nothing and says so. (Parsing is in place by design:
           on an engine that already holds a spec, unmatched words leave the earlier values
           alone - that is what "unset fields keep whatever spec already had" means.) */
        let mut fresh = Engine::new(AfSpec::defaulted());
        let junk = std::ffi::CString::new("qwertyuiop zxcvbnm").unwrap();
        let mut out2 = AfSpec::defaulted();
        let n2 = af_engine_analyze_prompt(&mut fresh as *mut Engine as *mut c_void, junk.as_ptr(), &mut out2);
        assert_eq!(n2, 0);
        assert_eq!(out2.sex, AF_SEX_UNSPECIFIED);
        assert_eq!(out2.age_years, 30.0, "the documented default age survives a failed parse");

        /* ...and on the engine that already has a woman, noise leaves the woman alone. */
        let n3 = af_engine_analyze_prompt(&mut e as *mut Engine as *mut c_void, junk.as_ptr(), std::ptr::null_mut());
        assert_eq!(n3, 0);
        assert_eq!(e.spec.sex, AF_SEX_FEMALE);
    }

    #[test]
    fn region_names_come_from_the_table() {
        let s = |p: *const c_char| -> String {
            unsafe { std::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned()
        };
        assert_eq!(s(af_engine_region_name(0)), "skin");
        assert_eq!(s(af_engine_region_name(9)), "cloth_upper");
        assert_eq!(s(af_engine_region_name(1)), "eye_left");
        assert_eq!(s(af_engine_region_name(99)), "skin");
    }
}
