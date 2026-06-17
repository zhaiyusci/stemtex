#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_root="${SRC_ROOT:-$root/third_party-msvc-src}"
build_root="${BUILD_ROOT:-$root/out/thirdparty-msvc-build}"
prefix="${PREFIX:-$root/prebuilt-msvc}"
downloads="${DOWNLOADS:-$root/out/downloads}"
jobs="${JOBS:-16}"

cmake_exe="${CMAKE_EXE:-/c/Qt/Tools/CMake_64/bin/cmake.exe}"
ninja_exe="${NINJA_EXE:-/c/Qt/Tools/Ninja/ninja.exe}"
vswhere_win="${VSWHERE:-C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe}"

zlib_version=1.3.2
libpng_version=1.6.58
expat_version=2.8.1
freetype_version=2.14.3
fontconfig_version=2.18.1
graphite2_version=1.3.15
harfbuzz_version=14.2.1
icu_version=78.3

mkdir -p "$src_root" "$build_root" "$prefix/include" "$prefix/lib" "$prefix/bin" "$downloads"

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

need_cmd curl
need_cmd tar
need_cmd cygpath
need_file "$cmake_exe"
need_file "$ninja_exe"

find_vcvars() {
  local install
  local vswhere_msys
  vswhere_msys="$(cygpath -u "$vswhere_win")"
  [[ -f "$vswhere_msys" ]] || die "vswhere.exe not found: $vswhere_win"
  install="$("$vswhere_msys" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r' | tail -n1)"
  [[ -n "$install" ]] || die "Visual Studio with VC tools not found"
  local vcvars="$install\\VC\\Auxiliary\\Build\\vcvars64.bat"
  local vcvars_msys
  vcvars_msys="$(cygpath -u "$vcvars")"
  [[ -f "$vcvars_msys" ]] || die "vcvars64.bat not found: $vcvars"
  printf '%s\n' "$vcvars"
}

vcvars_win="$(find_vcvars)"

find_windows_python() {
  if [[ -n "${PYTHON_WIN:-}" ]]; then
    local configured
    configured="$(cygpath -u "$PYTHON_WIN")"
    [[ -f "$configured" ]] || die "PYTHON_WIN does not exist: $PYTHON_WIN"
    printf '%s\n' "$configured"
    return
  fi

  local p
  for p in \
    "/c/Users/${USERNAME:-${USER:-}}/AppData/Local/Python"/pythoncore-*-64/python.exe \
    /c/Python*/python.exe; do
    if [[ -f "$p" ]]; then
      printf '%s\n' "$p"
      return
    fi
  done

  die "Windows Python not found; set PYTHON_WIN to python.exe from a Windows Python installation with meson installed"
}

run_vc() {
  local workdir="$1"
  shift
  local workdir_win
  workdir_win="$(cygpath -w "$workdir")"
  local cmd="$*"
  echo "==> $cmd"
  local bat="$build_root/run-vc-$RANDOM-$RANDOM.cmd"
  local bat_win
  mkdir -p "$build_root"
  cat >"$bat" <<EOF
@echo off
call "$vcvars_win" >nul || exit /b 1
set CL=
set _CL_=
cd /d "$workdir_win" || exit /b 1
$cmd
EOF
  bat_win="$(cygpath -w "$bat")"
  cmd.exe //D //S //C "$bat_win"
}

download() {
  local url="$1"
  local file="$downloads/${url##*/}"
  if [[ ! -f "$file" ]]; then
    echo "DOWNLOAD $url" >&2
    curl -L --fail --retry 3 -o "$file.tmp" "$url"
    mv "$file.tmp" "$file"
  fi
  printf '%s\n' "$file"
}

extract_tar() {
  local archive="$1"
  local dir="$2"
  if [[ ! -d "$dir" ]]; then
    mkdir -p "$src_root"
    echo "EXTRACT $archive" >&2
    tar -xf "$archive" -C "$src_root"
  fi
}

copy_tree_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ -d "$src" ]]; then
    mkdir -p "$dst"
    cp -a "$src"/. "$dst"/
  fi
}

