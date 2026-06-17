#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_bin="${CMAKE_BIN:-/c/Qt/Tools/CMake_64/bin/cmake.exe}"
build_dir="${BUILD_DIR:-$repo_root/build/cpp-daemon}"

if [[ ! -x "$cmake_bin" ]]; then
  echo "CMake not found: $cmake_bin" >&2
  exit 1
fi

"$cmake_bin" -S "$repo_root/cpp-daemon" -B "$build_dir" -G "Visual Studio 17 2022" -A x64
"$cmake_bin" --build "$build_dir" --config Release --target stemtex-renderer stemtex-renderer-smoke --parallel "${JOBS:-8}"

