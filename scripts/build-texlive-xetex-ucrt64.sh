#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
default_src="$(cd "$repo_root/.." && pwd)/texlive-source"
default_build="$(cd "$repo_root/.." && pwd)/tlbuild-xetex-ucrt64-mingw"
default_generated="$repo_root/texlive-xetex/web2c"
default_prebuilt="$repo_root/texlive-xetex/prebuilt-ucrt64"

tl_src="${TL_SRC:-$default_src}"
build_dir="${BUILD_DIR:-$default_build}"
generated_dir="${GENERATED_XETEX_DIR:-$default_generated}"
prebuilt_dir="${PREBUILT_LIBS_DIR:-$default_prebuilt}"
jobs="${JOBS:-16}"
clean="${CLEAN:-0}"
use_generated="${USE_GENERATED:-1}"
use_prebuilt_libs="${USE_PREBUILT_LIBS:-1}"

export MSYSTEM="${MSYSTEM:-UCRT64}"
export PATH="/ucrt64/bin:/usr/bin:$PATH"

die() {
  echo "error: $*" >&2
  exit 1
}

need_file() {
  [[ -f "$1" ]] || die "missing required file: $1"
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

run() {
  echo "==> $*"
  "$@"
}

run_sh() {
  echo "==> $*"
  "$@"
}

configure_subdir() {
  local subdir="$1"
  mkdir -p "$build_dir/$subdir"
  local cmd
  cmd="$(sed "s,auxdir/auxsub,$subdir,g" "$build_dir/subsubdir-conf.cmd")"
  echo "==> configure $subdir"
  (cd "$build_dir/$subdir" && eval /bin/sh "$cmd")
}

patch_web2c_makefile_for_native_windows() {
  local makefile="$build_dir/texk/web2c/Makefile"
  need_file "$makefile"

  # The generated Makefile assumes Unix path lists such as WEBINPUTS=.:srcdir.
  # Here the tools being run are native Windows executables, so kpathsea needs
  # semicolon-separated lists. Quote the assignment so /bin/sh does not treat
  # the semicolon as a command separator.
  perl -0pi -e '
     s{WEBINPUTS=\.:\$\(srcdir\)/xetexdir}{WEBINPUTS=".;\$(srcdir)/xetexdir"}g;
     s{WEBINPUTS=\.:\$\(srcdir\)/pmpostdir}{WEBINPUTS=".;\$(srcdir)/pmpostdir"}g;
     s{WEBINPUTS=\.:\$\(srcdir\)/pdftexdir}{WEBINPUTS=".;\$(srcdir)/pdftexdir"}g;
     s{WEBINPUTS=\.:\$\(srcdir\)/omegaware}{WEBINPUTS=".;\$(srcdir)/omegaware"}g;
     s{WEBINPUTS=\.:\$\(srcdir\)([^/])}{WEBINPUTS=".;\$(srcdir)"$1}g;
     s{WEBINPUTS=\.:\$\(srcdir\)$}{WEBINPUTS=".;\$(srcdir)"}g;
  ' \
    "$makefile"
}

seed_generated_xetex_sources() {
  [[ "$use_generated" == "1" ]] || return 0
  [[ -d "$generated_dir" ]] || return 0

  local web2c_build="$build_dir/texk/web2c"
  local files=(
    xetexini.c
    xetex0.c
    xetexcoerce.h
    xetexd.h
    xetex-pool.c
  )

  for f in "${files[@]}"; do
    need_file "$generated_dir/$f"
  done

  echo "==> seeding generated XeTeX web2c sources from: $generated_dir"
  cp -p "$generated_dir"/xetexini.c \
        "$generated_dir"/xetex0.c \
        "$generated_dir"/xetexcoerce.h \
        "$generated_dir"/xetexd.h \
        "$generated_dir"/xetex-pool.c \
        "$web2c_build"/

  # Preserve the Makefile's multiple-output stamp logic without invoking tie,
  # otangle, or web2c convert. The order matters:
  #   xetex-tangle < xetex.p/xetex.pool < xetex-web2c < generated C/H files
  touch "$web2c_build/xetex-tangle"
  sleep 1
  : >"$web2c_build/xetex.p"
  : >"$web2c_build/xetex.pool"
  touch "$web2c_build/xetex.p" "$web2c_build/xetex.pool"
  sleep 1
  touch "$web2c_build/xetex-web2c"
  mkdir -p "$web2c_build/web2c"
  touch "$web2c_build/web2c/stamp-makecpool"
  sleep 1
  touch "$web2c_build/xetexini.c" \
        "$web2c_build/xetex0.c" \
        "$web2c_build/xetexcoerce.h" \
        "$web2c_build/xetexd.h" \
        "$web2c_build/xetex-pool.c"
}

seed_prebuilt_static_libraries() {
  [[ "$use_prebuilt_libs" == "1" ]] || return 1
  [[ -d "$prebuilt_dir" ]] || return 1

  local files=(
    texk/kpathsea/.libs/libkpathsea.a
    texk/kpathsea/libkpathsea.la
    texk/kpathsea/c-auto.h
    texk/kpathsea/kpathsea.h
    texk/kpathsea/paths.h
    libs/teckit/libTECkit.a
    libs/teckit/include/teckit/TECkit_Common.h
    libs/teckit/include/teckit/TECkit_Engine.h
    libs/pplib/libpplib.a
    libs/pplib/include/pplib.h
    texk/web2c/lib/lib.a
    texk/web2c/libmd5.a
  )

  for f in "${files[@]}"; do
    need_file "$prebuilt_dir/$f"
  done

  echo "==> seeding prebuilt UCRT64 static libraries from: $prebuilt_dir"
  mkdir -p "$build_dir"
  cp -a "$prebuilt_dir"/. "$build_dir"/
  return 0
}

backup_bootstrap_files() {
  backup_dir="$(mktemp -d)"
  bootstrap_files=(
    "texk/web2c/tangleboot.pin"
    "texk/web2c/ctangleboot.cin"
    "texk/web2c/cwebboot.cin"
  )

  for rel in "${bootstrap_files[@]}"; do
    need_file "$tl_src/$rel"
    mkdir -p "$backup_dir/$(dirname "$rel")"
    cp -p "$tl_src/$rel" "$backup_dir/$rel"
  done
}

restore_bootstrap_files() {
  local changed=0
  for rel in "${bootstrap_files[@]}"; do
    if ! cmp -s "$tl_src/$rel" "$backup_dir/$rel"; then
      echo "==> restoring source bootstrap file changed by build: $rel"
      cp -p "$backup_dir/$rel" "$tl_src/$rel"
      changed=1
    fi
  done
  rm -rf "$backup_dir"
  return "$changed"
}

configure_top() {
  mkdir -p "$build_dir"
  (
    cd "$build_dir"
    run_sh "$tl_src/configure" \
      --build=x86_64-w64-mingw32 \
      --host=x86_64-w64-mingw32 \
      --disable-native-texlive-build \
      --disable-all-pkgs \
      --enable-web2c \
      --enable-xetex \
      --disable-xetex-synctex \
      --without-x \
      --with-system-zlib \
      --with-system-libpng \
      --with-system-freetype2 \
      --with-system-icu \
      --with-system-graphite2 \
      --with-system-harfbuzz \
      --without-system-teckit \
      --with-fontconfig-includes=/ucrt64/include \
      --with-fontconfig-libdir=/ucrt64/lib
  )
}

main() {
  need_cmd sed
  need_cmd perl
  need_cmd make
  need_cmd x86_64-w64-mingw32-gcc
  need_cmd x86_64-w64-mingw32-g++
  need_cmd bison
  need_cmd flex

  [[ -d "$tl_src" ]] || die "TL_SRC does not exist: $tl_src"
  need_file "$tl_src/configure"

  if [[ "$clean" == "1" ]]; then
    echo "==> removing build dir: $build_dir"
    rm -rf "$build_dir"
  fi

  backup_bootstrap_files
  trap 'restore_bootstrap_files >/dev/null 2>&1 || true' EXIT

  if [[ ! -f "$build_dir/Makefile" ]]; then
    configure_top
  fi

  if ! seed_prebuilt_static_libraries; then
    # Build XeTeX's internal base libraries only.
    run make -C "$build_dir" -j"$jobs" recurse

    # XeTeX needs TECkit and pplib from the TL tree when no system TECkit
    # package is available. Configure them directly instead of recursing through
    # every library listed in libs/Makefile.
    [[ -f "$build_dir/libs/teckit/Makefile" ]] || configure_subdir "libs/teckit"
    [[ -f "$build_dir/libs/pplib/Makefile" ]] || configure_subdir "libs/pplib"
    run make -C "$build_dir/libs/teckit" -j"$jobs" all
    run make -C "$build_dir/libs/pplib" -j"$jobs" all
  fi

  [[ -f "$build_dir/texk/web2c/Makefile" ]] || configure_subdir "texk/web2c"
  patch_web2c_makefile_for_native_windows
  seed_generated_xetex_sources

  # The web2c bootstrap stage has multiple generated outputs and is sensitive
  # to stale lock files. Build the requested real target name, not plain
  # "xetex", which can be hijacked by GNU make's built-in Pascal rule.
  run make -r -C "$build_dir/texk/web2c" -j"$jobs" xetex.exe

  restore_bootstrap_files || true
  trap - EXIT

  need_file "$build_dir/texk/web2c/xetex.exe"
  echo "built: $build_dir/texk/web2c/xetex.exe"
  "$build_dir/texk/web2c/xetex.exe" --version | head -8 || true
}

main "$@"
