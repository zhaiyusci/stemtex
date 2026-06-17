#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$root/.." && pwd)"
tl_src="${TL_SRC:-$root/src}"
kpse_generated="${KPSE_GENERATED:-$root/src/texk/kpathsea/generated-msvc}"
prefix="${PREFIX:-$root/prebuilt-msvc}"
build_root="${BUILD_ROOT:-$root/out/texlive-libs-msvc-build}"
jobs="${JOBS:-16}"
vswhere_win="${VSWHERE:-C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe}"

mkdir -p "$prefix/lib" "$prefix/bin" "$prefix/texk/kpathsea" \
  "$prefix/texk/web2c/lib" "$prefix/libs/pplib/include" \
  "$prefix/libs/teckit/include/teckit" "$build_root"

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

need_dir() {
  [[ -d "$1" ]] || die "missing directory: $1"
}

need_cmd cygpath
need_cmd xargs
need_cmd find
need_cmd awk
need_dir "$tl_src"
need_dir "$kpse_generated"
need_file "$prefix/lib/zs.lib"

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
  bat="$build_root/run-vc-$RANDOM-$RANDOM.cmd"
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

copy_kpathsea_headers() {
  local src="$tl_src/texk/kpathsea"
  local gen="$kpse_generated"
  need_file "$gen/c-auto.h"
  need_file "$gen/paths.h"
  need_file "$gen/kpathsea.h"
  cp -p "$gen/c-auto.h" "$gen/paths.h" "$gen/kpathsea.h" "$prefix/texk/kpathsea/"
  awk '
    /^#define HAVE_UNISTD_H / { print "/* #undef HAVE_UNISTD_H */"; next }
    { print }
  ' "$prefix/texk/kpathsea/c-auto.h" >"$prefix/texk/kpathsea/c-auto.h.tmp"
  mv "$prefix/texk/kpathsea/c-auto.h.tmp" "$prefix/texk/kpathsea/c-auto.h"
  cat >>"$prefix/texk/kpathsea/c-auto.h" <<'EOF'

/* MSVC standalone overrides. */
#ifndef HAVE_ASSERT_H
#define HAVE_ASSERT_H 1
#endif
#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H 1
#endif
#ifndef HAVE_STDLIB_H
#define HAVE_STDLIB_H 1
#endif
#ifndef HAVE_STRING_H
#define HAVE_STRING_H 1
#endif
#ifndef HAVE_SYS_STAT_H
#define HAVE_SYS_STAT_H 1
#endif
#ifndef HAVE_SYS_TYPES_H
#define HAVE_SYS_TYPES_H 1
#endif
EOF
  find "$src" -maxdepth 1 -type f -name '*.h' -exec cp -p {} "$prefix/texk/kpathsea/" \;
}

build_kpathsea() {
  echo "==> build kpathsea.lib"
  copy_kpathsea_headers
  local src="$tl_src/texk/kpathsea"
  local objdir="$build_root/kpathsea"
  rm -rf "$objdir"
  mkdir -p "$objdir"

  local sources=(
    tex-file.c absolute.c atou.c cnf.c concat.c concat3.c concatn.c db.c
    debug.c dir.c dirent.c elt-dirs.c expand.c extend-fname.c file-p.c find-suffix.c
    fn.c fontmap.c hash.c kdefault.c kpathsea.c line.c magstep.c
    make-suffix.c path-elt.c pathsearch.c proginit.c progname.c readable.c
    rm-suffix.c str-list.c str-llist.c tex-glyph.c tex-hush.c tex-make.c
    tilde.c uppercasify.c variable.c version.c xbasename.c xcalloc.c
    xdirname.c xfopen.c xfseek.c xftell.c xgetcwd.c xmalloc.c xopendir.c
    xstrdup.c
    xputenv.c xrealloc.c xstat.c getopt.c getopt1.c win32lib.c knj.c
  )
  local src_paths=()
  local s
  for s in "${sources[@]}"; do
    need_file "$src/$s"
    src_paths+=("$src/$s")
  done

  local compile_script="$objdir/compile-one.cmd"
  cat >"$compile_script" <<'EOF'
@echo off
setlocal
call "%VCVARS_WIN%" >nul || exit /b 1
set SRC=%~1
set OBJ=%~2
cl /nologo /O2 /MT /utf-8 /wd4244 /wd4267 /wd4996 /DWIN32=1 /D_WIN32=1 /DHAVE_CONFIG_H=1 /DNO_KPSE_DLL=1 /DMAKE_KPSE_DLL=1 /I"%KPSE_INC_PARENT%" /I"%KPSE_SRC%" /I"%KPSE_SRC_PARENT%" /Fo"%OBJ%" /c "%SRC%"
exit /b %ERRORLEVEL%
EOF
  local compile_script_win
  compile_script_win="$(cygpath -w "$compile_script")"
  local inc_parent_win src_parent_win
  inc_parent_win="$(cygpath -w "$prefix/texk")"
  src_parent_win="$(cygpath -w "$tl_src/texk")"
  local kpse_src_win
  kpse_src_win="$(cygpath -w "$src")"

  printf '%s\n' "${src_paths[@]}" | xargs -n1 -P "$jobs" bash -c '
    src="$0"
    base="$(basename "$src" .c)"
    obj="'"$objdir"'/$base.obj"
    VCVARS_WIN="'"$vcvars_win"'" KPSE_INC_PARENT="'"$inc_parent_win"'" KPSE_SRC="'"$kpse_src_win"'" KPSE_SRC_PARENT="'"$src_parent_win"'" cmd.exe //D //S //C "'"$compile_script_win"'" "$(cygpath -w "$src")" "$(cygpath -w "$obj")"
  '

  local objs
  objs="$(msys_to_win_list "$objdir"/*.obj)"
  run_vc "$objdir" "lib /nologo /OUT:kpathsea.lib $objs"
  cp -p "$objdir/kpathsea.lib" "$prefix/lib/kpathsea.lib"
  mkdir -p "$prefix/texk/kpathsea/.libs"
  cp -p "$objdir/kpathsea.lib" "$prefix/texk/kpathsea/.libs/kpathsea.lib"
}

