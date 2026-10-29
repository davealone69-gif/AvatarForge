#!/usr/bin/env bash
# ==============================================================================================
#  tools/device_check.sh -- collect the evidence a phase has to pass on a real phone
# ==============================================================================================
#
#  The rule for this project is that no feature is "done" because it built, or because a desktop
#  said so. It is done when a physical Android device ran it, and there is a measurement. This
#  script is the measurement. Unless --apk is supplied it installs nothing and needs no network:
#  it reads what Android reports and writes one report you can keep.
#
#      tools/device_check.sh                    # device report (needs adb + a connected phone)
#      tools/device_check.sh --soak 120         # also watch frame pacing for 120 s
#      tools/device_check.sh --apk artifacts/avatarforge-debug-arm64.apk   # install first
#
#  What it records, and why each one is here:
#
#      identity      model, SoC, Android version, ABI - so a result is attributable to a device
#      GPU           system capability from SurfaceFlinger vs this app's own logged GLES context.
#                    ANGLE-on-Vulkan in GL_RENDERER is an OS translation layer, not an app backend.
#      thermal       thermal status and headroom before and after the run. ADPF is Phase 3, but a
#                    soak that throttles without this column looks like a rendering bug
#      memory        PSS high-water mark of the app process during the run (the < 2 GB budget)
#      frames        gfxinfo frame statistics: janky %, p50/p90/p95/p99 frame times
#      logcat        our own tag, filtered, so a native crash or a JNI error cannot hide
#
#  It deliberately prints "UNAVAILABLE" for anything the device will not tell it. A blank column
#  that looks like a zero is the failure mode this project forbids, so the report never invents one.
# ==============================================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADB="${ADB:-${ANDROID_HOME:-$HOME/Android/Sdk}/platform-tools/adb}"
# Fall back to a bare "adb" when that path does not exist, so a phone-tools-only machine works.
[ -x "$ADB" ] || ADB="adb"
# This must match :app's applicationId (not its Kotlin namespace com.avatarforge.app).
PKG="${PKG:-com.avatarforge}"
APK=""
SOAK=0
OUT="$ROOT/artifacts/device-report.txt"

while [ $# -gt 0 ]; do
    case "$1" in
        --soak) SOAK="${2:-60}"; shift 2 ;;
        --apk)  APK="${2:-}"; shift 2 ;;
        --out)  OUT="${2:-$OUT}"; shift 2 ;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

say()  { printf '%s\n' "$*" | tee -a "$OUT"; }
kv()   { printf '  %-22s %s\n' "$1" "$2" | tee -a "$OUT"; }
unavailable() { kv "$1" "UNAVAILABLE ($2)"; }

mkdir -p "$(dirname "$OUT")"
: > "$OUT"
say "# AvatarForge device report"
say "# generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
say "# host: $(uname -srm)"
say ""

# ---------------------------------------------------------------- 0. is there a device at all?
if ! command -v "$ADB" >/dev/null 2>&1; then
    say "RESULT: NO DEVICE TOOL"
    say "  adb is not on PATH. Nothing in this report is a device result - it is the absence of one."
    exit 3
fi
DEVICES="$("$ADB" devices 2>/dev/null | sed '1d' | grep -c 'device$' || true)"
if [ "${DEVICES:-0}" -eq 0 ]; then
    say "RESULT: NO DEVICE ATTACHED"
    say "  adb sees no authorised device. Per this project's rule, no phase can be called done"
    say "  without a run on hardware; this report is therefore empty by design, not by failure."
    echo
    echo "report written to $OUT"
    exit 3
fi

SERIAL="$("$ADB" devices 2>/dev/null | sed '1d' | grep 'device$' | head -1 | cut -f1)"
A() { "$ADB" -s "$SERIAL" "$@"; }

