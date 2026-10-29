#!/usr/bin/env bash
# Device-report parsing test with a fake adb. This does not test a GPU or a Samsung device.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/adb" <<'ADB_STUB'
#!/usr/bin/env bash
set -eu
if [ "${1:-}" = "-s" ]; then shift 2; fi
case "$*" in
    devices) printf 'List of devices attached\nSAMSUNG-TEST\tdevice\n' ;;
    'shell getprop ro.product.model') echo 'SM-S921B' ;;
    shell\ getprop\ *) echo 'test' ;;
    'shell dumpsys SurfaceFlinger') printf '  GLES: SurfaceFlinger-only GPU\n  Vulkan: system capability\n' ;;
    'shell pm list features') printf 'feature:android.hardware.opengles.aep\nfeature:android.hardware.vulkan.level=1\n' ;;
    'shell dumpsys thermalservice') echo 'Thermal Status: 3' ;;
    'shell pm path com.avatarforge') echo 'package:/data/app/com.avatarforge/base.apk' ;;
    'shell dumpsys meminfo com.avatarforge') echo 'TOTAL PSS: 321000' ;;
    'shell dumpsys gfxinfo com.avatarforge') printf 'Total frames rendered: 60\nJanky frames: 2\n50th percentile: 16ms\n90th percentile: 33ms\n95th percentile: 35ms\n99th percentile: 50ms\n' ;;
    'shell pidof com.avatarforge') [ "${NO_APP:-0}" = 1 ] || echo '4252' ;;
    'logcat -d --pid=4252 -s AvatarForgeRenderer:I')
        echo 'I/AvatarForgeRenderer(4252): AF_GLES_CONTEXT backend=gles31 GL_RENDERER="ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304)" GL_VERSION="OpenGL ES 3.2" GL_VENDOR="Samsung"' ;;
    logcat\ -d\ -t\ 400\ -s\ *) echo 'I/AvatarForgeRenderer(4252): normal frame log' ;;
    'logcat -d -b crash -t 200') : ;;
    *) printf 'unexpected adb call: %s\n' "$*" >&2; exit 1 ;;
esac
ADB_STUB
chmod +x "$TMP/adb"

ADB="$TMP/adb" bash "$ROOT/tools/device_check.sh" --out "$TMP/active.txt" > /dev/null
grep -Eq '^  installed +yes$' "$TMP/active.txt"
grep -Fq 'system GLES            SurfaceFlinger-only GPU' "$TMP/active.txt"
grep -F 'app GLES context' "$TMP/active.txt" | grep -Fq 'backend=gles31 GL_RENDERER="ANGLE ((Samsung Xclipse 940) on Vulkan 1.3.304)"'
grep -Fq 'process PID            4252' "$TMP/active.txt"
if grep -Fq 'com.avatarforge.app' "$TMP/active.txt"; then
    echo 'FAIL: report queried the Kotlin namespace instead of the installed application ID' >&2
    exit 1
fi

NO_APP=1 ADB="$TMP/adb" bash "$ROOT/tools/device_check.sh" --out "$TMP/stopped.txt" > /dev/null
grep -F 'app GLES context' "$TMP/stopped.txt" | grep -Fq 'UNAVAILABLE (com.avatarforge is not running'
if grep -Fq 'backend=gles31' "$TMP/stopped.txt"; then
    echo 'FAIL: a stale or system driver string was attributed to a stopped app' >&2
    exit 1
fi
printf '%s\n' 'PASS: device report uses the APK ID and attributes GLES/ANGLE to the current app process (mock adb; no device)'
