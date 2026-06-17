#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$root/src/web2c"
prebuilt="$root/prebuilt-ucrt64"
out="${OUT_DIR:-$root/out/standalone-ucrt64}"
jobs="${JOBS:-16}"
link_mode="${LINK_MODE:-dynamic}"
strip_output="${STRIP_OUTPUT:-1}"

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
if [[ "$strip_output" == "1" ]]; then
  need_cmd strip
fi

if [[ "$link_mode" != "dynamic" && "$link_mode" != "static" ]]; then
  echo "LINK_MODE must be dynamic or static, got: $link_mode" >&2
  exit 1
fi

pkg_config_libs() {
  if [[ "$link_mode" == "static" ]]; then
    pkg-config --static --libs "$@"
  else
    pkg-config --libs "$@"
  fi
}

need_file "$src/xetexini.c"
need_file "$src/xetex0.c"
need_file "$src/xetex-pool.c"
need_file "$src/xetexdir/xetexextra.c"
need_file "$prebuilt/texk/kpathsea/.libs/libkpathsea.a"
need_file "$prebuilt/libs/teckit/libTECkit.a"
need_file "$prebuilt/libs/pplib/libpplib.a"
need_file "$prebuilt/texk/web2c/lib/lib.a"
need_file "$prebuilt/texk/web2c/libmd5.a"
need_file "$root/src/dvipdfm-x/dvipdfmx.c"
need_file "$root/src/libpaper/lib/paper.c"
need_file "$root/src/windows_mingw_wrapper/calldll.c"
need_file "$root/src/windows_mingw_wrapper/xetexdllmain.c"

mkdir -p "$out/obj"

pkg_cflags=(
  $(pkg-config --cflags freetype2 harfbuzz libpng fontconfig icu-i18n icu-uc)
)

