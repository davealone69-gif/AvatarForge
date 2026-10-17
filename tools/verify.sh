#!/usr/bin/env bash
# ==============================================================================================
#  AvatarForge :: tools/verify.sh
# ==============================================================================================
#
#  Re-runs every host-side acceptance check in one go, so "is this repository green?" has a single
#  answer. Nothing here needs a device or an emulator, and nothing here touches the network.
#
#      tools/verify.sh              # kernel harness + shaders + exported model + artifact audit
#      tools/verify.sh --quick      # skip the slow geometry-heavy kernel sections
#
#  Each section prints a PASS/FAIL line and the script exits non-zero on the first failure, so it is
#  usable as a pre-commit hook or a CI step (a CI runner with no Android SDK still passes sections
#  1-2 and skips the rest with a clear message).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
QUICK="${1:-}"
FAILED=0
SECTION=0

banner() { SECTION=$((SECTION + 1)); printf '\n\033[1m[%d] %s\033[0m\n' "$SECTION" "$1"; }
ok()     { printf '    \033[32mPASS\033[0m %s\n' "$1"; }
bad()    { printf '    \033[31mFAIL\033[0m %s\n' "$1"; FAILED=1; }
skip()   { printf '    \033[33mSKIP\033[0m %s\n' "$1"; }

# ------------------------------------------------------------------ 1. kernel + host harness ----
banner "kernel: build + host test harness"
# --quick forwards to the harness, which then skips the pose-composition section and its summary
# line; the exit code is authoritative either way.
HARNESS_ARGS=""
[ "$QUICK" = "--quick" ] && HARNESS_ARGS="--quick"
if bash tools/build_host.sh test $HARNESS_ARGS > build-host/host_tests_run.txt 2>&1; then
    SUMMARY="$(grep -o 'All [0-9]* checks complete: 0 failure(s)' build-host/host_tests_run.txt | tail -1)"
    if [ -z "$SUMMARY" ]; then SUMMARY="kernel sections 1-11 passed (--quick)"; fi
    ok "$SUMMARY"
    grep -E "^     .*(vertices|deviation|bake:|charts=)|^\[(4|5|7|11|12)\]" build-host/host_tests_run.txt \
        | sed 's/\x1b\[[0-9;]*m//g' | sed 's/^/   /' | head -14
else
    bad "harness reported failures - see build-host/host_tests_run.txt"
    grep -n "FAIL" build-host/host_tests_run.txt | head -10 || tail -20 build-host/host_tests_run.txt
fi

# ------------------------------------------------------------------ 2. shader tables ------------
banner "shaders: SPIR-V + GLSL ES tables match the sources"
# glslc and spirv-val ship inside the NDK; make sure the Python script can see them even when
# neither ANDROID_NDK nor ANDROID_NDK_HOME is exported (ANDROID_HOME alone is the common case).
if [ -z "${ANDROID_NDK:-}" ] && [ -z "${ANDROID_NDK_HOME:-}" ]; then
    ANDROID_NDK="$(ls -d "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/nonexistent}}"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
    export ANDROID_NDK
fi
if python3 tools/embed_shaders.py --check > /tmp/af_shader_check.txt 2>&1; then
    ok "$(tail -1 /tmp/af_shader_check.txt)"
else
    if grep -qi "glslc" /tmp/af_shader_check.txt; then
        skip "glslc not found (comes with the NDK's shader-tools)"
    else
        bad "shader tables are stale or a stage failed to compile"
        tail -12 /tmp/af_shader_check.txt | sed 's/^/      /'
    fi
fi

# ------------------------------------------------------------------ 3. exported model -----------
banner "export: a produced .glb validates"
# `out/` is scratch and is not kept between sessions, so a sample export that must survive lives
# in artifacts/. Prefer whichever exists, largest first (the newest export is the biggest).
GLB="$(ls -S out/*.glb artifacts/*.glb 2>/dev/null | head -1 || true)"
if [ -z "$GLB" ] && command -v g++ >/dev/null 2>&1 && [ "$QUICK" != "--quick" ]; then
    # Nothing exported yet: produce one through the public C ABI (geometry + rig + GLB, ~25 s) so
    # this section is a real end-to-end proof rather than a skip on a fresh checkout.
    printf '      building tools/export_probe.cpp and exporting a fresh model ...\n'
    mkdir -p out
    if g++ -std=c++17 -O2 -o build-host/export_probe tools/export_probe.cpp \
            -Inative/include -Lbuild-host -lafcore -Wl,-rpath,"$ROOT/build-host" >/dev/null 2>&1 &&
       build-host/export_probe "$ROOT/out/verify.glb" >/dev/null 2>&1; then
        GLB="$ROOT/out/verify.glb"
    else
        bad "export through the C ABI failed (see tools/export_probe.cpp)"
    fi
fi
if [ -n "$GLB" ]; then
    if python3 tools/validate_glb.py "$GLB" > /tmp/af_glb_check.txt 2>&1; then
        ok "$GLB: $(tail -1 /tmp/af_glb_check.txt)"
    else
        bad "GLB validation failed for $GLB"
        tail -12 /tmp/af_glb_check.txt | sed 's/^/      /'
    fi
    python3 tools/glb_census.py "$GLB" 2>/dev/null | head -12 | sed 's/^/      /'
