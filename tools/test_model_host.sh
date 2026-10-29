#!/usr/bin/env bash
# Focused model-detail regressions (hand frame, garment layer resolution).
# Builds the host kernel if needed, then links the test against it. No GPU/device required.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build-host"
CXX="${CXX:-g++}"
bash "$ROOT/tools/build_host.sh" kernel
"$CXX" -std=c++17 -O1 -Wall -Wextra -Werror \
    -I"$ROOT/native/include" -I"$ROOT/native/src" \
    -o "$OUT/model_detail_tests" "$ROOT/native/tests/model_detail_tests.cpp" \
    -L"$OUT" -lafcore -Wl,-rpath,"$OUT"
exec "$OUT/model_detail_tests"
