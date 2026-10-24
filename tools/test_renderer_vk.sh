#!/usr/bin/env bash
# Debian/Ubuntu: g++ libvulkan-dev mesa-vulkan-drivers vulkan-validationlayers libx11-dev xvfb xauth
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p build-host
bash tools/build_renderer_test_support.sh
"${CXX:-g++}" -std=c++17 -O1 -Wall -Wextra -I native/tests/support -I native/include -I native/src \
    native/tests/render_vk_tests.cpp native/src/render/af_shaders.cpp \
    -L build-host -Wl,-rpath,'$ORIGIN' -lafrender_test_support -lafcore -lvulkan -lX11 -pthread -o build-host/render_vk_tests
LVP="$(find /usr/share/vulkan/icd.d -name '*lvp*json' -print -quit)"
if [ -z "$LVP" ]; then echo 'Mesa lavapipe ICD not installed' >&2; exit 2; fi
export VK_ICD_FILENAMES="$LVP" LP_NUM_THREADS="${LP_NUM_THREADS:-2}"
for profile in default full; do
    if [ "$profile" = full ]; then export AF_RENDER_TEST_FULL_PROFILE=1
    else unset AF_RENDER_TEST_FULL_PROFILE; fi
    xvfb-run -a -s '-screen 0 640x480x24' build-host/render_vk_tests
done
