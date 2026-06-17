#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$root/src/web2c"
prebuilt="$root/prebuilt-ucrt64"
out="${OUT_DIR:-$root/out/standalone-ucrt64}"
jobs="${JOBS:-16}"

export MSYSTEM="${MSYSTEM:-UCRT64}"
export PATH="/ucrt64/bin:/usr/bin:$PATH"

cc="${CC:-x86_64-w64-mingw32-gcc}"
cxx="${CXX:-x86_64-w64-mingw32-g++}"
ar="${AR:-ar}"
ranlib="${RANLIB:-ranlib}"

need_file() {
  [[ -f "$1" ]] || {
    echo "missing required file: $1" >&2
    exit 1
  }
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing command: $1" >&2
    exit 1
  }
}

need_cmd "$cc"
need_cmd "$cxx"
need_cmd "$ar"
need_cmd "$ranlib"
need_cmd pkg-config

need_file "$src/xetexini.c"
need_file "$src/xetex0.c"
need_file "$src/xetex-pool.c"
need_file "$src/xetexdir/xetexextra.c"
need_file "$prebuilt/texk/kpathsea/.libs/libkpathsea.a"
need_file "$prebuilt/libs/teckit/libTECkit.a"
need_file "$prebuilt/libs/pplib/libpplib.a"
need_file "$prebuilt/texk/web2c/lib/lib.a"
need_file "$prebuilt/texk/web2c/libmd5.a"

mkdir -p "$out/obj"

pkg_cflags=(
  $(pkg-config --cflags freetype2 harfbuzz libpng fontconfig icu-i18n icu-uc)
)

common_flags=(
  -DHAVE_CONFIG_H
  -DNO_DEBUG
  -DU_STATIC_IMPLEMENTATION
  -DWINVER=0x0601
  -D_WIN32_WINNT=0x0601
  -I"$src"
  -I"$src/w2c"
  -I"$src/xetexdir"
  -I"$src/libmd5"
  -I"$prebuilt/texk"
  -I"$prebuilt/libs/teckit/include"
  -I"$prebuilt/libs/pplib/include"
  "${pkg_cflags[@]}"
  -g
  -O2
)

c_flags=(-Wimplicit -Wreturn-type)
cxx_flags=(-Wreturn-type -Wno-write-strings)

compile_env="$out/compile-env.sh"
{
  declare -p common_flags c_flags cxx_flags
  printf 'cc=%q\n' "$cc"
  printf 'cxx=%q\n' "$cxx"
  printf 'src=%q\n' "$src"
  printf 'out=%q\n' "$out"
  printf 'PATH=%q\n' "$PATH"
  cat <<'EOS'
compile_one() {
  local rel="$1"
  local obj="$out/obj/${rel%.*}.o"
  mkdir -p "$(dirname "$obj")"
  case "$rel" in
    *.cpp)
      echo "CXX $rel"
      "$cxx" "${common_flags[@]}" "${cxx_flags[@]}" -c "$src/$rel" -o "$obj"
      ;;
    *.c)
      echo "CC  $rel"
      "$cc" "${common_flags[@]}" "${c_flags[@]}" -c "$src/$rel" -o "$obj"
      ;;
    *)
      echo "unsupported source: $rel" >&2
      return 1
      ;;
  esac
}
EOS
} >"$compile_env"

libxetex_sources=(
  xetexdir/XeTeXFontInst.cpp
  xetexdir/XeTeXFontMgr.cpp
  xetexdir/XeTeXLayoutInterface.cpp
  xetexdir/XeTeXOTMath.cpp
  xetexdir/XeTeX_ext.c
  xetexdir/XeTeX_pic.c
  xetexdir/trans.c
  xetexdir/hz.cpp
  xetexdir/pdfimage.cpp
  xetexdir/image/bmpimage.c
  xetexdir/image/jpegimage.c
  xetexdir/image/mfileio.c
  xetexdir/image/numbers.c
  xetexdir/image/pngimage.c
  xetexdir/XeTeXFontMgr_FC.cpp
)

top_sources=(
  xetexdir/xetexextra.c
  xetexini.c
  xetex0.c
  xetex-pool.c
)

for rel in "${libxetex_sources[@]}" "${top_sources[@]}"; do
  need_file "$src/$rel"
done

printf '%s\n' "${libxetex_sources[@]}" | xargs -n1 -P "$jobs" bash -c "source '$compile_env'; compile_one \"\$0\""

libxetex="$out/libxetex.a"
rm -f "$libxetex"
"$ar" cr "$libxetex" $(find "$out/obj/xetexdir" -name '*.o' | sort)
"$ranlib" "$libxetex"

printf '%s\n' "${top_sources[@]}" | xargs -n1 -P "$jobs" bash -c "source '$compile_env'; compile_one \"\$0\""

pkg_libs=(
  $(pkg-config --libs harfbuzz-subset harfbuzz graphite2 freetype2 libpng fontconfig icu-i18n icu-uc)
)

echo "LINK xetex.exe"
"$cxx" -g -O2 -o "$out/xetex.exe" \
  "$out/obj/xetexdir/xetexextra.o" \
  "$out/obj/xetexini.o" \
  "$out/obj/xetex0.o" \
  "$out/obj/xetex-pool.o" \
  "$libxetex" \
  "${pkg_libs[@]}" \
  "$prebuilt/libs/teckit/libTECkit.a" \
  "$prebuilt/libs/pplib/libpplib.a" \
  "$prebuilt/texk/web2c/libmd5.a" \
  "$prebuilt/texk/web2c/lib/lib.a" \
  "$prebuilt/texk/kpathsea/.libs/libkpathsea.a" \
  -lz -lwsock32

echo "built: $out/xetex.exe"
"$out/xetex.exe" --version | head -8 || true
