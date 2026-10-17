#!/usr/bin/env bash
# ==============================================================================================
#  AvatarForge :: tools/build_gradle.sh
# ==============================================================================================
#
#  One wrapper for every Gradle invocation, so the Android build is reproducible on a workstation,
#  on this container, or on a laptop. It pins the three things that otherwise differ per machine:
#
#    * JAVA_HOME  - AGP 8.5 needs JDK 17 (a JDK 11 first on PATH fails with an obscure error);
#    * SDK        - ANDROID_HOME / ANDROID_SDK_ROOT must point at a platform-34 SDK;
#    * RAM        - the daemon is capped, which keeps the build inside a 2 GB container.
#
#  Usage:
#      tools/build_gradle.sh :app:assembleDebug -Paf.abIs=arm64-v8a -Paf.prebuiltJni=$PWD/prebuilt
#      tools/build_gradle.sh :sdk:assembleRelease
#
#  Override any of the three with AF_JAVA_HOME / ANDROID_HOME / AF_GRADLE / AF_JVM_ARGS.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

export JAVA_HOME="${AF_JAVA_HOME:-/usr/local/tc/jdk17}"
export ANDROID_HOME="${ANDROID_HOME:-/usr/local/tc/android-sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-/usr/local/tc/gradle-home}"
GRADLE_BIN="${AF_GRADLE:-/usr/local/tc/gradle-8.9/bin/gradle}"
JVM_ARGS="${AF_JVM_ARGS:--Dorg.gradle.jvmargs=-Xmx1100m}"

if [ ! -x "$GRADLE_BIN" ]; then
    echo "gradle not found at $GRADLE_BIN - set AF_GRADLE to your gradle launcher" >&2
    exit 1
fi
if [ ! -d "$ANDROID_HOME/platforms/android-34" ]; then
    echo "no android-34 platform under $ANDROID_HOME - set ANDROID_HOME" >&2
    exit 1
fi

cd "$ROOT"
exec "$GRADLE_BIN" --no-daemon --console=plain "$JVM_ARGS" "$@"
