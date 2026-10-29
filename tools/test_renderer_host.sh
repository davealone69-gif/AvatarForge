#!/usr/bin/env bash
# Compile the real GLES initialization/fallback code against a stateful API test double.
# No Android SDK, GPU, network, or kernel generation is required. This is NOT a device test.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build-host"
CXX="${CXX:-g++}"
mkdir -p "$OUT"
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -I"$ROOT/native/include" -I"$ROOT/native/src" -I"$ROOT/native/src/render" \
    -I"$ROOT/native/tests/render_stub_headers" \
    -o "$OUT/renderer_init_tests" \
    "$ROOT/native/tests/render/init_tests.cpp" \
    "$ROOT/native/tests/render/gl_mock.cpp" \
    "$ROOT/native/src/render/af_gl.cpp" \
    "$ROOT/native/src/render/af_render.cpp" \
    "$ROOT/native/src/render/af_shaders.cpp"
exec "$OUT/renderer_init_tests"
