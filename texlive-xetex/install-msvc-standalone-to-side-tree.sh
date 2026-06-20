#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/.." && pwd)"
standalone="${STANDALONE_DIR:-$root/out/standalone-msvc}"
tree="${STEMTEX_SIDE_TREE:-$repo/dist/stemtex-texlive-daemon-static}"
staged_texmf_dist="${STAGED_TEXMF_DIST:-$repo/staging/runtime/texmf-dist}"
bin="$tree/bin/windows"

die() {
  echo "error: $*" >&2
  exit 1
}

need_file() {
  [[ -f "$1" ]] || die "missing file: $1"
}

need_dir() {
  [[ -d "$1" ]] || die "missing directory: $1"
}

need_dir "$standalone"
mkdir -p "$tree"
if [[ -d "$staged_texmf_dist" ]]; then
  rm -rf "$tree/texmf-dist"
  mkdir -p "$tree"
  cp -a "$staged_texmf_dist" "$tree/"
elif [[ ! -d "$tree/texmf-dist" ]]; then
  die "missing staged texmf-dist: $staged_texmf_dist (run scripts/stage-stemtex.sh first)"
fi
mkdir -p "$bin"

for name in xetexdaemon.dll xetexdaemon.exe dvipdfmxdaemon.dll xdvipdfmxdaemon.exe; do
  need_file "$standalone/$name"
  cp -p "$standalone/$name" "$bin/$name"
done

rm -f "$bin/dvipdfmx.dll" "$bin/xdvipdfmx.exe"

icu_data="$root/prebuilt-msvc/share/icu-data/icudt78l.dat"
need_file "$icu_data"
mkdir -p "$bin/icu-data"
rm -f "$bin/icu-data"/icudt*.dat
cp -p "$icu_data" "$bin/icu-data/icudt78l.dat"

need_file "$tree/texmf-dist/web2c/texmf.cnf"
need_file "$tree/texmf-dist/dvipdfmx/dvipdfmx.cfg"
need_file "$tree/texmf-dist/tex/latex/preview/preview.sty"

if [[ "${PRUNE_OLD_RUNTIME_DLLS:-0}" == "1" ]]; then
  rm -f \
    "$bin"/api-ms-win-crt-*.dll \
    "$bin"/libbrotli*.dll \
    "$bin"/libbz2-1.dll \
    "$bin"/libexpat-1.dll \
    "$bin"/libfontconfig-1.dll \
    "$bin"/libfreetype-6.dll \
    "$bin"/libgcc_s_seh-1.dll \
    "$bin"/libglib-2.0-0.dll \
    "$bin"/libgraphite2.dll \
    "$bin"/libharfbuzz-0.dll \
    "$bin"/libiconv-2.dll \
    "$bin"/libicu*.dll \
    "$bin"/libintl-8.dll \
    "$bin"/libpcre2-8-0.dll \
    "$bin"/libpng16-16.dll \
    "$bin"/libstdc++-6.dll \
    "$bin"/libwinpthread-1.dll \
    "$bin"/msvcp140.dll \
    "$bin"/ucrtbase.dll \
    "$bin"/vcruntime140*.dll \
    "$bin"/zlib1.dll
fi

echo "installed MSVC standalone binaries into: $bin"
echo "using texmf.cnf: $tree/texmf-dist/web2c/texmf.cnf"
