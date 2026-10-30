#!/usr/bin/env bash
# Compile the real GLES path and native backend selector against a stateful GLES/EGL mock
# and a factory-only Vulkan double. This is NOT a Vulkan command or device test.
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
    "$ROOT/native/tests/render/vulkan_factory_mock.cpp" \
    "$ROOT/native/src/render/af_gl.cpp" \
    "$ROOT/native/src/render/af_render.cpp" \
    "$ROOT/native/src/render/af_shaders.cpp"
"$OUT/renderer_init_tests"

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}"
if [ -z "$NDK" ]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -n "$SDK" ]; then NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"; fi
fi
VULKAN_HEADERS="${NDK:+$NDK/sources/third_party/vulkan/src/include}"
if [ -n "$VULKAN_HEADERS" ] && [ -f "$VULKAN_HEADERS/vulkan/vulkan.h" ]; then
    "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror \
        -I"$ROOT/native/src/render" -I"$VULKAN_HEADERS" \
        -o "$OUT/vulkan_version_tests" \
        "$ROOT/native/tests/render/vulkan_version_tests.cpp"
    "$OUT/vulkan_version_tests"
else
    echo "SKIP Vulkan version helper unit test (pinned NDK Vulkan headers not found)"
fi
