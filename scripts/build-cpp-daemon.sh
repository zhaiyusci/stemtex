#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$repo_root/build/cpp-daemon}"

win_path() {
  local p="$1"
  case "$p" in
    /mnt/[a-zA-Z]/*)
      local drive="${p:5:1}"
      printf '%s:/%s\n' "${drive^^}" "${p:8}"
      ;;
    /[a-zA-Z]/*)
      local drive="${p:1:1}"
      printf '%s:/%s\n' "${drive^^}" "${p:3}"
      ;;
    *)
      printf '%s\n' "$p"
      ;;
  esac
}

resolve_cmake_bin() {
  if [[ -n "${CMAKE_BIN:-}" ]]; then
    printf '%s\n' "$CMAKE_BIN"
    return
  fi
  for candidate in \
    /c/Qt/Tools/CMake_64/bin/cmake.exe \
    /mnt/c/Qt/Tools/CMake_64/bin/cmake.exe \
    "C:/Qt/Tools/CMake_64/bin/cmake.exe" \
    cmake; do
    if command -v "$candidate" >/dev/null 2>&1 || [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
  printf '%s\n' /c/Qt/Tools/CMake_64/bin/cmake.exe
}

cmake_bin="$(resolve_cmake_bin)"

if [[ ! -x "$cmake_bin" ]]; then
  echo "CMake not found: $cmake_bin" >&2
  exit 1
fi

"$cmake_bin" -S "$(win_path "$repo_root/cpp-daemon")" -B "$(win_path "$build_dir")" -G "Visual Studio 17 2022" -A x64
"$cmake_bin" --build "$(win_path "$build_dir")" --config Release --target stemtex-renderer stemtex-renderer-smoke --parallel "${JOBS:-8}"
