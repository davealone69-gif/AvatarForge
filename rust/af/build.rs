// =============================================================================================
//  AvatarForge :: af/build.rs
// =============================================================================================
//
//  `cargo:rustc-link-arg` only reaches the binaries and tests of the package that emits it, so
//  af-sys publishes its resolved library directory through the `links` mechanism
//  (DEP_AFCORE_LIB_DIR) and this script turns it into an rpath for *this* crate's tests and
//  examples. Without it, `cargo test` builds fine and then fails at run time with
//  "libafcore.so: cannot open shared object file".
// =============================================================================================

use std::env;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    // (DEP_AFCORE_LIB_DIR is provided by cargo via the links key; no rerun trigger needed.)

    let Ok(dir) = env::var("DEP_AFCORE_LIB_DIR") else {
        // af-sys did not find a library (a normal situation for an Android build where the .so
        // ships inside the APK). Nothing to do.
        return;
    };
    if dir.is_empty() {
        return;
    }
    println!("cargo:rustc-link-search=native={dir}");
    // This RUNPATH lets standalone binaries locate the selected library. Cargo prepends
    // target/debug/deps to LD_LIBRARY_PATH for tests; when the Rust alternate core is also built
    // there as libafcore.so, that directory can shadow the C++ ABI library. tools/verify.sh
    // preloads the exact C++ library for the workspace test run. Android packages the selected .so.
    let rpath = format!("-Wl,-rpath,{dir}");
    println!("cargo:rustc-link-arg={rpath}");
    println!("cargo:rustc-link-arg-tests={rpath}");
}