build_web2c_libs() {
  echo "==> build web2c support libs"
  local src="$root/src/web2c"
  local objdir="$build_root/web2c"
  rm -rf "$objdir"
  mkdir -p "$objdir"
  need_file "$src/libmd5/md5.c"

  run_vc "$objdir" "cl /nologo /O2 /MT /utf-8 /wd4996 /I\"$(cygpath -w "$src/libmd5")\" /Fo:md5.obj /c \"$(cygpath -w "$src/libmd5/md5.c")\" && lib /nologo /OUT:libmd5.lib md5.obj"
  cp -p "$objdir/libmd5.lib" "$prefix/texk/web2c/libmd5.lib"

  local sources=(
    alloca.c basechsuffix.c chartostring.c coredump.c eofeoln.c fprintreal.c
    input2int.c inputint.c openclose.c printversion.c setupvar.c
    uexit.c usage.c version.c zround.c
  )
  local src_paths=()
  local s
  for s in "${sources[@]}"; do
    need_file "$src/lib/$s"
    src_paths+=("$src/lib/$s")
  done
  local compile_script="$objdir/compile-one.cmd"
  cat >"$compile_script" <<'EOF'
@echo off
setlocal
call "%VCVARS_WIN%" >nul || exit /b 1
set SRC=%~1
set OBJ=%~2
cl /nologo /O2 /MT /utf-8 /wd4244 /wd4267 /wd4996 /DWIN32=1 /D_CRT_SECURE_NO_DEPRECATE=1 /DNO_KPSE_DLL=1 /I"%WEB2C_INC%" /I"%KPSE_INC_PARENT%" /Fo"%OBJ%" /c "%SRC%"
exit /b %ERRORLEVEL%
EOF
  local compile_script_win web2c_inc_win kpse_inc_parent_win
  compile_script_win="$(cygpath -w "$compile_script")"
  web2c_inc_win="$(cygpath -w "$src")"
  kpse_inc_parent_win="$(cygpath -w "$prefix/texk")"
  printf '%s\n' "${src_paths[@]}" | xargs -n1 -P "$jobs" bash -c '
    src="$0"
    base="$(basename "$src" .c)"
    obj="'"$objdir"'/$base.obj"
    VCVARS_WIN="'"$vcvars_win"'" WEB2C_INC="'"$web2c_inc_win"'" KPSE_INC_PARENT="'"$kpse_inc_parent_win"'" cmd.exe //D //S //C "'"$compile_script_win"'" "$(cygpath -w "$src")" "$(cygpath -w "$obj")"
  '
  local objs
  objs="$(msys_to_win_list "$objdir"/alloca.obj "$objdir"/basechsuffix.obj "$objdir"/chartostring.obj "$objdir"/coredump.obj "$objdir"/eofeoln.obj "$objdir"/fprintreal.obj "$objdir"/input2int.obj "$objdir"/inputint.obj "$objdir"/openclose.obj "$objdir"/printversion.obj "$objdir"/setupvar.obj "$objdir"/uexit.obj "$objdir"/usage.obj "$objdir"/version.obj "$objdir"/zround.obj)"
  run_vc "$objdir" "lib /nologo /OUT:web2c-lib.lib $objs"
  cp -p "$objdir/web2c-lib.lib" "$prefix/texk/web2c/lib/lib.lib"
}