write_static_pkgconfigs() {
  local pcdir="$prefix/lib/pkgconfig"
  local prefix_pc
  prefix_pc="$(cygpath -m "$prefix")"
  mkdir -p "$pcdir"
  cat >"$pcdir/zlib.pc" <<EOF
prefix=$prefix_pc
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: zlib compression library
Version: $zlib_version
Libs: -L\${libdir} -lzs
Cflags: -I\${includedir}
EOF
  cat >"$pcdir/libpng.pc" <<EOF
prefix=$prefix_pc
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libpng
Description: Loads and saves PNG files
Version: $libpng_version
Requires: zlib
Libs: -L\${libdir} -llibpng16_static
Cflags: -I\${includedir}
EOF
  cp -f "$pcdir/libpng.pc" "$pcdir/libpng16.pc"
  cat >"$pcdir/expat.pc" <<EOF
prefix=$prefix_pc
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: expat
Description: XML parser library
Version: $expat_version
Libs: -L\${libdir} -lexpat
Cflags: -I\${includedir}
EOF
  cat >"$pcdir/graphite2.pc" <<EOF
prefix=$prefix_pc
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: Graphite2
Description: Font rendering engine for Complex Scripts
Version: $graphite2_version
Libs: -L\${libdir} -lgraphite2
Cflags: -I\${includedir}
EOF
}

build_cmake_static() {
  local name="$1"
  local src="$2"
  shift 2
  local build="$build_root/$name"
  rm -rf "$build"
  mkdir -p "$build"
  local prefix_win cmake_win ninja_win src_win build_win
  prefix_win="$(cygpath -w "$prefix")"
  cmake_win="$(cygpath -w "$cmake_exe")"
  ninja_win="$(cygpath -w "$ninja_exe")"
  src_win="$(cygpath -w "$src")"
  build_win="$(cygpath -w "$build")"
  run_vc "$root" "\"$cmake_win\" -S \"$src_win\" -B \"$build_win\" -G Ninja -DCMAKE_MAKE_PROGRAM=\"$ninja_win\" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\"$prefix_win\" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded $*"
  run_vc "$root" "\"$cmake_win\" --build \"$build_win\" --config Release --parallel $jobs"
  run_vc "$root" "\"$cmake_win\" --install \"$build_win\" --config Release"
}

configure_cmake_static() {
  local name="$1"
  local src="$2"
  shift 2
  local build="$build_root/$name"
  rm -rf "$build"
  mkdir -p "$build"
  local prefix_win cmake_win ninja_win src_win build_win
  prefix_win="$(cygpath -w "$prefix")"
  cmake_win="$(cygpath -w "$cmake_exe")"
  ninja_win="$(cygpath -w "$ninja_exe")"
  src_win="$(cygpath -w "$src")"
  build_win="$(cygpath -w "$build")"
  run_vc "$root" "\"$cmake_win\" -S \"$src_win\" -B \"$build_win\" -G Ninja -DCMAKE_MAKE_PROGRAM=\"$ninja_win\" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\"$prefix_win\" -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded $*"
}

build_cmake_target() {
  local name="$1"
  local target="$2"
  local build="$build_root/$name"
  local cmake_win build_win
  cmake_win="$(cygpath -w "$cmake_exe")"
  build_win="$(cygpath -w "$build")"
  run_vc "$root" "\"$cmake_win\" --build \"$build_win\" --config Release --target \"$target\" --parallel $jobs"
}

build_zlib() {
  local archive dir
  archive="$(download "https://zlib.net/zlib-$zlib_version.tar.gz")"
  dir="$src_root/zlib-$zlib_version"
  extract_tar "$archive" "$dir"
  build_cmake_static "zlib-$zlib_version" "$dir" -DZLIB_BUILD_EXAMPLES=OFF
  if [[ -f "$prefix/include/zconf.h" ]]; then
    awk '
      /^#  define Z_HAVE_UNISTD_H$/ { print "/* # undef Z_HAVE_UNISTD_H for MSVC standalone */"; next }
      /^#    define Z_HAVE_UNISTD_H$/ { print "/* # undef Z_HAVE_UNISTD_H for MSVC standalone */"; next }
      { print }
    ' "$prefix/include/zconf.h" >"$prefix/include/zconf.h.tmp"
    mv "$prefix/include/zconf.h.tmp" "$prefix/include/zconf.h"
  fi
  rm -f "$prefix/bin/z.dll" "$prefix/lib/z.lib"
  write_static_pkgconfigs
}

