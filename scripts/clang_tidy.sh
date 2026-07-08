#!/usr/bin/env bash
set -euo pipefail

file="${1:?Usage: $0 <file.cpp> [check]}"

args=(
  -p build-tidy
  "$file"
  --extra-arg=-isystem/usr/include/c++/13
  --extra-arg=-isystem/usr/include/x86_64-linux-gnu/c++/13
)

if [[ $# -ge 2 ]]; then
  args+=("-checks=-*,$2")
fi

clang-tidy "${args[@]}"
