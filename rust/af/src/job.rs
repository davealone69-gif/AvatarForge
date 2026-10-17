// =============================================================================================
//  af::job -- progress, cancellation and stage reporting
// =============================================================================================
//
//  The kernel reports progress through C callbacks that run synchronously on the worker thread
//  that called into it. Those callbacks must not allocate, must not unwind, and must not call
//  back into the engine, so this module keeps them as small as physically possible:
//
//      * the callback trampoline only touches two atomics and one mutex;
//      * everything a UI wants to read (stage, fraction, last message, log lines) is snapshotted
//        into a shared `JobState` the caller can poll from any thread;
//      * cancellation is a single `AtomicBool` the trampoline reads - the kernel polls it at its
//        own safe points, so a cancel request never corrupts a half-built mesh.
//
//  `JobHandle` is the Rust twin of the Kotlin SDK's JobHandle: same states, same semantics, so a
//  hybrid app can drive either from the same view-model code.
// =============================================================================================

use af_sys as sys;
use core::ffi::c_char;
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};

/// Lifecycle of a generation job. Mirrors the Kotlin `JobHandle.Status`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled,
}

impl Status {
    pub fn as_str(self) -> &'static str {
        match self {
            Status::Idle => "idle",
            Status::Running => "running",
            Status::Succeeded => "succeeded",
            Status::Failed => "failed",
            Status::Cancelled => "cancelled",
        }
    }

    pub fn is_terminal(self) -> bool {
        matches!(self, Status::Succeeded | Status::Failed | Status::Cancelled)
    }
}

/// One log line from the kernel, with the severity it was reported at.
#[derive(Debug, Clone)]
pub struct LogLine {
    pub severity: i32,
    pub message: String,
}

/// A snapshot of what the job is doing right now. Cheap to clone; the UI polls it once a frame.
#[derive(Debug, Clone)]
pub struct Progress {
    pub status: Status,
    /// 0..=10, one of the `af_stage` values (see `Stage`).
    pub stage: i32,
    /// Human-readable stage name, e.g. "Baking textures".
    pub stage_name: &'static str,
    /// 0..1 within the current stage (the kernel reports per-stage fractions).
    pub fraction: f32,
    /// Overall estimate across all stages, 0..1.
    pub overall: f32,
    pub message: String,
    pub logs: Vec<LogLine>,
}

impl Default for Progress {
    fn default() -> Self {
        Self {
            status: Status::Idle,
            stage: sys::AF_STAGE_IDLE,
            stage_name: stage_name(sys::AF_STAGE_IDLE),
            fraction: 0.0,
            overall: 0.0,
            message: String::new(),
            logs: Vec::new(),
        }
    }
}

/// Human-readable name for one of the kernel's eleven stages. Used by the CLI, the Rust SDK and
/// the app's progress list alike, so the wording is identical everywhere.
pub fn stage_name(stage: i32) -> &'static str {
    match stage {
        sys::AF_STAGE_IDLE => "Idle",
        sys::AF_STAGE_ANALYZE_INPUTS => "Analysing inputs",
        sys::AF_STAGE_FIT_ANATOMY => "Fitting anatomy",
        sys::AF_STAGE_GEOMETRY => "Building geometry",
        sys::AF_STAGE_UV_UNWRAP => "Unwrapping UVs",
        sys::AF_STAGE_BAKE_TEXTURES => "Baking textures",
        sys::AF_STAGE_RIG => "Rigging",
        sys::AF_STAGE_BLENDSHAPES => "Facial blendshapes",
        sys::AF_STAGE_ANIMATION => "Authoring animation",
        sys::AF_STAGE_EXPORT => "Exporting",
        sys::AF_STAGE_DONE => "Done",
        _ => "Unknown",
    }
}

/// Number of pipeline stages the progress bar is scaled over (IDLE..DONE inclusive).
pub const STAGE_SPAN: f32 = 10.0;

