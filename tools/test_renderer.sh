#!/usr/bin/env bash
# Production GLES-only integration test on headless Mesa (Debian/Ubuntu packages:
# g++ libegl1-mesa-dev libgles2-mesa-dev). Uses a pbuffer, not a mock GL implementation.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p build-host
bash tools/build_renderer_test_support.sh
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -I native/tests/support -I native/include -I native/src \
    native/tests/render_gl_tests.cpp native/src/render/af_shaders.cpp \
    -L build-host -Wl,-rpath,'$ORIGIN' -lafrender_test_support -lafcore -lEGL -lGLESv2 -pthread \
    -o build-host/render_gl_tests
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
# Limit llvmpipe workers on memory-constrained runners.
export LP_NUM_THREADS="${LP_NUM_THREADS:-2}"
for profile in default full; do
    if [ "$profile" = full ]; then export AF_RENDER_TEST_FULL_PROFILE=1
    else unset AF_RENDER_TEST_FULL_PROFILE; fi
    for alignment in 16 64 256; do build-host/render_gl_tests "$alignment"; done
done