build_libpng() {
  local archive dir
  archive="$(download "https://download.sourceforge.net/libpng/libpng-$libpng_version.tar.gz")"
  dir="$src_root/libpng-$libpng_version"
  extract_tar "$archive" "$dir"
  build_cmake_static "libpng-$libpng_version" "$dir" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF "-DZLIB_INCLUDE_DIR=$(cygpath -w "$prefix/include")" "-DZLIB_LIBRARY=$(cygpath -w "$prefix/lib/zs.lib")"
}

build_expat() {
  local archive dir tag
  tag="R_${expat_version//./_}"
  archive="$(download "https://github.com/libexpat/libexpat/releases/download/$tag/expat-$expat_version.tar.xz")"
  dir="$src_root/expat-$expat_version"
  extract_tar "$archive" "$dir"
  build_cmake_static "expat-$expat_version" "$dir" -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_SHARED_LIBS=OFF -DEXPAT_MSVC_STATIC_CRT=ON
  cp -f "$prefix/lib/libexpatMD.lib" "$prefix/lib/expat.lib"
}

build_graphite2() {
  local archive dir
  archive="$(download "https://github.com/silnrsi/graphite/releases/download/$graphite2_version/graphite2-$graphite2_version.tgz")"
  dir="$src_root/graphite2-$graphite2_version"
  extract_tar "$archive" "$dir"
  local build="$build_root/graphite2-$graphite2_version"
  rm -rf "$build"
  mkdir -p "$build"
  local dir_win build_win
  dir_win="$(cygpath -w "$dir")"
  build_win="$(cygpath -w "$build")"
  run_vc "$root" "cl /nologo /utf-8 /O2 /MT /EHsc /DGRAPHITE2_STATIC /DGRAPHITE2_NTRACING /D_CRT_SECURE_NO_WARNINGS /I\"$dir_win\\include\" /I\"$dir_win\\src\" /I\"$dir_win\\src\\inc\" /Fo\"$build_win\\\\\" /c \"$dir_win\\src\\call_machine.cpp\" \"$dir_win\\src\\gr_char_info.cpp\" \"$dir_win\\src\\gr_face.cpp\" \"$dir_win\\src\\gr_features.cpp\" \"$dir_win\\src\\gr_font.cpp\" \"$dir_win\\src\\gr_logging.cpp\" \"$dir_win\\src\\gr_segment.cpp\" \"$dir_win\\src\\gr_slot.cpp\" \"$dir_win\\src\\json.cpp\" \"$dir_win\\src\\CmapCache.cpp\" \"$dir_win\\src\\Code.cpp\" \"$dir_win\\src\\Collider.cpp\" \"$dir_win\\src\\Decompressor.cpp\" \"$dir_win\\src\\Face.cpp\" \"$dir_win\\src\\FeatureMap.cpp\" \"$dir_win\\src\\FileFace.cpp\" \"$dir_win\\src\\Font.cpp\" \"$dir_win\\src\\GlyphCache.cpp\" \"$dir_win\\src\\GlyphFace.cpp\" \"$dir_win\\src\\Intervals.cpp\" \"$dir_win\\src\\Justifier.cpp\" \"$dir_win\\src\\NameTable.cpp\" \"$dir_win\\src\\Pass.cpp\" \"$dir_win\\src\\Position.cpp\" \"$dir_win\\src\\Segment.cpp\" \"$dir_win\\src\\Silf.cpp\" \"$dir_win\\src\\Slot.cpp\" \"$dir_win\\src\\Sparse.cpp\" \"$dir_win\\src\\TtfUtil.cpp\" \"$dir_win\\src\\UtfCodec.cpp\""
  run_vc "$build" "lib /nologo /OUT:graphite2.lib *.obj"
  mkdir -p "$prefix/lib" "$prefix/include"
  cp -f "$build/graphite2.lib" "$prefix/lib/graphite2.lib"
  copy_tree_if_exists "$dir/include/graphite2" "$prefix/include/graphite2"
  write_static_pkgconfigs
}