pub(crate) struct SharedState {
    status: AtomicI32,
    stage: AtomicI32,
    /// f32 bits, so the callback can store a float without a lock.
    fraction: AtomicU32,
    cancel: AtomicBool,
    text: Mutex<ProgressText>,
}

struct ProgressText {
    message: String,
    logs: Vec<LogLine>,
}

impl SharedState {
    fn new() -> Self {
        Self {
            status: AtomicI32::new(Status::Idle as i32),
            stage: AtomicI32::new(sys::AF_STAGE_IDLE),
            fraction: AtomicU32::new(0.0f32.to_bits()),
            cancel: AtomicBool::new(false),
            text: Mutex::new(ProgressText { message: String::new(), logs: Vec::new() }),
        }
    }

    pub(crate) fn snapshot(&self) -> Progress {
        let status = match self.status.load(Ordering::Relaxed) {
            x if x == Status::Running as i32 => Status::Running,
            x if x == Status::Succeeded as i32 => Status::Succeeded,
            x if x == Status::Failed as i32 => Status::Failed,
            x if x == Status::Cancelled as i32 => Status::Cancelled,
            _ => Status::Idle,
        };
        let stage = self.stage.load(Ordering::Relaxed);
        let fraction = f32::from_bits(self.fraction.load(Ordering::Relaxed));
        let text = self.text.lock().unwrap_or_else(|e| e.into_inner());
        let overall = ((stage.max(0) as f32) + fraction.clamp(0.0, 1.0)) / STAGE_SPAN;
        Progress {
            status,
            stage,
            stage_name: stage_name(stage),
            fraction,
            overall: overall.clamp(0.0, 1.0),
            message: text.message.clone(),
            logs: text.logs.clone(),
        }
    }
}

/// Owns the shared state plus the C callback table handed to `af_engine_create`.
pub(crate) struct CallbackBridge {
    pub(crate) state: Arc<SharedState>,
    pub(crate) table: sys::af_callbacks,
}

impl CallbackBridge {
    pub(crate) fn new(keep_logs: usize) -> Self {
        let state = Arc::new(SharedState::new());
        // Two raw pointers: one is the Arc (so the trampoline can find the state), the other the
        // log cap. They are packaged in a boxed user-data blob owned by the bridge.
        let user = Box::into_raw(Box::new(UserData {
            state: Arc::clone(&state),
            keep_logs,
        }));
        let table = sys::af_callbacks {
            user: user as *mut core::ffi::c_void,
            on_progress: Some(on_progress),
            on_log: Some(on_log),
            should_cancel: Some(should_cancel),
        };
        Self { state, table }
    }

    /// Drops the user-data blob. Called from the engine wrapper's destructor, after the handle is
    /// gone, so no callback can be in flight.
    pub(crate) unsafe fn release(&mut self) {
        if !self.table.user.is_null() {
            drop(unsafe { Box::from_raw(self.table.user as *mut UserData) });
            self.table.user = core::ptr::null_mut();
        }
    }

    pub(crate) fn request_cancel(&self) {
        self.state.cancel.store(true, Ordering::Relaxed);
    }

    pub(crate) fn clear_cancel(&self) {
        self.state.cancel.store(false, Ordering::Relaxed);
    }

    pub(crate) fn begin(&self) {
        self.state.status.store(Status::Running as i32, Ordering::Relaxed);
        self.state.stage.store(sys::AF_STAGE_IDLE, Ordering::Relaxed);
        self.state.fraction.store(0.0f32.to_bits(), Ordering::Relaxed);
    }

    pub(crate) fn finish(&self, status: Status) {
        self.state.status.store(status as i32, Ordering::Relaxed);
        if status == Status::Succeeded {
            self.state.stage.store(sys::AF_STAGE_DONE, Ordering::Relaxed);
            self.state.fraction.store(0.0f32.to_bits(), Ordering::Relaxed);
        }
    }
}

