// ==============================================================================================
//  AvatarForge :: af-core  --  the Rust native core
// ==============================================================================================
//
//  This crate is the Rust implementation of the shared C ABI in `native/include/afcore.h`. It is
//  built the opposite way round from a binding:
//
//      * a *binding* (rust/af-sys) calls someone else's library through the ABI;
//      * this crate *is* a library behind the same ABI, so the Kotlin app and the Rust consumers
//        can talk to either implementation with no code change on their side.
//
//  Why two implementations. The ABI is the contract (`abi/afcore.abi.json`, generated from the
//  header), and the engine logic - the stage state machine, the memory budget, the determinism
//  guarantees, and the tables that decide what clip id 3 or blendshape 24 means - is the part that
//  must never drift between the Kotlin SDK and the Rust API. Rust gives that logic memory safety
//  and, once the numeric subsystems are ported, a second independent implementation to check the
//  first against.
//
//  Status: **a work in progress with a measurable gate.** Implemented here today:
//
//      * lifecycle and validation   af_engine_create/destroy/reset, af_spec_init
//      * engine logic               stage state machine, memory budget, progress/log/cancel
//                                   callbacks, error strings, statistics
//      * anatomy                    the anthropometric solver, ported in full
//      * prompt                     the deterministic text-prompt grammar -> af_spec
//      * tables                     clip ids and durations, the 52 ARKit channels, regions
//
//  NOT implemented yet (and therefore not exported at all, so nothing can call a function that
//  silently does nothing): image analysis, geometry, UV unwrap, texture bake, rig,
//  blendshape and animation bake, pose/blendshape application, geometry accessors, GLB export.
//
//  `tools/abi_conformance.py` measures exactly that: it prints how many of the 47 ABI symbols this
//  library resolves, checks the invariants both implementations must satisfy, and with
//  `--reference` diffs behaviour against the C++ kernel. The Rust core is ready to be packaged
//  into the APK when that report is 47/47 with no differences - not before.
//
//  Build and check:
//      cargo build --release --offline -p af-core
//      cargo test  --offline -p af-core
//      python3 tools/abi_conformance.py target/release/libafcore_rust.so \
//              --reference build-host/libafcore.so
// ==============================================================================================

pub mod abi;
pub mod anatomy;
pub mod engine;
pub mod prompt;
pub mod spec;
pub mod tables;

pub use abi::*;
pub use anatomy::BodyMetrics;
pub use engine::Engine;
pub use prompt::{parse_prompt, looks_like_avatar_request, PromptParse, PromptToken};
