#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root="$repo/texlive-xetex"
install_tl="${INSTALL_TL:-/c/texlive/2026/install-tl}"
texdir="${TEXDIR:-$repo/dist/stemtex-tlmgr-root}"
repository="${REPOSITORY:-https://mirror.ctan.org/systems/texlive/tlnet}"
standalone="${STANDALONE_DIR:-$root/out/standalone-msvc}"
packages_file="${PACKAGES_FILE:-$root/stemtex-tlmgr-packages.txt}"
side_tree_source="${SIDE_TREE_SOURCE:-$repo/dist/stemtex-texlive-daemon-static}"
profile="$root/out/stemtex-tlmgr.profile"

die() {
  echo "error: $*" >&2
  exit 1
}

need_file() {
  [[ -f "$1" ]] || die "missing file: $1"
}

need_file "$install_tl"
need_file "$packages_file"
need_file "$standalone/xetexdaemon.exe"
need_file "$standalone/xetexdaemon.dll"
need_file "$standalone/xdvipdfmxdaemon.exe"
need_file "$standalone/dvipdfmxdaemon.dll"
need_file "$root/prebuilt-msvc/share/icu-data/icudt78l.dat"

write_fontconfig_runtime_config() {
  local target_root="$1"
  local conf_dir="$target_root/texmf-var/fonts/conf"
  local cache_dir="$target_root/texmf-var/fonts/cache"
  local texmf_dist="$target_root/texmf-dist"
  local root_win cache_win fonts_otf_win fonts_ttf_win
  mkdir -p "$conf_dir/conf.d" "$cache_dir"
  cp -f "$target_root/tlpkg/tlpostcode/xetex/conf/fonts.dtd" "$conf_dir/fonts.dtd" 2>/dev/null || true
  root_win="$(cygpath -m "$target_root")"
  cache_win="$(cygpath -m "$cache_dir")"
  fonts_otf_win="$(cygpath -m "$texmf_dist/fonts/opentype")"
  fonts_ttf_win="$(cygpath -m "$texmf_dist/fonts/truetype")"
  cat >"$conf_dir/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>C:/Windows/fonts</dir>
  <dir>$fonts_otf_win</dir>
  <dir>$fonts_ttf_win</dir>
  <cachedir>$cache_win</cachedir>
  <include ignore_missing="yes">conf.d</include>
  <config>
    <rescan><int>0</int></rescan>
  </config>
</fontconfig>
EOF
  cat >"$conf_dir/conf.d/51-local.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <include ignore_missing="yes">$root_win/texmf-var/fonts/conf/local.conf</include>
</fontconfig>
EOF
}

mkdir -p "$(dirname "$profile")"
texdir_win="$(cygpath -m "$texdir")"
export PATH="$texdir/bin/windows:$PATH"

cat >"$profile" <<EOF
selected_scheme scheme-infraonly
TEXDIR $texdir_win
TEXMFCONFIG $texdir_win/texmf-config
TEXMFHOME $texdir_win/texmf-home
TEXMFLOCAL $texdir_win/texmf-local
TEXMFSYSCONFIG $texdir_win/texmf-config
TEXMFSYSVAR $texdir_win/texmf-var
TEXMFVAR $texdir_win/texmf-var
binary_windows 1
instopt_adjustpath 0
instopt_adjustrepo 1
instopt_letter 0
instopt_portable 1
instopt_write18_restricted 1
tlpdbopt_autobackup 0
tlpdbopt_backupdir tlpkg/backups
tlpdbopt_create_formats 1
tlpdbopt_desktop_integration 0
tlpdbopt_file_assocs 0
tlpdbopt_generate_updmap 0
tlpdbopt_install_docfiles 0
tlpdbopt_install_srcfiles 0
tlpdbopt_post_code 1
EOF

if [[ ! -f "$texdir/tlpkg/texlive.tlpdb" ]]; then
  echo "==> install-tl scheme-infraonly into $texdir"
  "$install_tl" -force-platform windows -profile "$profile" -repository "$repository"
else
  echo "==> existing tlmgr root: $texdir"
fi

tlmgr="$texdir/bin/windows/tlmgr.bat"
need_file "$tlmgr"

mapfile -t packages < <(grep -vE '^\s*(#|$)' "$packages_file" | grep -vE '^xecjk$')
if ((${#packages[@]})); then
  echo "==> tlmgr install ${#packages[@]} packages"
  "$tlmgr" option repository "$repository"
  "$tlmgr" install "${packages[@]}"
fi

if grep -qE '^xecjk$' "$packages_file"; then
  echo "==> tlmgr install --no-depends xecjk"
  "$tlmgr" install --no-depends xecjk
fi

bin="$texdir/bin/windows"
mkdir -p "$bin/icu-data"
cp -p "$standalone/xetexdaemon.exe" "$bin/xetexdaemon.exe"
cp -p "$standalone/xetexdaemon.dll" "$bin/xetexdaemon.dll"
cp -p "$standalone/xdvipdfmxdaemon.exe" "$bin/xdvipdfmxdaemon.exe"
cp -p "$standalone/dvipdfmxdaemon.dll" "$bin/dvipdfmxdaemon.dll"
rm -f "$bin/icu-data"/icudt*.dat
cp -p "$root/prebuilt-msvc/share/icu-data/icudt78l.dat" "$bin/icu-data/icudt78l.dat"
write_fontconfig_runtime_config "$texdir"

cat >"$texdir/run-xelatexdaemon.bat" <<'EOF'
@echo off
setlocal
set "TLROOT=%~dp0"
if "%TLROOT:~-1%"=="\" set "TLROOT=%TLROOT:~0,-1%"
set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"
set "TEXMFROOT=%TLROOT%"
set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"
set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
if exist "%TLROOT%\bin\windows\icu-data\icudt*l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"
"%TLROOT%\bin\windows\xetexdaemon.exe" -fmt=xelatexdaemon --no-font-cache-refresh %*
EOF

cat >"$bin/xelatexdaemon.bat" <<'EOF'
@echo off
"%~dp0xetexdaemon.exe" -fmt=xelatexdaemon %*
EOF

if [[ -f "$side_tree_source/cache-warmup/warmup.tex" ]]; then
  mkdir -p "$texdir/cache-warmup"
  cp -p "$side_tree_source/cache-warmup/warmup.tex" "$texdir/cache-warmup/warmup.tex"
fi

if [[ -f "$side_tree_source/texmf-dist/tex/latex/ctex/ctexhook.sty" ]]; then
  mkdir -p "$texdir/texmf-dist/tex/latex/ctex"
  cp -p "$side_tree_source/texmf-dist/tex/latex/ctex/ctexhook.sty" "$texdir/texmf-dist/tex/latex/ctex/ctexhook.sty"
  "$bin/mktexlsr.exe" "$texdir/texmf-dist" "$texdir/texmf-var" >/dev/null
fi

"$repo/scripts/dump-stemtex-format.sh" "$texdir"

echo "==> done: $texdir"
echo "==> tlmgr: $tlmgr"
echo "==> daemon: $bin/xetexdaemon.exe"