else
    skip "no .glb in out/ - run tools/export_probe.cpp or export from the app first"
fi

# ------------------------------------------------------------------ 4. shared C ABI ------------
# The ABI is the contract both native implementations answer to. This section proves the manifest
# still matches the header, that the C++ kernel is a complete 45/45 implementation, and - when a
# Rust toolchain is present - that the Rust core agrees with it on every symbol it implements.
banner "ABI: manifest freshness + conformance for both kernels"
if python3 tools/abi_manifest.py --check > /tmp/af_abi_manifest.txt 2>&1; then
    ok "$(tail -1 /tmp/af_abi_manifest.txt)"
else
    bad "abi/afcore.abi.json is stale - run python3 tools/abi_manifest.py"
    tail -6 /tmp/af_abi_manifest.txt | sed 's/^/      /'
fi

if [ -f build-host/libafcore.so ]; then
    if python3 tools/abi_conformance.py build-host/libafcore.so --require-complete \
            > /tmp/af_abi_cpp.txt 2>&1; then
        ok "C++ kernel: $(tail -1 /tmp/af_abi_cpp.txt | sed 's/CONFORMANCE //')"
    else
        bad "C++ kernel fails the ABI contract"
        tail -10 /tmp/af_abi_cpp.txt | sed 's/^/      /'
    fi
fi

export RUSTUP_HOME="${RUSTUP_HOME:-/usr/local/tc/rustup}"
export CARGO_HOME="${CARGO_HOME:-/usr/local/tc/cargo-home}"
export PATH="$CARGO_HOME/bin:$PATH"
if command -v cargo >/dev/null 2>&1; then
    if ( cd rust && cargo build --release --offline -q -p af-core ) > /tmp/af_rust_build.txt 2>&1; then
        if python3 tools/abi_conformance.py rust/target/release/libafcore.so \
                --reference build-host/libafcore.so > /tmp/af_abi_rust.txt 2>&1; then
            ok "Rust core: $(tail -1 /tmp/af_abi_rust.txt | sed 's/CONFORMANCE //')"
        else
            bad "Rust core drifts from the ABI or the reference kernel"
            grep -E "FAIL|reference .* vs candidate" /tmp/af_abi_rust.txt | head -10 | sed 's/^/      /'
        fi
        if bash tools/prompt_parity.sh > /tmp/af_prompt_parity.txt 2>&1; then
            ok "$(tail -1 /tmp/af_prompt_parity.txt)"
        else
            bad "the C++ and Rust prompt grammars disagree"
            tail -12 /tmp/af_prompt_parity.txt | sed 's/^/      /'
        fi
    else
        bad "the Rust core does not build - see /tmp/af_rust_build.txt"
        tail -8 /tmp/af_rust_build.txt | sed 's/^/      /'
    fi
else
    skip "no cargo on PATH - the Rust core was not checked (see BUILD.md)"
fi

# ------------------------------------------------------------------ 5. artifacts -----------------
banner "artifacts: APKs and the SDK AAR"
for f in artifacts/avatarforge-debug-arm64.apk artifacts/avatarforge-release-arm64.apk artifacts/avatarforge-sdk-arm64.aar; do
    if [ -f "$f" ]; then
        printf '      %-46s %10d bytes  sha256 %s\n' "$f" "$(stat -c%s "$f")" "$(sha256sum "$f" | cut -c1-16)"
    else
        skip "$f not built yet"
    fi
done

# The one artifact claim that must never regress: no INTERNET permission, anywhere.
AAPT="$(ls "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/nonexistent}}"/build-tools/*/aapt2 2>/dev/null | sort -V | tail -1 || true)"
if [ -n "$AAPT" ] && [ -x "$AAPT" ]; then
    for apk in artifacts/*.apk; do
        [ -f "$apk" ] || continue
        PERMS="$("$AAPT" dump permissions "$apk" 2>/dev/null)"
        if printf '%s' "$PERMS" | grep -q "android.permission.INTERNET"; then
            bad "$apk requests INTERNET - the whole project fails on this"
        else
            ok "$(basename "$apk"): no INTERNET permission"
        fi
        # The listing is captured once and matched with a case statement: `unzip -l | grep -q`
        # under `set -o pipefail` reports a failure whenever grep finds the match and exits early,
        # because unzip is then killed by SIGPIPE (exit 141).
        LISTING="$(unzip -l "$apk" 2>/dev/null || true)"
        for so in libafcore libafrender libavatarforge; do
            case "$LISTING" in
                *"lib/arm64-v8a/$so.so"*) ;;
                *) bad "$(basename "$apk") is missing $so.so" ;;
            esac
        done
    done
else
    skip "aapt2 not found (comes with the SDK build-tools)"
fi

# ------------------------------------------------------------------ summary ----------------------
if [ "$FAILED" -eq 0 ]; then
    printf '\n\033[32m\033[1mAll host-side checks passed.\033[0m  Device pass: see DEVICE.md\n\n'
else
    printf '\n\033[31m\033[1mAt least one check failed - see the section above.\033[0m\n\n'
fi
exit "$FAILED"
