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
    // Tests, benches and examples of this package all need to find the kernel at run time.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    println!("cargo:rustc-link-arg-tests=-Wl,-rpath,{dir}");
}
