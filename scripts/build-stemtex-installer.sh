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
runtime_root="${RUNTIME_ROOT:-$repo_root/dist/stemtex-texlive-daemon-static}"
gui_root="${GUI_ROOT:-$repo_root/build/gui/Release}"
cpp_daemon_root="${CPP_DAEMON_ROOT:-$repo_root/build/cpp-daemon/Release}"
iscc="${ISCC:-}"

if [[ ! -d "$runtime_root" ]]; then
  echo "Runtime root not found: $runtime_root" >&2
  exit 1
fi
if [[ ! -f "$gui_root/stemtex-renderer-gui.exe" ]]; then
  echo "Renderer GUI build not found: $gui_root/stemtex-renderer-gui.exe" >&2
  exit 1
fi
if [[ ! -f "$cpp_daemon_root/stemtex-renderer.dll" || ! -f "$cpp_daemon_root/stemtex-renderer.lib" ]]; then
  echo "C++ renderer build not found under: $cpp_daemon_root" >&2
  exit 1
fi
if [[ ! -f "$runtime_root/texmf-dist/tex/latex/preview/preview.sty" ]]; then
  echo "Runtime is missing preview package: $runtime_root/texmf-dist/tex/latex/preview/preview.sty" >&2
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

rm -rf "$stage_root"
mkdir -p "$stage_root/runtime" "$stage_root/gui" "$output_dir"

cp -a "$runtime_root/." "$stage_root/runtime/"
cp -a "$gui_root/." "$stage_root/gui/"
rm -f "$stage_root/gui/stemtex-renderer.dll" "$stage_root/gui/stemtex-renderer.lib" "$stage_root/gui/stemtex-renderer.exp"
cp -f "$repo_root/cpp-daemon/worker-template.tex" "$stage_root/runtime/worker-template.tex"
rm -rf "$stage_root/runtime/profiles"
mkdir -p "$stage_root/runtime/profiles"
cp -a "$repo_root/profiles/." "$stage_root/runtime/profiles/"
find "$stage_root/runtime/profiles" -type f \( -name '*.aux' -o -name '*.log' -o -name '*.pdf' -o -name '*.synctex.gz' \) -delete
mkdir -p "$stage_root/runtime/sdk/include" "$stage_root/runtime/sdk/lib" "$stage_root/runtime/bin/sdk"
cp -f "$cpp_daemon_root/stemtex-renderer.dll" "$stage_root/runtime/bin/sdk/stemtex-renderer.dll"
cp -f "$cpp_daemon_root/stemtex-renderer.lib" "$stage_root/runtime/sdk/lib/stemtex-renderer.lib"
cp -f "$repo_root/cpp-daemon/stemtex_renderer.h" "$stage_root/runtime/sdk/include/stemtex_renderer.h"

rm -rf "$stage_root/runtime/texmf-var/fonts/cache"/*
rm -rf "$stage_root/runtime/texmf-var/cache-warmup"
mkdir -p "$stage_root/runtime/texmf-var/fonts/cache"

MSYS2_ARG_CONV_EXCL='*' "$iscc" \
  "/DSourceDir=$(cygpath -w "$stage_root")" \
  "/DOutputDir=$(cygpath -w "$output_dir")" \
  "/DAppVersion=$app_version" \
  "$(cygpath -w "$repo_root/installer/stemtex.iss")"

installer="$output_dir/StemTeX-$app_version-Setup.exe"
test -f "$installer"
ls -lh "$installer"
