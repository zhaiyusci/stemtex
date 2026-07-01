#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${APP_VERSION:-}" ]]; then
  app_version="$APP_VERSION"
else
  app_version="$(tr -d '\r\n[:space:]' < "$repo_root/VERSION")"
fi
if [[ -z "$app_version" ]]; then
  echo "VERSION is empty" >&2
  exit 1
fi
stage_root="${STAGE_ROOT:-$repo_root/dist/stemtex-installer/StemTeX}"
output_dir="${OUTPUT_DIR:-$repo_root/dist/installer}"
build_dir="${BUILD_DIR:-$repo_root/build/stemtex}"
cmake_bin="${CMAKE:-cmake}"
iscc="${ISCC:-}"

if [[ ! -d "$build_dir" ]]; then
  echo "CMake build directory not found: $build_dir" >&2
  echo "Set BUILD_DIR=/path/to/build or configure/build the top-level CMake project first." >&2
  exit 1
fi

if [[ -z "$iscc" ]]; then
  local_appdata_candidate=""
  if [[ -n "${LOCALAPPDATA:-}" ]]; then
    local_appdata_candidate="$(cygpath -u "$LOCALAPPDATA")/Programs/Inno Setup 6/ISCC.exe"
  fi
  for candidate in \
    "$local_appdata_candidate" \
    "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "/c/Program Files/Inno Setup 6/ISCC.exe"; do
    [[ -z "$candidate" ]] && continue
    if [[ -x "$candidate" ]]; then
      iscc="$candidate"
      break
    fi
  done
fi
if [[ -z "$iscc" || ! -x "$iscc" ]]; then
  echo "ISCC.exe not found. Set ISCC=/path/to/ISCC.exe" >&2
  exit 1
fi

mkdir -p "$output_dir"
rm -rf "$stage_root"
"$cmake_bin" --install "$build_dir" --prefix "$stage_root" >/tmp/stemtex-installer-stage.log

MSYS2_ARG_CONV_EXCL='*' "$iscc" \
  "/DSourceDir=$(cygpath -w "$stage_root")" \
  "/DOutputDir=$(cygpath -w "$output_dir")" \
  "/DAppVersion=$app_version" \
  "$(cygpath -w "$repo_root/installer/stemtex.iss")"

installer="$output_dir/StemTeX-$app_version-Setup.exe"
test -f "$installer"
ls -lh "$installer"
