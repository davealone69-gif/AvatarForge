#!/usr/bin/env bash
# ==============================================================================================
#  AvatarForge :: tools/build_host.sh
# ==============================================================================================
#  Builds the kernel and the renderer for the machine you are sitting at, which is how the
#  project is developed and how the Rust crates and the test harness get something to link
#  against. Nothing here is used by the Android app: the app builds the same sources through
#  native/CMakeLists.txt and the NDK.
#
#      tools/build_host.sh              build everything (kernel + renderer + harness)
#      tools/build_host.sh kernel       build build-host/libafcore.so only
#      tools/build_host.sh test                   build and run the host test harness
#      tools/build_host.sh test --quick           ... skip the slower end-to-end sections
#      tools/build_host.sh test --compression-only ... run only profile/codec regressions
#
#  Outputs (all under build-host/, which is git-ignored):
#      libafcore.so     the kernel, with the full public C ABI from native/include/afcore.h
#      libafrender.so   the renderer is Android-only (EGL/ANativeWindow); on the desktop only the
#                       kernel is built, and the Rust `renderer` feature stays off.
#      host_tests       the 464-check host harness
# ==============================================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NATIVE="$ROOT/native"
OUT="$ROOT/build-host"
CXX="${CXX:-g++}"
JOBS="${JOBS:-2}"
TARGET="${1:-all}"

mkdir -p "$OUT"

# The kernel is deliberately compiled without -ffast-math: the isosurface extractor depends on
# IEEE float semantics for its watertightness guarantees (see native/CMakeLists.txt).
KERNEL_FLAGS=(-std=c++17 -O2 -fPIC -fno-math-errno -fno-finite-math-only -ffp-contract=off
              -Wall -Wextra -I"$NATIVE/include" -I"$NATIVE/src" -I"$NATIVE/src/render")
CODEC_FLAGS=(-I"$NATIVE/third_party/astcenc/Source"
             -I"$NATIVE/third_party/etc2comp/EtcLib/Etc"
             -I"$NATIVE/third_party/etc2comp/EtcLib/EtcCodec")

codec_sources() {
    local astc="$NATIVE/third_party/astcenc/Source"
    printf '%s\n' \
        "$astc/astcenc_averages_and_directions.cpp" \
        "$astc/astcenc_block_sizes.cpp" \
        "$astc/astcenc_color_quantize.cpp" \
        "$astc/astcenc_color_unquantize.cpp" \
        "$astc/astcenc_compress_symbolic.cpp" \
        "$astc/astcenc_compute_variance.cpp" \
        "$astc/astcenc_decompress_symbolic.cpp" \
        "$astc/astcenc_diagnostic_trace.cpp" \
        "$astc/astcenc_entry.cpp" \
        "$astc/astcenc_find_best_partitioning.cpp" \
        "$astc/astcenc_ideal_endpoints_and_weights.cpp" \
        "$astc/astcenc_image.cpp" \
        "$astc/astcenc_integer_sequence.cpp" \
        "$astc/astcenc_mathlib.cpp" \
        "$astc/astcenc_mathlib_softfloat.cpp" \
        "$astc/astcenc_partition_tables.cpp" \
        "$astc/astcenc_percentile_tables.cpp" \
        "$astc/astcenc_pick_best_endpoint_format.cpp" \
        "$astc/astcenc_quantization.cpp" \
        "$astc/astcenc_symbolic_physical.cpp" \
        "$astc/astcenc_weight_align.cpp" \
        "$astc/astcenc_weight_quant_xfer_tables.cpp"
    find "$NATIVE/third_party/etc2comp/EtcLib/Etc" \
         "$NATIVE/third_party/etc2comp/EtcLib/EtcCodec" -maxdepth 1 -name '*.cpp' -print | sort
}

kernel_sources() {
    # Every kernel translation unit: the app's CMakeLists.txt lists the same files explicitly.
    find "$NATIVE/src" -maxdepth 1 -name '*.cpp' | sort
}

build_kernel() {
    echo "==> libafcore.so ($(${CXX} --version | head -1))"
    # shellcheck disable=SC2046
    "$CXX" "${KERNEL_FLAGS[@]}" -shared -o "$OUT/libafcore.so" $(kernel_sources) -lz
    echo "    $(du -h "$OUT/libafcore.so" | cut -f1)  $OUT/libafcore.so"
}

build_tests() {
    echo "==> host_tests"
    # af_shaders.cpp is part of the renderer library, but the harness checks the baked shader
    # tables it defines (SPIR-V magic, stage coverage, GLSL ES text), so it is compiled in here.
    "$CXX" "${KERNEL_FLAGS[@]}" "${CODEC_FLAGS[@]}" -o "$OUT/host_tests" \
        "$NATIVE/tests/host_tests.cpp" $(kernel_sources) \
        "$NATIVE/src/render/af_shaders.cpp" \
        "$NATIVE/src/render/af_avatar_culler.cpp" "$NATIVE/src/render/af_avatar_lod.cpp" \
        "$NATIVE/src/render/af_gpu_profile.cpp" "$NATIVE/src/render/af_texture_compression.cpp" \
        $(codec_sources) -lz -pthread
    echo "    $OUT/host_tests"
}

case "$TARGET" in
    kernel) build_kernel ;;
    # `test` rebuilds the shared library as well: the harness compiles the sources directly, so
    # without this the probes (and the Rust crates) would keep linking yesterday's libafcore.so
    # while the harness reports a fresh pass - a stale-artifact trap that costs an hour to notice.
    test)   build_kernel; build_tests; echo; exec "$OUT/host_tests" "${@:2}" ;;
    all)    build_kernel; build_tests ;;
    *)      echo "usage: $0 [all|kernel|test]" >&2; exit 2 ;;
esac