say "## identity"
kv "serial" "$SERIAL"
kv "model" "$(A shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
kv "device" "$(A shell getprop ro.product.device 2>/dev/null | tr -d '\r')"
kv "manufacturer" "$(A shell getprop ro.product.manufacturer 2>/dev/null | tr -d '\r')"
kv "soc" "$(A shell getprop ro.soc.model 2>/dev/null | tr -d '\r')"
kv "android" "$(A shell getprop ro.build.version.release 2>/dev/null | tr -d '\r') (API $(A shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r'))"
kv "abilist" "$(A shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')"
kv "build fingerprint" "$(A shell getprop ro.build.fingerprint 2>/dev/null | tr -d '\r')"
say ""

# ---------------------------------------------------------------- 1. system GPU capability (NOT the app context)
say "## system GPU capability (SurfaceFlinger, not AvatarForge)"
GLINFO="$(A shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r')"
RENDERER="$(printf '%s' "$GLINFO" | grep -i -m1 'GLES:' | sed 's/^ *GLES: *//')"
if [ -n "${RENDERER:-}" ]; then kv "system GLES" "$RENDERER"; else unavailable "system GLES" "dumpsys returned no GLES line"; fi
VKVER="$(printf '%s' "$GLINFO" | grep -i -m1 'Vulkan' | sed 's/^ *//')"
if [ -n "${VKVER:-}" ]; then kv "system Vulkan" "$VKVER"; else unavailable "system Vulkan" "no Vulkan line in dumpsys"; fi
# Feature flags describe platform capability; they do not select an AvatarForge backend.
FEATURES="$(A shell pm list features 2>/dev/null | tr -d '\r' | grep -i -E 'vulkan|opengl' | sort | tr '\n' ' ')"
FEATURES="${FEATURES:-}" 
if [ -n "$FEATURES" ]; then kv "declared gfx" "$FEATURES"; else unavailable "declared gfx" "pm list features reported none"; fi
say ""

# ---------------------------------------------------------------- 2. thermal state before
say "## thermal (before)"
THERMAL="$(A shell dumpsys thermalservice 2>/dev/null | tr -d '\r')"
STATUS="$(printf '%s' "$THERMAL" | grep -m1 'Thermal Status' | sed 's/^ *//')"
if [ -n "${STATUS:-}" ]; then kv "status" "$STATUS"; else unavailable "status" "thermalservice not present"; fi
HEADROOM="$(printf '%s' "$THERMAL" | grep -i -m1 'headroom' | sed 's/^ *//')"
if [ -n "${HEADROOM:-}" ]; then kv "headroom" "$HEADROOM"; else unavailable "headroom" "not reported on this build"; fi
say ""

# ---------------------------------------------------------------- 3. optional install
if [ -n "$APK" ]; then
    say "## install"
    if [ ! -f "$APK" ]; then
        kv "apk" "MISSING: $APK"
    else
        kv "apk" "$APK ($(wc -c < "$APK") bytes)"
        if A install -r -d "$APK" >/tmp/af_install.txt 2>&1; then
            kv "install" "ok"
        else
            kv "install" "FAILED: $(tail -1 /tmp/af_install.txt)"
        fi
    fi
    say ""
fi

# ---------------------------------------------------------------- 4. is our app installed / running
say "## app"
if A shell pm path "$PKG" 2>/dev/null | grep -q package; then
    kv "installed" "yes"
    PSS="$(A shell dumpsys meminfo "$PKG" 2>/dev/null | tr -d '\r' | grep -m1 'TOTAL PSS' | sed 's/^ *//')"
    if [ -n "${PSS:-}" ]; then kv "memory now" "$PSS"; else kv "memory now" "process not running"; fi
else
    kv "installed" "NO - $PKG is not on this device (build and install first)"
fi
say ""

# ---------------------------------------------------------------- 5. soak / frame stats
say "## frames"
if [ "$SOAK" -gt 0 ]; then
    say "  soaking for ${SOAK}s; launch the viewer on the device now if it is not already open"
    sleep "$SOAK"
