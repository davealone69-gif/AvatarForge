#!/usr/bin/env bash
# ==============================================================================================
#  tools/prompt_parity.sh -- the C++ and the Rust prompt grammars must agree, word for word
# ==============================================================================================
#  Builds tools/prompt_probe.cpp against the C++ kernel's parser, runs the Rust `promptdump`
#  binary, and diffs the two over tools/prompt_corpus.txt. Any behaviour difference - a colour
#  attaching to a different noun, a measurement landing in a different field, a token count that
#  differs - is a failure with a readable diff.
#
#  Usage: tools/prompt_parity.sh            (full check)
#         tools/prompt_parity.sh --update   also rewrites the corpus if it does not exist
# ==============================================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CORPUS="tools/prompt_corpus.txt"
PROBE="build-host/prompt_probe"
OUT_DIR="build-host"
CPP_OUT="$OUT_DIR/prompt_cpp.txt"
RS_OUT="$OUT_DIR/prompt_rust.txt"

if [ ! -f "$CORPUS" ]; then
    echo "prompt_parity: missing $CORPUS" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
[ -f "$OUT_DIR/libafcore.so" ] || { echo "prompt_parity: build the kernel first (tools/build_host.sh kernel)" >&2; exit 1; }

echo "== C++ reference =="
g++ -std=c++17 -O2 -I native/include -I native/src tools/prompt_probe.cpp native/src/af_prompt.cpp \
    -L "$OUT_DIR" -lafcore -Wl,-rpath,"$PWD/$OUT_DIR" -o "$PROBE"
"$PROBE" "$CORPUS" > "$CPP_OUT"

echo "== Rust core =="
export RUSTUP_HOME="${RUSTUP_HOME:-/usr/local/tc/rustup}"
export CARGO_HOME="${CARGO_HOME:-/usr/local/tc/cargo-home}"
export PATH="$CARGO_HOME/bin:$PATH"
( cd rust && cargo run --release --offline -q -p af-core --bin promptdump -- "../$CORPUS" ) > "$RS_OUT"

echo "== diff =="
PROMPTS=$(grep -v '^[[:space:]]*#' "$CORPUS" | grep -cv '^[[:space:]]*$' || true)
if diff -u "$CPP_OUT" "$RS_OUT" > "$OUT_DIR/prompt_diff.txt"; then
    echo "PROMPT PARITY PASSED: $PROMPTS prompts, C++ and Rust identical"
else
    echo "PROMPT PARITY FAILED ($PROMPTS prompts) -- first differences:" >&2
    head -40 "$OUT_DIR/prompt_diff.txt" >&2
    echo "full diff: $OUT_DIR/prompt_diff.txt" >&2
    exit 1
fi
