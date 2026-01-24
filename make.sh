#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="${1:-RelWithDebInfo}"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
else
  JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
fi
cmake --build build -- -j "${JOBS}"
ctest --test-dir build

./build/holder
