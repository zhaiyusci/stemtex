#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ptx_root="${PTX_ROOT:-$repo_root/ptx}"
texlive_root="${TEXLIVE_ROOT:-/c/texlive/2026}"

export PATH="$PATH:/ucrt64/bin:/usr/local/bin:/usr/bin:/bin"

run_make() {
  local dir="$1"
  shift || true
  echo "==> $dir: make -j 16 $*"
  (cd "$ptx_root/$dir" && make "$@")
}

run_sh() {
  local dir="$1"
  shift
  echo "==> $dir: sh $*"
  (cd "$ptx_root/$dir" && sh "$@")
}

need_file() {
  if [[ ! -f "$1" ]]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

need_file "$ptx_root/texk/web2c/Makefile"

if [[ -f "$texlive_root/texmf-dist/web2c/texmf.cnf" ]]; then
  cp -f "$texlive_root/texmf-dist/web2c/texmf.cnf" "$ptx_root/texk/web2c/texmf.cnf"
fi

touch "$ptx_root/libs/fontconfig/src/unistd.h"
touch "$ptx_root/libs/potrace-exec/unistd.h"

run_make "texk/kpathsea"
run_make "texk/ptexenc"

run_make "libs/zlib"
run_make "libs/libpng"
run_sh "libs/freetype" "w32build.sh"
run_make "libs/expat/lib"
run_make "libs/fontconfig/src"
run_sh "libs/fontconfig" "mkall.sh"
run_make "libs/pplib/src"
run_make "libs/graphite2-src/src"
run_make "libs/teckit"

run_make "libs/icu-src/source/common"
run_make "libs/icu-src/source/common" "install"
run_make "libs/icu-src/source/i18n"
run_make "libs/icu-src/source/i18n" "install"
run_make "libs/icu-src/source/io"
run_make "libs/icu-src/source/io" "install"
run_make "libs/icu-src/source/stubdata"
run_make "libs/icu-src/source/stubdata" "install"

run_make "libs/harfbuzz/harfbuzz-src/src"

run_make "texk/web2c/web2c" "makecpool.exe"
run_make "texk/web2c" "xetex.dll"

need_file "$ptx_root/texk/web2c/xetex.dll"
echo "built: $ptx_root/texk/web2c/xetex.dll"