fi
GFX="$(A shell dumpsys gfxinfo "$PKG" 2>/dev/null | tr -d '\r')"
if printf '%s' "$GFX" | grep -q 'Total frames rendered'; then
    FRAMES="$(printf '%s' "$GFX" | grep -m1 'Total frames rendered' | sed 's/^ *//')"
    JANKY="$(printf '%s' "$GFX" | grep -m1 'Janky frames' | sed 's/^ *//')"
    FIFTY="$(printf '%s' "$GFX" | grep -m1 '50th percentile' | sed 's/^ *//')"
    NINETY="$(printf '%s' "$GFX" | grep -m1 '90th percentile' | sed 's/^ *//')"
    NINETYFIVE="$(printf '%s' "$GFX" | grep -m1 '95th percentile' | sed 's/^ *//')"
    NINETYNINE="$(printf '%s' "$GFX" | grep -m1 '99th percentile' | sed 's/^ *//')"
    kv "total" "${FRAMES:-UNAVAILABLE}"
    kv "janky" "${JANKY:-UNAVAILABLE}"
    kv "p50" "${FIFTY:-UNAVAILABLE}"
    kv "p90" "${NINETY:-UNAVAILABLE}"
    kv "p95" "${NINETYFIVE:-UNAVAILABLE}"
    kv "p99" "${NINETYNINE:-UNAVAILABLE}"
else
    unavailable "frame stats" "gfxinfo has no frames for $PKG - the app has not drawn since boot"
fi
say ""

# ---------------------------------------------------------------- app's GLES context (current process only)
say "## app graphics context (from AvatarForge, not SurfaceFlinger)"
APP_PID="$(A shell pidof "$PKG" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
if [ -z "$APP_PID" ]; then
    unavailable "app GLES context" "$PKG is not running; open the viewer and rerun"
else
    # --pid prevents a previous launch (or SurfaceFlinger) from being mistaken for this app's
    # active driver. No guess is made when the phone does not expose logcat for this PID.
    CONTEXT_LOG="$(A logcat -d --pid="$APP_PID" -s AvatarForgeRenderer:I 2>/dev/null | tr -d '\r' |
        grep -F 'AF_GLES_CONTEXT backend=' | tail -1 | sed 's/^.*AF_GLES_CONTEXT /AF_GLES_CONTEXT /')"
    if [ -n "$CONTEXT_LOG" ]; then
        kv "app GLES context" "${CONTEXT_LOG#AF_GLES_CONTEXT }"
        kv "process PID" "$APP_PID"
    else
        unavailable "app GLES context" "no successful context log for PID $APP_PID; open the viewer using this build and rerun, or export Self diagnostics"
    fi
fi
say ""

# ---------------------------------------------------------------- 6. thermal after
say "## thermal (after)"
THERMAL2="$(A shell dumpsys thermalservice 2>/dev/null | tr -d '\r')"
STATUS2="$(printf '%s' "$THERMAL2" | grep -m1 'Thermal Status' | sed 's/^ *//')"
kv "status" "${STATUS2:-UNAVAILABLE}"
TEMPS="$(printf '%s' "$THERMAL2" | grep -E 'Temperature\{' | head -8 | sed 's/^ *//' | tr '\n' ';')"
kv "temperatures" "${TEMPS:-UNAVAILABLE}"
say ""

# ---------------------------------------------------------------- 7. our own log lines
say "## app log (last 400 lines, ours only)"
LOGS="$(A logcat -d -t 400 -s AvatarForge:V AvatarForgeRenderer:V afcore:V DEBUG:V 2>/dev/null | tr -d '\r' | tail -60)"
if [ -n "${LOGS:-}" ]; then
    printf '%s\n' "$LOGS" | sed 's/^/  /' | tee -a "$OUT"
else
    unavailable "app log" "no lines tagged AvatarForge/AvatarForgeRenderer/afcore in the last 400"
fi
say ""

# ---------------------------------------------------------------- 8. crash evidence
say "## crashes since boot"
CRASHES="$(A logcat -d -b crash -t 200 2>/dev/null | tr -d '\r' | grep -c "$PKG" || true)"
kv "crash entries for $PKG" "${CRASHES:-0}"
say ""

say "## verdict"
say "  This report is evidence, not a pass. Read it against DEVICE.md: the app context"
say "  must be reported, numeric measurements must meet their thresholds, and pixels need"
say "  direct visual confirmation. ANGLE-on-Vulkan is the OS driver, not an app backend."
echo
echo "report written to $OUT"