build_freetype() {
  local archive dir
  archive="$(download "https://download.savannah.gnu.org/releases/freetype/freetype-$freetype_version.tar.xz")"
  dir="$src_root/freetype-$freetype_version"
  extract_tar "$archive" "$dir"
  build_cmake_static "freetype-$freetype_version" "$dir" \
    -DFT_DISABLE_ZLIB=OFF \
    -DFT_DISABLE_PNG=OFF \
    -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    "-DZLIB_INCLUDE_DIR=$(cygpath -w "$prefix/include")" \
    "-DZLIB_LIBRARY=$(cygpath -w "$prefix/lib/zs.lib")" \
    "-DPNG_PNG_INCLUDE_DIR=$(cygpath -w "$prefix/include")" \
    "-DPNG_LIBRARY=$(cygpath -w "$prefix/lib/libpng16_static.lib")"
}

build_icu() {
  local archive dir bash_win prefix_msys
  archive="$(download "https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-$icu_version-sources.tgz")"
  dir="$src_root/icu"
  extract_tar "$archive" "$dir"
  bash_win="$(cygpath -w /usr/bin/bash.exe)"
  prefix_msys="$(cygpath -m "$prefix")"
  run_vc "$root" "\"$bash_win\" --noprofile --norc -c \"export MSYS2_ARG_CONV_EXCL='*'; cd '$(cygpath -m "$dir/source")' && make distclean >/dev/null 2>&1 || true\""
  run_vc "$root" "\"$bash_win\" --noprofile --norc -c \"export MSYS2_ARG_CONV_EXCL='*'; cd '$(cygpath -m "$dir/source")' && CFLAGS='-Gy -MT' CXXFLAGS='-Gy -MT /std:c++17' ./runConfigureICU MSYS/MSVC --prefix='$prefix_msys' --disable-shared --enable-static --disable-samples --disable-tests --disable-extras --disable-icuio\""
  run_vc "$root" "\"$bash_win\" --noprofile --norc -c \"export MSYS2_ARG_CONV_EXCL='*'; cd '$(cygpath -m "$dir/source")' && make -j $jobs\""
  run_vc "$root" "\"$bash_win\" --noprofile --norc -c \"export MSYS2_ARG_CONV_EXCL='*'; cd '$(cygpath -m "$dir/source")' && make install\""
  cp -f "$prefix/lib/sicuuc.lib" "$prefix/lib/icuuc.lib"
  cp -f "$prefix/lib/sicuin.lib" "$prefix/lib/icuin.lib"
  cp -f "$dir/source/stubdata/sicudt.lib" "$prefix/lib/icudt.lib"
  mkdir -p "$prefix/share/icu-data"
  cp -f "$dir/source/data/in/icudt78l.dat" "$prefix/share/icu-data/icudt78l.dat"
}

build_harfbuzz() {
  local archive dir
  archive="$(download "https://github.com/harfbuzz/harfbuzz/releases/download/$harfbuzz_version/harfbuzz-$harfbuzz_version.tar.xz")"
  dir="$src_root/harfbuzz-$harfbuzz_version"
  extract_tar "$archive" "$dir"
  build_cmake_static "harfbuzz-$harfbuzz_version" "$dir" \
    -DHB_HAVE_FREETYPE=ON \
    -DHB_HAVE_GRAPHITE2=ON \
    -DHB_HAVE_ICU=ON \
    -DHB_BUILD_UTILS=OFF \
    -DHB_BUILD_SUBSET=ON \
    -DHB_BUILD_RASTER=OFF \
    -DHB_BUILD_VECTOR=OFF \
    -DHB_BUILD_GPU=OFF \
    "-DCMAKE_PREFIX_PATH=$(cygpath -w "$prefix")" \
    "-DFREETYPE_INCLUDE_DIRS=$(cygpath -w "$prefix/include/freetype2")" \
    "-DFREETYPE_LIBRARIES=$(cygpath -w "$prefix/lib/freetype.lib")" \
    "-DGRAPHITE2_INCLUDE_DIR=$(cygpath -w "$prefix/include")" \
    "-DGRAPHITE2_LIBRARY=$(cygpath -w "$prefix/lib/graphite2.lib")" \
    "-DCMAKE_CXX_FLAGS=/DU_STATIC_IMPLEMENTATION"
}