struct UserData {
    state: Arc<SharedState>,
    keep_logs: usize,
}

/// The callback itself. It must never panic (an unwind across the FFI boundary is UB), so every
/// fallible step is written with explicit checks instead of `unwrap`.
unsafe extern "C" fn on_progress(
    user: *mut core::ffi::c_void,
    stage: core::ffi::c_int,
    fraction: core::ffi::c_float,
    message: *const c_char,
) {
    if user.is_null() {
        return;
    }
    // SAFETY: `user` is the Box<UserData> created in CallbackBridge::new and released only
    // after af_engine_destroy, so no callback can observe a dangling pointer.
    let data = unsafe { &*(user as *const UserData) };
    data.state.stage.store(stage, Ordering::Relaxed);
    data.state.fraction.store(fraction.to_bits(), Ordering::Relaxed);
    if !message.is_null() {
        let text = unsafe { core::ffi::CStr::from_ptr(message) };
        if let Ok(text) = text.to_str() {
            if let Ok(mut guard) = data.state.text.lock() {
                guard.message.clear();
                guard.message.push_str(text);
            }
        }
    }
}

unsafe extern "C" fn on_log(user: *mut core::ffi::c_void, severity: core::ffi::c_int, message: *const c_char) {
    if user.is_null() || message.is_null() {
        return;
    }
    let data = unsafe { &*(user as *const UserData) };
    let text = match unsafe { core::ffi::CStr::from_ptr(message) }.to_str() {
        Ok(t) => t,
        Err(_) => return,
    };
    if let Ok(mut guard) = data.state.text.lock() {
        guard.logs.push(LogLine { severity, message: text.to_string() });
        if guard.logs.len() > data.keep_logs {
            let drop_count = guard.logs.len() - data.keep_logs;
            guard.logs.drain(0..drop_count);
        }
    }
}

unsafe extern "C" fn should_cancel(user: *mut core::ffi::c_void) -> core::ffi::c_int {
    if user.is_null() {
        return 0;
    }
    let data = unsafe { &*(user as *const UserData) };
    if data.state.cancel.load(Ordering::Relaxed) {
        1
    } else {
        0
    }
}

/// A pollable view of a running job. Cloning is cheap and any thread may read it; only the
/// `Engine` that owns the job may drive it.
#[derive(Clone)]
pub struct JobHandle {
    pub(crate) state: Arc<SharedState>,
}

impl JobHandle {
    pub fn status(&self) -> Status {
        self.state.snapshot().status
    }

    pub fn progress(&self) -> Progress {
        self.state.snapshot()
    }

    /// Asks the kernel to stop. The request is honoured at the next safe point, so a cancelled
    /// job leaves a consistent (if incomplete) engine behind, never a corrupt mesh.
    pub fn cancel(&self) {
        self.state.cancel.store(true, Ordering::Relaxed);
    }

    pub fn is_cancel_requested(&self) -> bool {
        self.state.cancel.load(Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stage_names_cover_every_stage() {
        for stage in 0..sys::AF_STAGE_COUNT {
            assert_ne!(stage_name(stage), "Unknown", "stage {stage} has no name");
        }
        assert_eq!(stage_name(9999), "Unknown");
    }

    #[test]
    fn overall_progress_is_monotonic_per_stage() {
        let mut bridge = CallbackBridge::new(4);
        bridge.begin();
        let mut previous = 0.0f32;
        for stage in 0..=sys::AF_STAGE_COUNT {
            bridge.state.stage.store(stage, Ordering::Relaxed);
            bridge.state.fraction.store(0.5f32.to_bits(), Ordering::Relaxed);
            let overall = bridge.state.snapshot().overall;
            assert!(overall >= previous, "overall went backwards at stage {stage}");
            previous = overall;
        }
        unsafe { bridge.release() }; // the bridge owns this allocation
    }
}
