#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$root/src/web2c"
prebuilt="$root/prebuilt-msvc"
out="${OUT_DIR:-$root/out/standalone-msvc}"
jobs="${JOBS:-16}"
vswhere_win="${VSWHERE:-C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe}"

mkdir -p "$out/obj" "$out/lib"

die() {
  echo "error: $*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

need_file() {
  [[ -f "$1" ]] || die "missing file: $1"
}

need_cmd cygpath
need_cmd xargs
need_cmd find

find_vcvars() {
  local vswhere_msys install vcvars vcvars_msys
  vswhere_msys="$(cygpath -u "$vswhere_win")"
  need_file "$vswhere_msys"
  install="$("$vswhere_msys" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r' | tail -n1)"
  [[ -n "$install" ]] || die "Visual Studio with VC tools not found"
  vcvars="$install\\VC\\Auxiliary\\Build\\vcvars64.bat"
  vcvars_msys="$(cygpath -u "$vcvars")"
  need_file "$vcvars_msys"
  printf '%s\n' "$vcvars"
}

vcvars_win="$(find_vcvars)"

run_vc() {
  local workdir="$1"
  shift
  local workdir_win bat bat_win
  workdir_win="$(cygpath -w "$workdir")"
  bat="$out/run-vc-$RANDOM-$RANDOM.cmd"
  cat >"$bat" <<EOF
@echo off
call "$vcvars_win" >nul || exit /b 1
set CL=
set _CL_=
cd /d "$workdir_win" || exit /b 1
$*
EOF
  bat_win="$(cygpath -w "$bat")"
  echo "==> $*"
  cmd.exe //D //S //C "$bat_win"
}

msys_to_win_list() {
  local out=()
  local item
  for item in "$@"; do
    out+=("$(cygpath -w "$item")")
  done
  printf '%s ' "${out[@]}"
}

need_file "$prebuilt/lib/kpathsea.lib"
need_file "$prebuilt/texk/web2c/lib/lib.lib"
need_file "$prebuilt/texk/web2c/libmd5.lib"
need_file "$prebuilt/lib/pplib.lib"
need_file "$prebuilt/lib/TECkit.lib"
need_file "$prebuilt/lib/fontconfig.lib"
need_file "$prebuilt/lib/freetype.lib"
need_file "$prebuilt/lib/harfbuzz.lib"
need_file "$prebuilt/lib/harfbuzz-icu.lib"
need_file "$prebuilt/lib/graphite2.lib"
need_file "$prebuilt/lib/libpng16_static.lib"
need_file "$prebuilt/lib/zs.lib"
need_file "$prebuilt/lib/expat.lib"
need_file "$prebuilt/lib/icuin.lib"
need_file "$prebuilt/lib/icuuc.lib"
need_file "$prebuilt/lib/icudt.lib"
need_file "$root/src/windows_mingw_wrapper/calldll.c"

common_includes=(
  "$out/msvc-config"
  "$src"
  "$src/w2c"
  "$src/xetexdir"
  "$src/libmd5"
  "$prebuilt/texk"
  "$prebuilt/include"
  "$prebuilt/include/harfbuzz"
  "$prebuilt/include/freetype2"
  "$prebuilt/include/libpng16"
  "$prebuilt/libs/teckit/include"
  "$prebuilt/libs/pplib/include"
)

mkdir -p "$out/msvc-config/w2c"
awk '
  /^#define HAVE_SYS_TIME_H / { print "/* #undef HAVE_SYS_TIME_H */"; next }
  /^#define HAVE_UNISTD_H / { print "/* #undef HAVE_UNISTD_H */"; next }
  { print }
' "$src/w2c/c-auto.h" >"$out/msvc-config/w2c/c-auto.h"

make_include_flags_win() {
  local flags=()
  local dir
  for dir in "${common_includes[@]}"; do
    flags+=("/I\"$(cygpath -w "$dir")\"")
  done
  printf '%s ' "${flags[@]}"
}

include_flags_win="$(make_include_flags_win)"

compile_script="$out/compile-one.cmd"
cat >"$compile_script" <<'EOF'
@echo off
setlocal
call "%VCVARS_WIN%" >nul || exit /b 1
set SRC=%~1
set OBJ=%~2
set EXT=%~x1
if /I "%EXT%"==".cpp" (
  cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /wd4244 /wd4267 /wd4819 /wd4996 /DWIN32=1 /D_WIN32=1 /DHAVE_CONFIG_H=1 /DNO_KPSE_DLL=1 /DU_STATIC_IMPLEMENTATION=1 /DHB_STATIC=1 /DGRAPHITE2_STATIC=1 /DFC_STATIC=1 /DXML_STATIC=1 /Dstrdup=_strdup /UZ_HAVE_UNISTD_H %INCLUDE_FLAGS% /Fo"%OBJ%" /c "%SRC%"
) else (
  cl /nologo /O2 /MT /utf-8 /wd4244 /wd4267 /wd4819 /wd4996 /DWIN32=1 /D_WIN32=1 /DHAVE_CONFIG_H=1 /DNO_KPSE_DLL=1 /DU_STATIC_IMPLEMENTATION=1 /DHB_STATIC=1 /DGRAPHITE2_STATIC=1 /DFC_STATIC=1 /DXML_STATIC=1 /Dstrdup=_strdup /UZ_HAVE_UNISTD_H %INCLUDE_FLAGS% /Fo"%OBJ%" /c "%SRC%"
)
exit /b %ERRORLEVEL%
EOF
compile_script_win="$(cygpath -w "$compile_script")"

compile_one() {
  local rel="$1"
  local obj="$out/obj/${rel%.*}.obj"
  mkdir -p "$(dirname "$obj")"
  VCVARS_WIN="$vcvars_win" INCLUDE_FLAGS="$include_flags_win" cmd.exe //D //S //C "$compile_script_win" "$(cygpath -w "$src/$rel")" "$(cygpath -w "$obj")"
}

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

export -f compile_one
export src out compile_script_win vcvars_win include_flags_win
printf '%s\n' "${libxetex_sources[@]}" | xargs -n1 -P "$jobs" bash -c 'compile_one "$0"'

libxetex_objs="$(msys_to_win_list $(find "$out/obj/xetexdir" -name '*.obj' | sort))"
run_vc "$out" "lib /nologo /OUT:libxetex.lib $libxetex_objs"

printf '%s\n' "${top_sources[@]}" | xargs -n1 -P "$jobs" bash -c 'compile_one "$0"'
top_objs="$(msys_to_win_list "$out/obj/xetexdir/xetexextra.obj" "$out/obj/xetexini.obj" "$out/obj/xetex0.obj" "$out/obj/xetex-pool.obj")"

libs=(
  "$out/libxetex.lib"
  "$prebuilt/lib/harfbuzz-icu.lib"
  "$prebuilt/lib/harfbuzz.lib"
  "$prebuilt/lib/graphite2.lib"
  "$prebuilt/lib/freetype.lib"
  "$prebuilt/lib/fontconfig.lib"
  "$prebuilt/lib/libpng16_static.lib"
  "$prebuilt/lib/TECkit.lib"
  "$prebuilt/lib/pplib.lib"
  "$prebuilt/texk/web2c/libmd5.lib"
  "$prebuilt/texk/web2c/lib/lib.lib"
  "$prebuilt/lib/kpathsea.lib"
  "$prebuilt/lib/expat.lib"
  "$prebuilt/lib/zs.lib"
  "$prebuilt/lib/icuin.lib"
  "$prebuilt/lib/icuuc.lib"
  "$prebuilt/lib/icudt.lib"
)
libs_win="$(msys_to_win_list "${libs[@]}")"

system_libs="ws2_32.lib user32.lib advapi32.lib shell32.lib rpcrt4.lib gdi32.lib ole32.lib uuid.lib"

run_vc "$out" "link /nologo /DLL /OUT:xetexdaemon.dll /IMPLIB:xetexdaemon.lib $top_objs $libs_win $system_libs"
run_vc "$out" "cl /nologo /O2 /MT /utf-8 /DDLLPROC=dllxetexmain \"$(cygpath -w "$root/src/windows_mingw_wrapper/calldll.c")\" xetexdaemon.lib /link /OUT:xetexdaemon.exe"

echo "built: $out/xetexdaemon.dll"
echo "built: $out/xetexdaemon.exe"

dvipdfmx_src="$root/src/dvipdfm-x"
libpaper_src="$root/src/libpaper/lib"

need_file "$dvipdfmx_src/dvipdfmx.c"
need_file "$libpaper_src/paper.c"

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

dvipdfmx_compile_script="$out/compile-dvipdfmx-one.cmd"
cat >"$dvipdfmx_compile_script" <<'EOF'
@echo off
setlocal
call "%VCVARS_WIN%" >nul || exit /b 1
set SRC=%~1
set OBJ=%~2
cl /nologo /O2 /MT /utf-8 /wd4244 /wd4267 /wd4819 /wd4996 /DWIN32=1 /D_WIN32=1 /D__STDC__=1 /DHAVE_CONFIG_H=1 /DDVIPDFMX_STANDALONE_DLL=1 /DPAPERSIZE=\"a4\" /DPAPERSIZEVAR=\"PAPERSIZE\" /DNO_KPSE_DLL=1 /DXML_STATIC=1 /Dstrcasecmp=_stricmp /UZ_HAVE_UNISTD_H %DVIPDFMX_INCLUDE_FLAGS% /Fo"%OBJ%" /c "%SRC%"
exit /b %ERRORLEVEL%
EOF
dvipdfmx_compile_script_win="$(cygpath -w "$dvipdfmx_compile_script")"

dvipdfmx_include_dirs=(
  "$dvipdfmx_src"
  "$libpaper_src"
  "$prebuilt/texk"
  "$prebuilt/include"
  "$prebuilt/include/libpng16"
)
dvipdfmx_include_flags=()
for dir in "${dvipdfmx_include_dirs[@]}"; do
  dvipdfmx_include_flags+=("/I\"$(cygpath -w "$dir")\"")
done
dvipdfmx_include_flags_win="$(printf '%s ' "${dvipdfmx_include_flags[@]}")"

compile_dvipdfmx_one() {
  local rel="$1"
  local srcfile obj
  case "$rel" in
    libpaper/*)
      srcfile="$libpaper_src/${rel#libpaper/}"
      obj="$out/obj/${rel%.c}.obj"
      ;;
    *)
      srcfile="$dvipdfmx_src/$rel"
      obj="$out/obj/dvipdfm-x/${rel%.c}.obj"
      ;;
  esac
  mkdir -p "$(dirname "$obj")"
  VCVARS_WIN="$vcvars_win" DVIPDFMX_INCLUDE_FLAGS="$dvipdfmx_include_flags_win" cmd.exe //D //S //C "$dvipdfmx_compile_script_win" "$(cygpath -w "$srcfile")" "$(cygpath -w "$obj")"
}

export -f compile_dvipdfmx_one
export out dvipdfmx_src libpaper_src dvipdfmx_compile_script_win vcvars_win dvipdfmx_include_flags_win
for rel in "${dvipdfmx_sources[@]}"; do
  need_file "$dvipdfmx_src/$rel"
done
printf '%s\n' "${dvipdfmx_sources[@]}" libpaper/paper.c libpaper/dimen.c |
  xargs -n1 -P "$jobs" bash -c 'compile_dvipdfmx_one "$0"'

dvipdfmx_objs="$(msys_to_win_list $(find "$out/obj/dvipdfm-x" -name '*.obj' | sort) "$out/obj/libpaper/paper.obj" "$out/obj/libpaper/dimen.obj")"
dvipdfmx_libs="$(msys_to_win_list "$prebuilt/lib/libpng16_static.lib" "$prebuilt/lib/pplib.lib" "$prebuilt/lib/kpathsea.lib" "$prebuilt/lib/zs.lib")"

run_vc "$out" "link /nologo /DLL /OUT:dvipdfmxdaemon.dll /IMPLIB:dvipdfmxdaemon.lib $dvipdfmx_objs $dvipdfmx_libs ws2_32.lib user32.lib advapi32.lib shell32.lib"
run_vc "$out" "cl /nologo /O2 /MT /utf-8 /DDLLPROC=dlldvipdfmxmain \"$(cygpath -w "$root/src/windows_mingw_wrapper/calldll.c")\" dvipdfmxdaemon.lib /link /OUT:xdvipdfmxdaemon.exe"

echo "built: $out/dvipdfmxdaemon.dll"
echo "built: $out/xdvipdfmxdaemon.exe"
