#!/usr/bin/env bash
# =============================================================================================
#  AvatarForge :: tools/syntax_check/run.sh
#
#  Desktop syntax-check for the Android-only native sources (renderer + JNI). The host test
#  harness (tools/build_host.sh) compiles the kernel, but the renderer needs EGL/GLES/Vulkan/
#  android headers that only exist in the NDK, so renderer commits could silently carry
#  compile errors all the way to a device install. This script catches them with a plain g++
#  using the minimal stub headers in tools/syntax_check/stubs/ plus the real Vulkan headers
#  (fetched from the Khronos repository into /tmp when absent - needs github.com reachable).
#
#  Usage:  bash tools/syntax_check/run.sh
#  Exit 0 = all translation units parse; non-zero = at least one has a compile error.
# =============================================================================================
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STUBS="$REPO_ROOT/tools/syntax_check/stubs"
VK_HEADERS="/tmp/vulkan-headers/include"

if [ ! -f "$VK_HEADERS/vulkan/vulkan.h" ]; then
    echo "Fetching Vulkan headers (KhronosGroup/Vulkan-Headers) into /tmp/vulkan-headers …"
    rm -rf /tmp/vulkan-headers
    git clone --depth 1 --filter=blob:none --sparse https://github.com/KhronosGroup/Vulkan-Headers /tmp/vulkan-headers \
        || { echo "ERROR: could not fetch Vulkan headers (github.com unreachable?)" >&2; exit 2; }
    ( cd /tmp/vulkan-headers && git sparse-checkout set include ) \
        || { echo "ERROR: sparse checkout failed" >&2; exit 2; }
fi

CXX="${CXX:-g++}"
COMMON_FLAGS=(-std=c++17 -fsyntax-only -Wall -Wno-unused-parameter -Wno-sign-compare
              -I"$STUBS" -I"$VK_HEADERS"
              -DVK_USE_PLATFORM_ANDROID_KHR)

TUS=(
    "$REPO_ROOT/native/src/render/af_render.cpp"
    "$REPO_ROOT/native/src/render/af_render_capi.cpp"
    "$REPO_ROOT/native/src/render/af_gl.cpp"
    "$REPO_ROOT/native/src/render/af_vk.cpp"
    "$REPO_ROOT/native/src/android/af_jni.cpp"
)

rc=0
for tu in "${TUS[@]}"; do
    echo "== $(basename "$tu")"
    if ! "$CXX" "${COMMON_FLAGS[@]}" "$tu" 2>&1; then
        rc=1
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "OK: all renderer/JNI translation units parse clean."
else
    echo "FAILED: compile errors above."
fi
exit "$rc"