common_flags=(
  -DHAVE_CONFIG_H
  -DNO_DEBUG
  -DU_STATIC_IMPLEMENTATION
  -DGRAPHITE2_STATIC=1
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
mkdir -p "$out/obj/windows_mingw_wrapper"
"$cc" -O2 -c "$root/src/windows_mingw_wrapper/xetexdllmain.c" \
  -o "$out/obj/windows_mingw_wrapper/xetexdllmain.o"

pkg_libs=(
  $(pkg_config_libs harfbuzz-subset harfbuzz graphite2 freetype2 libpng fontconfig icu-i18n icu-uc)
)

link_flags=()
if [[ "$link_mode" == "static" ]]; then
  link_flags=(-static-libgcc -static-libstdc++)
  pkg_libs=(-Wl,-Bstatic "${pkg_libs[@]}" -lz -liconv -Wl,-Bdynamic)
fi

zlib_link=(-lz)
if [[ "$link_mode" == "static" ]]; then
  zlib_link=(-Wl,-Bstatic -lz -Wl,-Bdynamic)
fi

xetex_link_inputs=(
  "$out/obj/xetexdir/xetexextra.o" \
  "$out/obj/xetexini.o" \
  "$out/obj/xetex0.o" \
  "$out/obj/xetex-pool.o" \
  "$out/obj/windows_mingw_wrapper/xetexdllmain.o" \
  "$libxetex" \
  "${pkg_libs[@]}" \
  "$prebuilt/libs/teckit/libTECkit.a" \
  "$prebuilt/libs/pplib/libpplib.a" \
  "$prebuilt/texk/web2c/libmd5.a" \
  "$prebuilt/texk/web2c/lib/lib.a" \
  "$prebuilt/texk/kpathsea/.libs/libkpathsea.a" \
  "${zlib_link[@]}" \
  -lwsock32
)

echo "LINK xetex.dll"
"$cxx" -shared -g -O2 -o "$out/xetex.dll" \
  -Wl,--out-implib,"$out/xetex.dll.a" \
  "${link_flags[@]}" \
  "${xetex_link_inputs[@]}"

echo "LINK xetex.exe"
"$cc" -O2 -s -DDLLPROC=dllxetexmain \
  "${link_flags[@]}" \
  "$root/src/windows_mingw_wrapper/calldll.c" \
  "$out/xetex.dll.a" \
  -o "$out/xetex.exe"

echo "built: $out/xetex.exe"
"$out/xetex.exe" --version | head -8 || true

echo "LINK xetexdaemon.dll"
"$cxx" -shared -g -O2 -o "$out/xetexdaemon.dll" \
  -Wl,--out-implib,"$out/xetexdaemon.dll.a" \
  "${link_flags[@]}" \
  "${xetex_link_inputs[@]}"

echo "LINK xetexdaemon.exe"
"$cc" -O2 -s -DDLLPROC=dllxetexmain \
  "${link_flags[@]}" \
  "$root/src/windows_mingw_wrapper/calldll.c" \
  "$out/xetexdaemon.dll.a" \
  -o "$out/xetexdaemon.exe"

echo "built: $out/xetexdaemon.exe"
"$out/xetexdaemon.exe" --version | head -8 || true

dvipdfmx_src="$root/src/dvipdfm-x"
libpaper_src="$root/src/libpaper/lib"

dvipdfmx_flags=(
  -DHAVE_CONFIG_H
  -DDVIPDFMX_STANDALONE_DLL
  -DWIN32
  -DPAPERSIZE=\"a4\"
  -DPAPERSIZEVAR=\"PAPERSIZE\"
  -DWINVER=0x0601
  -D_WIN32_WINNT=0x0601
  -I"$dvipdfmx_src"
  -I"$libpaper_src"
  -I"$prebuilt/texk"
  $(pkg-config --cflags libpng)
  -g
  -O2
)

dvipdfmx_sources=(
  agl.c
  bmpimage.c
  cff.c
  cff_dict.c
  cid.c
  cidtype0.c
  cidtype2.c
  cmap.c
  cmap_read.c
  cmap_write.c
  cs_type2.c
  dpxconf.c
  dpxcrypt.c
  dpxfile.c
  dpxutil.c
  dvi.c
  dvipdfmx.c
  epdf.c
  error.c
  fontmap.c
  jp2image.c
  jpegimage.c
  mem.c
  mfileio.c
  mpost.c
  mt19937ar.c
  numbers.c
  otl_opt.c
  pdfcolor.c
  pdfdev.c
  pdfdoc.c
  pdfdraw.c
  pdfencrypt.c
  pdfencoding.c
  pdffont.c
  pdfnames.c
  pdfobj.c
  pdfparse.c
  pdfresource.c
  pdfximage.c
  pkfont.c
  pngimage.c
  pst.c
  pst_obj.c
  sfnt.c
  spc_color.c
  spc_dvipdfmx.c
  spc_dvips.c
  spc_html.c
  spc_misc.c
  spc_pdfm.c
  spc_tpic.c
  spc_util.c
  spc_xtx.c
  specials.c
  subfont.c
  t1_char.c
  t1_load.c
  tfm.c
  truetype.c
  tt_aux.c
  tt_cmap.c
  tt_glyf.c
  tt_gsub.c
  tt_post.c
  tt_table.c
  type0.c
  type1.c
  type1c.c
  unicode.c
  vf.c
  xbb.c
)

for rel in "${dvipdfmx_sources[@]}"; do
  need_file "$dvipdfmx_src/$rel"
done

dvipdfmx_compile_env="$out/dvipdfmx-compile-env.sh"
{
  declare -p dvipdfmx_flags
  printf 'cc=%q\n' "$cc"
  printf 'out=%q\n' "$out"
  printf 'dvipdfmx_src=%q\n' "$dvipdfmx_src"
  printf 'libpaper_src=%q\n' "$libpaper_src"
  printf 'PATH=%q\n' "$PATH"
  cat <<'EOS'
compile_dvipdfmx_one() {
  local rel="$1"
  local srcfile obj
  case "$rel" in
    libpaper/*)
      srcfile="$libpaper_src/${rel#libpaper/}"
      obj="$out/obj/${rel%.c}.o"
      ;;
    *)
      srcfile="$dvipdfmx_src/$rel"
      obj="$out/obj/dvipdfm-x/${rel%.c}.o"
      ;;
  esac
  mkdir -p "$(dirname "$obj")"
  echo "CC  $rel"
  "$cc" "${dvipdfmx_flags[@]}" -c "$srcfile" -o "$obj"
}
EOS
} >"$dvipdfmx_compile_env"

printf '%s\n' "${dvipdfmx_sources[@]}" libpaper/paper.c libpaper/dimen.c |
  xargs -n1 -P "$jobs" bash -c "source '$dvipdfmx_compile_env'; compile_dvipdfmx_one \"\$0\""

dvipdfmx_link_inputs=(
  $(find "$out/obj/dvipdfm-x" -name '*.o' | sort) \
  "$out/obj/libpaper/paper.o" \
  "$out/obj/libpaper/dimen.o" \
  $(if [[ "$link_mode" == "static" ]]; then
      printf '%s\n' -Wl,-Bstatic
    fi) \
  $(pkg_config_libs libpng) \
  "$prebuilt/texk/kpathsea/.libs/libkpathsea.a" \
  $(if [[ "$link_mode" == "static" ]]; then
      printf '%s\n' -lz -Wl,-Bdynamic
    else
      printf '%s\n' -lz
    fi) \
  -lwsock32
)

echo "LINK dvipdfmxdaemon.dll"
"$cc" -shared -g -O2 -o "$out/dvipdfmxdaemon.dll" \
  -Wl,--out-implib,"$out/dvipdfmxdaemon.dll.a" \
  "${link_flags[@]}" \
  "${dvipdfmx_link_inputs[@]}"

echo "LINK xdvipdfmxdaemon.exe"
"$cc" -O2 -s -DDLLPROC=dlldvipdfmxmain \
  "${link_flags[@]}" \
  "$root/src/windows_mingw_wrapper/calldll.c" \
  "$out/dvipdfmxdaemon.dll.a" \
  -o "$out/xdvipdfmxdaemon.exe"

echo "built: $out/xdvipdfmxdaemon.exe"
"$out/xdvipdfmxdaemon.exe" --version | head -8 || true

if [[ "$strip_output" == "1" ]]; then
  echo "STRIP output binaries"
  strip "$out/xetex.dll" "$out/xetexdaemon.dll" "$out/dvipdfmxdaemon.dll" \
        "$out/xetex.exe" "$out/xetexdaemon.exe" "$out/xdvipdfmxdaemon.exe"
fi

echo "dependency audit:"
for bin in "$out/xetexdaemon.dll" "$out/dvipdfmxdaemon.dll" "$out/xetexdaemon.exe" "$out/xdvipdfmxdaemon.exe"; do
  if command -v objdump >/dev/null 2>&1; then
    echo "== $bin"
    objdump -p "$bin" | sed -n 's/^[[:space:]]*DLL Name: //p' | sort -u
  fi
done
