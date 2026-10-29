#!/usr/bin/env bash
# ==============================================================================================
#  AvatarForge :: tools/bootstrap_toolchain.sh
# ==============================================================================================
#  Installs the Android + Rust toolchain this project builds with, into $AF_TC (default
#  /usr/local/tc). Everything is pinned; nothing is fetched by the app itself.
#
#      tools/bootstrap_toolchain.sh            install everything
#      AF_TC=$HOME/tc tools/bootstrap_toolchain.sh
#
#  What it installs and why each piece is needed:
#
#      cmdline-tools   sdkmanager, the only supported way to fetch SDK packages
#      platform 34     compileSdk / targetSdk
#      build-tools 34  aapt2, d8, apksigner - what actually produces the APK
#      platform-tools  adb, for installing on a device
#      cmake 3.22.1    builds native/CMakeLists.txt (the NDK's own cmake is not on PATH)
#      ndk r26b        clang 17 + the GLES/EGL stub libraries the renderer links against
#      gradle 8.9      the AGP 8.5 build
#      jdk 17          AGP 8.x requires it; the system JDK is usually 11
#      kotlin 1.9.24   standalone compiler, used to type-check the SDK without Gradle
#      rustup+cargo    builds rust/{af-sys,af} against the same C ABI
#
#  Roughly 1.2 GB of downloads. Everything lands outside the source tree, so the workspace stays
#  small; re-running is cheap because each step is skipped when the target already exists.
# ==============================================================================================
set -euo pipefail

AF_TC="${AF_TC:-/usr/local/tc}"
JOBS="${JOBS:-2}"
mkdir -p "$AF_TC" "$AF_TC/downloads"
cd "$AF_TC"

fetch() {  # fetch <url> <target>
    local url="$1" target="$2"
    if [ -e "$target" ]; then echo "  have $(basename "$target")"; return; fi
    echo "  downloading $(basename "$target")"
    curl -fL --retry 3 --retry-delay 2 -o "$target.part" "$url"
    mv "$target.part" "$target"
}

# ---------------------------------------------------------------- JDK 17
if [ ! -x "$AF_TC/jdk17/bin/java" ]; then
    echo "==> JDK 17 (AGP 8.x requires it)"
    fetch "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse" \
          "$AF_TC/downloads/jdk17.tar.gz"
    mkdir -p "$AF_TC/jdk17"
    tar -xzf "$AF_TC/downloads/jdk17.tar.gz" -C "$AF_TC/jdk17" --strip-components=1
else
    echo "==> JDK 17 present"
fi
export JAVA_HOME="$AF_TC/jdk17"
export PATH="$JAVA_HOME/bin:$PATH"

# ---------------------------------------------------------------- Gradle
if [ ! -x "$AF_TC/gradle-8.9/bin/gradle" ]; then
    echo "==> Gradle 8.9"
    fetch "https://services.gradle.org/distributions/gradle-8.9-bin.zip" "$AF_TC/downloads/gradle-8.9-bin.zip"
    (cd "$AF_TC" && unzip -q -o "$AF_TC/downloads/gradle-8.9-bin.zip")
else
    echo "==> Gradle 8.9 present"
fi

# ---------------------------------------------------------------- Android SDK + NDK
if [ ! -x "$AF_TC/android-sdk/cmdline-tools/latest/bin/sdkmanager" ]; then
    echo "==> Android cmdline-tools"
    fetch "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" \
          "$AF_TC/downloads/cmdline-tools.zip"
    mkdir -p "$AF_TC/android-sdk/cmdline-tools"
    (cd "$AF_TC/android-sdk/cmdline-tools" && unzip -q -o "$AF_TC/downloads/cmdline-tools.zip" && mv cmdline-tools latest)
else
    echo "==> Android cmdline-tools present"
fi

export ANDROID_HOME="$AF_TC/android-sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
SDKMANAGER="$AF_TC/android-sdk/cmdline-tools/latest/bin/sdkmanager"

echo "==> SDK packages (platform 34, build-tools 34, platform-tools, cmake, ndk 26b)"
yes | "$SDKMANAGER" --sdk_root="$ANDROID_HOME" --licenses >/dev/null 2>&1 || true
"$SDKMANAGER" --sdk_root="$ANDROID_HOME" --install \
    "platforms;android-34" "build-tools;34.0.0" "platform-tools" "cmake;3.22.1" "ndk;26.1.10909125" >/dev/null

export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/26.1.10909125"

# ---------------------------------------------------------------- Kotlin (standalone)
if [ ! -x "$AF_TC/kotlinc/bin/kotlinc" ]; then
    echo "==> Kotlin 1.9.24 compiler"
    fetch "https://github.com/JetBrains/kotlin/releases/download/v1.9.24/kotlin-compiler-1.9.24.zip" \
          "$AF_TC/downloads/kotlin.zip"
    (cd "$AF_TC" && unzip -q -o "$AF_TC/downloads/kotlin.zip")
else
    echo "==> kotlinc present"
fi

# ---------------------------------------------------------------- Rust
export CARGO_HOME="$AF_TC/cargo-home"
export RUSTUP_HOME="$AF_TC/rustup"
if [ ! -x "$AF_TC/cargo/bin/cargo" ]; then
    echo "==> rustup + stable toolchain"
    fetch "https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init" "$AF_TC/downloads/rustup-init"
    chmod +x "$AF_TC/downloads/rustup-init"
    "$AF_TC/downloads/rustup-init" -y --no-modify-path --profile minimal \
        --default-toolchain stable >/dev/null
else
    echo "==> cargo present"
fi

cat <<EOF

Toolchain ready in $AF_TC

Add to your shell (or export per command):

    export JAVA_HOME=$AF_TC/jdk17
    export ANDROID_HOME=$AF_TC/android-sdk
    export ANDROID_SDK_ROOT=$AF_TC/android-sdk
    export ANDROID_NDK_HOME=$AF_TC/android-sdk/ndk/26.1.10909125
    export PATH=$AF_TC/jdk17/bin:$AF_TC/gradle-8.9/bin:$AF_TC/android-sdk/cmdline-tools/latest/bin:$AF_TC/android-sdk/platform-tools:$AF_TC/cargo/bin:\$PATH

Then:  cd $AF_TC/.. 2>/dev/null; cd \$(dirname "\$(realpath -m "\${BASH_SOURCE[0]:-.}")") 2>/dev/null
       tools/build_host.sh test        # kernel + 284-check harness (no device needed)
       gradle :app:assembleDebug       # APK
EOF
