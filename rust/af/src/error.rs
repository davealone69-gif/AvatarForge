// =============================================================================================
//  af::error -- typed view of the kernel's af_result codes
// =============================================================================================
//
//  The kernel never throws and never reports a bare integer to the caller: every failure carries
//  a message that lives on the engine handle until the next call. The wrapper pairs the two so a
//  Rust caller gets `Err(Error::NotReady { message })` rather than an opaque -4.
// =============================================================================================

use core::fmt;

/// Every way a kernel call can fail. Mirrors `af_result` one-for-one, plus the two conditions
/// that only exist on the Rust side (a poisoned mutex and invalid UTF-8 coming back from C).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// A pointer, index or size argument was rejected before any work was done.
    InvalidArgument(String),
    /// A field grid or texture allocation did not fit in the configured budget.
    OutOfMemory(String),
    /// The device lacks a feature this call needs.
    Unsupported(String),
    /// A prerequisite stage has not run yet (e.g. rigging before geometry).
    NotReady(String),
    /// Reading or writing a file failed (export targets only).
    Io(String),
    /// `cancel()` was requested through the callbacks and the kernel stopped at a safe point.
    Cancelled,
    /// Internal invariant violation. Always a bug: the message names the check that failed.
    Internal(String),
    /// The loaded library reports a different ABI version than these bindings were built for.
    AbiMismatch { expected: i32, found: i32 },
    /// The work would exceed the memory ceiling set with `set_memory_budget`.
    BudgetExceeded(String),
}

impl Error {
    /// Numeric kernel code, or `AF_ERR_INTERNAL` for the Rust-side conditions.
    pub fn code(&self) -> i32 {
        match self {
            Error::InvalidArgument(_) => af_sys::AF_ERR_INVALID_ARG,
            Error::OutOfMemory(_) => af_sys::AF_ERR_OUT_OF_MEMORY,
            Error::Unsupported(_) => af_sys::AF_ERR_UNSUPPORTED,
            Error::NotReady(_) => af_sys::AF_ERR_NOT_READY,
            Error::Io(_) => af_sys::AF_ERR_IO,
            Error::Cancelled => af_sys::AF_ERR_CANCELLED,
            Error::Internal(_) => af_sys::AF_ERR_INTERNAL,
            Error::AbiMismatch { .. } => af_sys::AF_ERR_ABI_MISMATCH,
            Error::BudgetExceeded(_) => af_sys::AF_ERR_BUDGET_EXCEEDED,
        }
    }

    /// True when retrying the same call with the same arguments could succeed (a transient
    /// allocation failure or an interrupted stage), as opposed to a caller mistake.
    pub fn is_retryable(&self) -> bool {
        matches!(self, Error::OutOfMemory(_) | Error::BudgetExceeded(_) | Error::Io(_))
    }

    pub(crate) fn from_code(code: i32, message: &str) -> Error {
        let text = message.to_string();
        match code {
            af_sys::AF_ERR_INVALID_ARG => Error::InvalidArgument(text),
            af_sys::AF_ERR_OUT_OF_MEMORY => Error::OutOfMemory(text),
            af_sys::AF_ERR_UNSUPPORTED => Error::Unsupported(text),
            af_sys::AF_ERR_NOT_READY => Error::NotReady(text),
            af_sys::AF_ERR_IO => Error::Io(text),
            af_sys::AF_ERR_CANCELLED => Error::Cancelled,
            af_sys::AF_ERR_ABI_MISMATCH => Error::AbiMismatch { expected: af_sys::AF_ABI_VERSION, found: code },
            af_sys::AF_ERR_BUDGET_EXCEEDED => Error::BudgetExceeded(text),
            _ => Error::Internal(text),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::InvalidArgument(m) => write!(f, "invalid argument: {m}"),
            Error::OutOfMemory(m) => write!(f, "out of memory: {m}"),
            Error::Unsupported(m) => write!(f, "unsupported: {m}"),
            Error::NotReady(m) => write!(f, "not ready: {m}"),
            Error::Io(m) => write!(f, "io error: {m}"),
            Error::Cancelled => write!(f, "cancelled"),
            Error::Internal(m) => write!(f, "internal error: {m}"),
            Error::AbiMismatch { expected, found } => {
                write!(f, "ABI mismatch: expected {expected}, library reported {found}")
            }
            Error::BudgetExceeded(m) => write!(f, "memory budget exceeded: {m}"),
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = core::result::Result<T, Error>;
