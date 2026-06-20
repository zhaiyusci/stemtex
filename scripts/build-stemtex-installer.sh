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
gui_root="${GUI_ROOT:-$repo_root/build/gui/Release}"
cpp_daemon_root="${CPP_DAEMON_ROOT:-$repo_root/build/cpp-daemon/Release}"
iscc="${ISCC:-}"

if [[ ! -f "$gui_root/stemtex-renderer-gui.exe" ]]; then
  echo "Renderer GUI build not found: $gui_root/stemtex-renderer-gui.exe" >&2
  exit 1
fi
if [[ ! -f "$cpp_daemon_root/stemtex-renderer.dll" || ! -f "$cpp_daemon_root/stemtex-renderer.lib" ]]; then
  echo "C++ renderer build not found under: $cpp_daemon_root" >&2
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
STAGE_ROOT="$stage_root" GUI_ROOT="$gui_root" CPP_DAEMON_ROOT="$cpp_daemon_root" \
  "$repo_root/scripts/stage-stemtex.sh" >/tmp/stemtex-installer-stage.log

MSYS2_ARG_CONV_EXCL='*' "$iscc" \
  "/DSourceDir=$(cygpath -w "$stage_root")" \
  "/DOutputDir=$(cygpath -w "$output_dir")" \
  "/DAppVersion=$app_version" \
  "$(cygpath -w "$repo_root/installer/stemtex.iss")"

installer="$output_dir/StemTeX-$app_version-Setup.exe"
test -f "$installer"
ls -lh "$installer"
