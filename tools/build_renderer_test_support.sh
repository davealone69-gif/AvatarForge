#!/usr/bin/env bash
# Build the production renderer's CPU support/codec code once for the desktop driver tests.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p build-host
CORE=build-host/libafcore.so
if [ ! -s "$CORE" ] || [ tools/build_host.sh -nt "$CORE" ] || \
   [ -n "$(find native/src -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$CORE" -print -quit 2>/dev/null)" ] || \
   [ -n "$(find native/include -type f -newer "$CORE" -print -quit 2>/dev/null)" ]; then
    bash tools/build_host.sh kernel
fi
ASTC=native/third_party/astcenc/Source
ETC=native/third_party/etc2comp/EtcLib
SOURCES=(native/src/render/af_avatar_culler.cpp native/src/render/af_avatar_lod.cpp
         native/src/render/af_gpu_profile.cpp native/src/render/af_texture_compression.cpp)
while IFS= read -r source; do SOURCES+=("$source"); done < <(
    find "$ASTC" -maxdepth 1 -name 'astcenc_*.cpp' -print | sort
    find "$ETC/Etc" "$ETC/EtcCodec" -maxdepth 1 -name '*.cpp' -print | sort
)
OUTPUT=build-host/libafrender_test_support.so
if [ -s "$OUTPUT" ] && [ ! "${BASH_SOURCE[0]}" -nt "$OUTPUT" ] && \
   [ -z "$(find native/src native/include "$ASTC" "$ETC" \
        -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$OUTPUT" -print -quit)" ]; then
    exit 0
fi
"${CXX:-g++}" -std=c++17 -O1 -fPIC -shared -pthread \
    -I native/include -I native/src -I "$ASTC" -I "$ETC/Etc" -I "$ETC/EtcCodec" \
    "${SOURCES[@]}" -o "$OUTPUT"
