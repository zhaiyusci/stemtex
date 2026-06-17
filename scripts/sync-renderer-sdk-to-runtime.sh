#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_root="${1:-$repo_root/dist/stemtex-texlive-daemon-static}"
cpp_daemon_root="${CPP_DAEMON_ROOT:-$repo_root/build/cpp-daemon/Release}"

if [[ ! -d "$runtime_root" ]]; then
  echo "Runtime root not found: $runtime_root" >&2
  exit 1
fi
if [[ ! -f "$cpp_daemon_root/stemtex-renderer.dll" || ! -f "$cpp_daemon_root/stemtex-renderer.lib" ]]; then
  echo "C++ renderer build not found under: $cpp_daemon_root" >&2
  exit 1
fi

mkdir -p "$runtime_root/bin/sdk" "$runtime_root/sdk/include" "$runtime_root/sdk/lib"
cp -f "$cpp_daemon_root/stemtex-renderer.dll" "$runtime_root/bin/sdk/stemtex-renderer.dll"
cp -f "$cpp_daemon_root/stemtex-renderer.lib" "$runtime_root/sdk/lib/stemtex-renderer.lib"
cp -f "$repo_root/cpp-daemon/stemtex_renderer.h" "$runtime_root/sdk/include/stemtex_renderer.h"

echo "Synced renderer SDK into: $runtime_root"