build_pplib() {
  echo "==> build pplib.lib"
  local src="$tl_src/libs/pplib/pplib-src/src"
  local objdir="$build_root/pplib"
  rm -rf "$objdir"
  mkdir -p "$objdir"
  local sources=(
    pparray.c ppcrypt.c ppdict.c ppheap.c ppload.c ppstream.c ppxref.c
    util/utilbasexx.c util/utilcrypt.c util/utilflate.c util/utilfpred.c
    util/utiliof.c util/utillog.c util/utillzw.c util/utilmd5.c util/utilmem.c
    util/utilmemheap.c util/utilmemheapiof.c util/utilmeminfo.c
    util/utilnumber.c util/utilsha.c
  )
  local src_paths=()
  local s
  for s in "${sources[@]}"; do
    need_file "$src/$s"
    src_paths+=("$src/$s")
  done
  local compile_script="$objdir/compile-one.cmd"
  cat >"$compile_script" <<'EOF'
@echo off
setlocal
call "%VCVARS_WIN%" >nul || exit /b 1
set SRC=%~1
set OBJ=%~2
cl /nologo /O2 /MT /utf-8 /wd4244 /wd4267 /wd4996 /D_CRT_SECURE_NO_DEPRECATE=1 /I"%PPLIB_SRC%" /I"%PPLIB_UTIL%" /I"%ZLIB_INC%" /Fo"%OBJ%" /c "%SRC%"
exit /b %ERRORLEVEL%
EOF
  local compile_script_win pplib_src_win pplib_util_win zlib_inc_win
  compile_script_win="$(cygpath -w "$compile_script")"
  pplib_src_win="$(cygpath -w "$src")"
  pplib_util_win="$(cygpath -w "$src/util")"
  zlib_inc_win="$(cygpath -w "$prefix/include")"
  printf '%s\n' "${src_paths[@]}" | xargs -n1 -P "$jobs" bash -c '
    src="$0"
    rel="${src#'"$src"'/}"
    base="${rel//\//_}"
    obj="'"$objdir"'/${base%.c}.obj"
    VCVARS_WIN="'"$vcvars_win"'" PPLIB_SRC="'"$pplib_src_win"'" PPLIB_UTIL="'"$pplib_util_win"'" ZLIB_INC="'"$zlib_inc_win"'" cmd.exe //D //S //C "'"$compile_script_win"'" "$(cygpath -w "$src")" "$(cygpath -w "$obj")"
  '
  local objs
  objs="$(msys_to_win_list "$objdir"/*.obj)"
  run_vc "$objdir" "lib /nologo /OUT:pplib.lib $objs"
  cp -p "$objdir/pplib.lib" "$prefix/lib/pplib.lib"
  mkdir -p "$prefix/libs/pplib"
  cp -p "$objdir/pplib.lib" "$prefix/libs/pplib/pplib.lib"
  find "$src" -maxdepth 1 -type f -name '*.h' -exec cp -p {} "$prefix/libs/pplib/include/" \;
  find "$src/util" -maxdepth 1 -type f -name '*.h' -exec cp -p {} "$prefix/libs/pplib/include/" \;
}

build_teckit() {
  echo "==> build TECkit libs"
  local src="$tl_src/libs/teckit/TECkit-src/source"
  local objdir="$build_root/teckit"
  rm -rf "$objdir"
  mkdir -p "$objdir"
  need_file "$src/Engine.cpp"
  need_file "$src/Compiler.cpp"
  need_file "$src/UnicodeNames.cpp"
  need_file "$src/NormalizationData.c"
  cp -p "$src"/Public-headers/*.h "$prefix/libs/teckit/include/teckit/"
  run_vc "$objdir" "cl /nologo /O2 /MT /EHsc /utf-8 /wd4267 /wd4996 /I\"$(cygpath -w "$src")\" /I\"$(cygpath -w "$src/Public-headers")\" /I\"$(cygpath -w "$prefix/include")\" /Fo:Engine.obj /c \"$(cygpath -w "$src/Engine.cpp")\" && lib /nologo /OUT:TECkit.lib Engine.obj"
  run_vc "$objdir" "cl /nologo /O2 /MT /EHsc /utf-8 /wd4267 /wd4996 /I\"$(cygpath -w "$src")\" /I\"$(cygpath -w "$src/Public-headers")\" /I\"$(cygpath -w "$prefix/include")\" /Fo:Compiler.obj /c \"$(cygpath -w "$src/Compiler.cpp")\" && cl /nologo /O2 /MT /EHsc /utf-8 /wd4267 /wd4996 /I\"$(cygpath -w "$src")\" /I\"$(cygpath -w "$src/Public-headers")\" /I\"$(cygpath -w "$prefix/include")\" /Fo:UnicodeNames.obj /c \"$(cygpath -w "$src/UnicodeNames.cpp")\" && lib /nologo /OUT:TECkit_Compiler.lib Compiler.obj UnicodeNames.obj"
  mkdir -p "$prefix/libs/teckit"
  cp -p "$objdir/TECkit.lib" "$prefix/lib/TECkit.lib"
  cp -p "$objdir/TECkit_Compiler.lib" "$prefix/lib/TECkit_Compiler.lib"
  cp -p "$objdir/TECkit.lib" "$prefix/libs/teckit/TECkit.lib"
  cp -p "$objdir/TECkit_Compiler.lib" "$prefix/libs/teckit/TECkit_Compiler.lib"
}

build_kpathsea
build_web2c_libs
build_pplib
build_teckit

echo "built TeX Live support libraries under: $prefix"