build_fontconfig() {
  local archive dir build prefix_win pkg_config_win python_win path_extra python_scripts_win
  archive="$(download "https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/$fontconfig_version/fontconfig-$fontconfig_version.tar.xz")"
  dir="$src_root/fontconfig-$fontconfig_version"
  extract_tar "$archive" "$dir"
  cp -f "$prefix/lib/libexpatMD.lib" "$prefix/lib/expat.lib"
  build="$build_root/fontconfig-$fontconfig_version"
  rm -rf "$build"
  prefix_win="$(cygpath -w "$prefix")"
  pkg_config_win="$(cygpath -w /usr/bin/pkg-config.exe)"
  local python_msys
  python_msys="$(find_windows_python)"
  python_win="$(cygpath -w "$python_msys")"
  python_scripts_win="$(cygpath -w "$(dirname "$python_msys")/Scripts")"
  path_extra="$(cygpath -w /usr/bin)"
  need_file "$(cygpath -u "$python_win")"
  write_static_pkgconfigs
  run_vc "$root" "set PATH=$python_scripts_win;%PATH%;$path_extra&& set PKG_CONFIG=$pkg_config_win&& set PKG_CONFIG_PATH=$prefix_win\\lib\\pkgconfig&& set CC=cl&& set CFLAGS=/utf-8 /MT /DXML_STATIC&& set LDFLAGS=&& \"$python_win\" -m mesonbuild.mesonmain setup \"$(cygpath -w "$build")\" \"$(cygpath -w "$dir")\" --prefix=\"$prefix_win\" --buildtype=release --default-library=static -Dtests=disabled -Ddoc=disabled -Dnls=disabled -Diconv=disabled -Dfontations=disabled -Dxml-backend=expat -Dcache-build=disabled -Dtools=enabled"
  run_vc "$root" "set PATH=$python_scripts_win;%PATH%;$path_extra&& \"$python_win\" -m mesonbuild.mesonmain compile -C \"$(cygpath -w "$build")\" -j $jobs"
  run_vc "$root" "set PATH=$python_scripts_win;%PATH%;$path_extra&& \"$python_win\" -m mesonbuild.mesonmain install -C \"$(cygpath -w "$build")\""
  cp -f "$prefix/lib/libfontconfig.a" "$prefix/lib/fontconfig.lib"
}

write_manifest() {
  cat >"$prefix/VERSIONS.txt" <<EOF
MSVC static third-party libraries for xetex-live-worker

zlib       $zlib_version
libpng     $libpng_version
expat      $expat_version
freetype   $freetype_version
fontconfig $fontconfig_version
graphite2  $graphite2_version
harfbuzz   $harfbuzz_version
icu        $icu_version

Built with:
$vcvars_win
EOF
}

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
  targets=(zlib libpng expat graphite2 freetype icu harfbuzz fontconfig)
fi

for target in "${targets[@]}"; do
  case "$target" in
    zlib) build_zlib ;;
    libpng) build_libpng ;;
    expat) build_expat ;;
    graphite2) build_graphite2 ;;
    freetype) build_freetype ;;
    icu) build_icu ;;
    harfbuzz) build_harfbuzz ;;
    fontconfig) build_fontconfig ;;
    all) build_zlib; build_libpng; build_expat; build_graphite2; build_freetype; build_icu; build_harfbuzz; build_fontconfig ;;
    *) die "unknown target: $target" ;;
  esac
done

write_manifest
echo "built MSVC prebuilt prefix: $prefix"
