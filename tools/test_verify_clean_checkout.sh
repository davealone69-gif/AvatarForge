#!/usr/bin/env bash
# Regression: tools/verify.sh must create its ignored log directory before redirecting output.
# Run against an isolated fixture so no real builds, devices, network, or repository files are touched.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FIXTURE="$TMP/repo"
mkdir -p "$TMP/bin" "$FIXTURE/tools" "$FIXTURE/rust"
cp "$ROOT/tools/verify.sh" "$FIXTURE/tools/verify.sh"

cat > "$TMP/bin/bash" <<'BASH_STUB'
#!/bin/sh
case "${1:-}" in
    tools/build_host.sh) printf '%s\n' 'All 411 checks complete: 0 failure(s)' ;;
    tools/test_renderer_host.sh) printf '%s\n' 'PASS: renderer host mock' ;;
    tools/test_device_check_host.sh) printf '%s\n' 'PASS: device report mock' ;;
    tools/test_model_host.sh) printf '%s\n' 'PASS: model detail mock' ;;
    tools/prompt_parity.sh) printf '%s\n' 'PROMPT PARITY PASSED: 72 prompts, C++ and Rust identical' ;;
    *) printf 'unexpected nested bash invocation: %s\n' "$*" >&2; exit 99 ;;
esac
BASH_STUB

cat > "$TMP/bin/python3" <<'PYTHON_STUB'
#!/bin/sh
case "${1:-}" in
    tools/embed_shaders.py) echo 'PASS: 12 shader stages; embedded table matches' ;;
    tools/abi_manifest.py) echo 'PASS: ABI manifest current' ;;
    tools/abi_conformance.py) echo 'CONFORMANCE PASS' ;;
    tools/validate_glb.py) echo 'OK: structural checks passed' ;;
    tools/glb_census.py) exit 0 ;;
    *) printf 'unexpected python invocation: %s\n' "$*" >&2; exit 98 ;;
esac
PYTHON_STUB

cat > "$TMP/bin/cargo" <<'CARGO_STUB'
#!/bin/sh
exit 0
CARGO_STUB
chmod +x "$TMP/bin/bash" "$TMP/bin/python3" "$TMP/bin/cargo"

(
    cd "$FIXTURE"
    PATH="$TMP/bin:/usr/bin:/bin" /bin/bash tools/verify.sh --quick
) > "$TMP/verify.out" 2>&1 || {
    cat "$TMP/verify.out"
    echo 'FAIL: verifier did not complete from a clean fixture' >&2
    exit 1
}

test -f "$FIXTURE/build-host/host_tests_run.txt" || {
    cat "$TMP/verify.out"
    echo 'FAIL: verifier did not create its host log file' >&2
    exit 1
}
grep -Fq 'All host-side checks passed.' "$TMP/verify.out" || {
    cat "$TMP/verify.out"
    echo 'FAIL: verifier did not report a clean run' >&2
    exit 1
}
printf '%s\n' 'PASS: verifier creates build-host/ before its first redirected log on a fresh checkout'
