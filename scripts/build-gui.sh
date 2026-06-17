#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_bin="${CMAKE_BIN:-/c/Qt/Tools/CMake_64/bin/cmake.exe}"
build_dir="${BUILD_DIR:-$repo_root/build/gui}"
qt_prefix="${QT_PREFIX:-/c/Qt/6.11.1/msvc2022_64}"

if [[ ! -x "$cmake_bin" ]]; then
  echo "CMake not found: $cmake_bin" >&2
  exit 1
fi

"$cmake_bin" -S "$repo_root/gui" -B "$build_dir" -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_PREFIX_PATH="$qt_prefix"
"$cmake_bin" --build "$build_dir" --config Release --target stemtex-renderer-gui --parallel "${JOBS:-8}"
