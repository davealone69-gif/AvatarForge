// =============================================================================================
//  AvatarForge :: af-sys/build.rs
// =============================================================================================
//
//  A dependency-free build script (no `cc`, no `bindgen`, no `cmake` crate - nothing to fetch).
//
//  Its only job is to point the linker at the already-built kernel library:
//
//     * host builds      -> the desktop `.so`/`.a` produced by `tools/build_host.sh`
//                           (default: <repo>/build-host)
//     * android builds   -> the CMake output directory used by the Gradle app module
//                           (default: <repo>/build/native/<abi>, override with AF_CORE_LIB_DIR)
//
//  If no library is found the build still succeeds and prints a warning: that is the normal
//  situation when a Rust *static* library is linked into the Android app (Kotlin loads
//  libafcore.so at runtime, so the symbols resolve then, not at link time). cargo test on the
//  host, however, needs a real file - see rust/README.md.
// =============================================================================================

use std::env;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=AF_CORE_LIB_DIR");

    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let repo = manifest.parent().and_then(Path::parent).unwrap_or(Path::new(".")).to_path_buf();
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();

    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(explicit) = env::var("AF_CORE_LIB_DIR") {
        candidates.push(PathBuf::from(explicit));
    }
    // Rust's own output dir is scanned first so `cargo build` can consume a library that a
    // previous `cargo run -p af-tools` style workflow dropped there.
    if let Ok(out) = env::var("OUT_DIR") {
        candidates.push(PathBuf::from(out).join("afcore"));
    }
    if target_os == "android" {
        let abi = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_else(|_| "aarch64".into());
        let abi = match abi.as_str() {
            "aarch64" => "arm64-v8a",
            "arm" => "armeabi-v7a",
            "x86_64" => "x86_64",
            "x86" => "x86",
            other => other,
        };
        candidates.push(repo.join("build/native").join(abi));
        candidates.push(repo.join("build/native"));
    }
    candidates.push(repo.join("build-host"));
    candidates.push(repo.join("build"));

    let mut found: Option<PathBuf> = None;
    for dir in &candidates {
        let shared = dir.join("libafcore.so");
        let static_ = dir.join("libafcore.a");
        if shared.exists() || static_.exists() {
            found = Some(dir.clone());
            break;
        }
    }

    match found {
        Some(dir) => {
            println!("cargo:rustc-link-search=native={}", dir.display());
            // Published to dependents' build scripts as DEP_AF_CORE_LIB_DIR (the `links` key in
            // Cargo.toml), so the `af` crate can put the same directory on its binaries' rpath.
            println!("cargo:lib_dir={}", dir.display());
            if dir.join("libafcore.so").exists() {
                println!("cargo:rustc-link-lib=dylib=afcore");
                // Tests and binaries get an rpath so `cargo test` finds the library without
                // LD_LIBRARY_PATH gymnastics.
                println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
            } else {
                println!("cargo:rustc-link-lib=static=afcore");
                println!("cargo:rustc-link-lib=dylib=z");
                if !target_os.contains("android") {
                    println!("cargo:rustc-link-lib=dylib=pthread");
                    println!("cargo:rustc-link-lib=dylib=stdc++");
                } else {
                    println!("cargo:rustc-link-lib=dylib=c++_shared");
                }
            }
        }
        None => {
            println!("cargo:warning=libafcore was not found; build it first (./tools/build_host.sh \
                      for a desktop test build, or the Gradle app module for Android). Set \
                      AF_CORE_LIB_DIR to override the search path.");
        }
    }
}
