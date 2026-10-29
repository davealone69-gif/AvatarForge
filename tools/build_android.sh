#!/usr/bin/env bash
# ==============================================================================================
#  AvatarForge :: tools/build_android.sh
# ==============================================================================================
#
#  Builds only the native libraries for Android (no JDK, no Gradle, no Android Studio needed) and
#  installs them into prebuilt/<abi>/, which is exactly the directory :sdk consumes when Gradle is
#  invoked with -Paf.prebuiltJni. Two reasons to do it this way:
#
#    * the CMake/NDK build is the memory-hungry part of an Android build - doing it once with ninja
#      and then letting Gradle package prebuilt objects keeps a small machine (this container has
#      2 GB) inside its budget;
#    * a C++-only machine can produce the binaries, and the .so files can be handed to a
#      Kotlin/UI-only machine that never installs an NDK.
#
#  Usage:
#      tools/build_android.sh                 # arm64-v8a, android-26 (default)
#      tools/build_android.sh x86_64          # emulator ABI
#      tools/build_android.sh arm64-v8a android-29
#
#  Needs ANDROID_HOME (or ANDROID_SDK_ROOT) pointing at an SDK with NDK r26 and CMake 3.22.
#  Override the tools with AF_NDK / AF_CMAKE / AF_NINJA.
set -euo pipefail

ABI="${1:-arm64-v8a}"
PLATFORM="${2:-android-26}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
    echo "set ANDROID_HOME to your Android SDK (it must contain an NDK and CMake)" >&2
    exit 1
fi

# Newest NDK/CMake in the SDK unless the caller pinned one. The kernel needs NDK r26 or newer for
# the C++17 + GLES headers it uses; r23 will not do.
NDK="${AF_NDK:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)}"
CMAKE_BIN="${AF_CMAKE:-$(ls "$SDK"/cmake/*/bin/cmake 2>/dev/null | sort -V | tail -1 || true)}"
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "no NDK found under $SDK/ndk - install one with sdkmanager 'ndk;26.1.10909125'" >&2
    exit 1
fi
if [ -z "$CMAKE_BIN" ]; then
    echo "no CMake found under $SDK/cmake - install one with sdkmanager 'cmake;3.22.1'" >&2
    exit 1
fi
NINJA="${AF_NINJA:-$(dirname "$CMAKE_BIN")/ninja}"

echo "ABI      : $ABI"
echo "platform : $PLATFORM"
echo "NDK      : $NDK"
echo "cmake    : $CMAKE_BIN"
echo "ninja    : $NINJA"

# One directory per ABI; the default ABI keeps the historical `build-android` name so an existing
# configured tree is reused instead of recompiled from scratch.
if [ "$ABI" = "arm64-v8a" ]; then BUILD_DIR="$ROOT/build-android"; else BUILD_DIR="$ROOT/build-android-$ABI"; fi

# The STL is static on purpose: the APK then ships three .so files and no libc++_shared.so, which
# removes a whole class of "works on my device" runtime link failures.
"$CMAKE_BIN" -S "$ROOT/native" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$PLATFORM" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MAKE_PROGRAM="$NINJA"

# AF_NINJA_JOBS keeps a small machine inside its RAM budget: three C++17 translation units at -O3 in
# parallel is the peak of the whole build.
JOBS="${AF_NINJA_JOBS:-0}"
if [ "$JOBS" -gt 0 ]; then "$NINJA" -C "$BUILD_DIR" -j "$JOBS"; else "$NINJA" -C "$BUILD_DIR"; fi

mkdir -p "$ROOT/prebuilt/$ABI"
python3 "$ROOT/tools/verify_gles_artifacts.py" "$BUILD_DIR"
cp "$BUILD_DIR"/*.so "$ROOT/prebuilt/$ABI/"
echo
echo "installed into prebuilt/$ABI :"
ls -l "$ROOT/prebuilt/$ABI"
echo
echo "next: tools/build_gradle.sh :app:assembleDebug -Paf.abIs=$ABI -Paf.prebuiltJni=$ROOT/prebuilt"
