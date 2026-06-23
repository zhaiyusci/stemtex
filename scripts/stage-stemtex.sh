#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage_root="${STAGE_ROOT:-${1:-$repo_root/staging}}"
texlive_root="${TEXLIVE_ROOT:-/c/texlive/2026}"
standalone_root="${STANDALONE_DIR:-$repo_root/texlive-xetex/out/standalone-msvc}"
runtime_root="${RUNTIME_ROOT:-$repo_root/dist/stemtex-texlive-daemon-static}"
gui_root="${GUI_ROOT:-$repo_root/build/gui/Release}"
cpp_daemon_root="${CPP_DAEMON_ROOT:-$repo_root/build/cpp-daemon/Release}"

if command -v cygpath >/dev/null 2>&1; then
  stage_root="$(cygpath -u "$stage_root")"
  texlive_root="$(cygpath -u "$texlive_root")"
  standalone_root="$(cygpath -u "$standalone_root")"
  runtime_root="$(cygpath -u "$runtime_root")"
  gui_root="$(cygpath -u "$gui_root")"
  cpp_daemon_root="$(cygpath -u "$cpp_daemon_root")"
fi

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

copy_one() {
  local from_root="$1"
  local to_root="$2"
  local rel="$3"
  local src="$from_root/$rel"
  local dst="$to_root/$rel"
  need_file "$src"
  mkdir -p "$(dirname "$dst")"
  cp -f "$src" "$dst"
}

copy_tree() {
  local from_root="$1"
  local to_root="$2"
  local rel="$3"
  local src="$from_root/$rel"
  local dst="$to_root/$rel"
  need_dir "$src"
  mkdir -p "$(dirname "$dst")"
  rm -rf "$dst"
  cp -a "$src" "$(dirname "$dst")/"
}

copy_var_one() {
  local rel="$1"
  local src="$texlive_root/texmf-var/$rel"
  local dst="$stage_root/runtime/texmf-var/$rel"
  need_file "$src"
  mkdir -p "$(dirname "$dst")"
  cp -f "$src" "$dst"
}

write_texmf_cnf_overlay() {
  local cnf="$stage_root/runtime/texmf-dist/web2c/texmf.cnf"
  local tmp="$cnf.tmp"
  need_file "$cnf"
  cat > "$tmp" <<'EOF'
% StemTeX staged texmf-dist overrides.
% Keep this block at the top: kpathsea uses the first assignment it sees.
TEXMFROOT = $SELFAUTOPARENT
TEXMFDIST = $TEXMFROOT/texmf-dist
TEXMFMAIN = $TEXMFDIST
TEXMFSYSVAR = $TEXMFROOT/texmf-var
TEXMFSYSCONFIG = $TEXMFROOT/texmf-config
TEXMFVAR = $TEXMFROOT/texmf-var
TEXMFCONFIG = $TEXMFROOT/texmf-config
TEXMFLOCAL = $TEXMFROOT/texmf-local
TEXMFHOME = $TEXMFROOT/texmf-home
TEXMF = {$TEXMFVAR,$TEXMFCONFIG,$TEXMFDIST}
TEXMFDBS = {$TEXMFVAR,$TEXMFCONFIG,$TEXMFDIST}
SYSTEXMF = $TEXMFSYSVAR;$TEXMFLOCAL;$TEXMFDIST
TEXMFCACHE = $TEXMFVAR
VARTEXFONTS = $TEXMFVAR/fonts
TEXFORMATS = $TEXMFVAR/web2c/$engine;$TEXMFVAR/web2c
TEXPOOL = $TEXMFVAR/web2c/$engine;$TEXMFDIST/web2c
OSFONTDIR = C:/Windows/fonts
FC_CACHEDIR = $TEXMFVAR/fonts/cache
XE_FC_CACHEDIR = $TEXMFVAR/fonts/cache
FONTCONFIG_PATH = $TEXMFVAR/fonts/conf
XE_FONTCONFIG_PATH = $TEXMFVAR/fonts/conf

EOF
  cat "$cnf" >> "$tmp"
  mv -f "$tmp" "$cnf"
}

write_language_config() {
  local dir="$stage_root/runtime/texmf-dist/tex/generic/config"
  mkdir -p "$dir"
  cat > "$dir/language.dat" <<'EOF'
% StemTeX intentionally keeps format hyphenation minimal.
english hyphen.tex
=usenglish
=USenglish
=american
nohyphenation zerohyph.tex
dumylang dumyhyph.tex
EOF
}

write_fontconfig() {
  local conf_dir="$stage_root/runtime/texmf-var/fonts/conf"
  local root_xml
  root_xml="$(cygpath -m "$stage_root/runtime")"
  mkdir -p "$conf_dir/conf.d" "$stage_root/runtime/texmf-var/fonts/cache"
  cat > "$conf_dir/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>C:/Windows/fonts</dir>
  <dir>$root_xml/texmf-dist/fonts/opentype</dir>
  <dir>$root_xml/texmf-dist/fonts/truetype</dir>
  <cachedir>$root_xml/texmf-var/fonts/cache</cachedir>
  <include ignore_missing="yes">conf.d</include>
  <config><rescan><int>30</int></rescan></config>
</fontconfig>
EOF
  cat > "$conf_dir/conf.d/51-local.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig></fontconfig>
EOF
}

write_run_script() {
  cat > "$stage_root/runtime/run-xelatexdaemon.bat" <<'EOF'
@echo off
setlocal
set "TLROOT=%~dp0"
if "%TLROOT:~-1%"=="\" set "TLROOT=%TLROOT:~0,-1%"
set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"
set "TEXMFROOT=%TLROOT%"
set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"
set "TEXFORMATS=%TLROOT%\texmf-var\web2c\xetex;%TLROOT%\texmf-var\web2c\xetex\"
set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
if exist "%TLROOT%\bin\windows\icu-data\icudt*l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"
"%TLROOT%\bin\windows\xetexdaemon.exe" -fmt=xelatexdaemon --no-font-cache-refresh %*
EOF
}

write_refresh_profile_cache_script() {
  cat > "$stage_root/runtime/refresh-profile-cache.bat" <<'EOF'
@echo off
setlocal
set "TLROOT=%~dp0"
if "%TLROOT:~-1%"=="\" set "TLROOT=%TLROOT:~0,-1%"
set "PROFILE=%~1"
if "%PROFILE%"=="" set "PROFILE=%TLROOT%\..\gui\profiles\unicodemath_cjk"
if not exist "%PROFILE%\warmup.tex" exit /b 0
if not exist "%TLROOT%\texmf-dist\web2c\texmf.cnf" exit /b 0
mkdir "%TLROOT%\texmf-var\fonts\conf\conf.d" >nul 2>nul
mkdir "%TLROOT%\texmf-var\fonts\cache" >nul 2>nul
(
echo ^<?xml version="1.0"?^>
echo ^<!DOCTYPE fontconfig SYSTEM "fonts.dtd"^>
echo ^<fontconfig^>
echo   ^<dir^>C:/Windows/fonts^</dir^>
echo   ^<dir^>%TLROOT:\=/%/texmf-dist/fonts/opentype^</dir^>
echo   ^<dir^>%TLROOT:\=/%/texmf-dist/fonts/truetype^</dir^>
echo   ^<cachedir^>%TLROOT:\=/%/texmf-var/fonts/cache^</cachedir^>
echo   ^<include ignore_missing="yes"^>conf.d^</include^>
echo   ^<config^>^<rescan^>^<int^>30^</int^>^</rescan^>^</config^>
echo ^</fontconfig^>
) > "%TLROOT%\texmf-var\fonts\conf\fonts.conf"
(
echo ^<?xml version="1.0"?^>
echo ^<!DOCTYPE fontconfig SYSTEM "fonts.dtd"^>
echo ^<fontconfig^>^</fontconfig^>
) > "%TLROOT%\texmf-var\fonts\conf\conf.d\51-local.conf"
set "PATH=%TLROOT%\bin\windows;%SystemRoot%\System32"
set "TEXMFROOT=%TLROOT%"
set "TEXMFCNF=%TLROOT%\texmf-dist\web2c"
set "TEXFORMATS=%TLROOT%\texmf-var\web2c\xetex;%TLROOT%\texmf-var\web2c\xetex\"
set "XE_FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "FONTCONFIG_PATH=%TLROOT%\texmf-var\fonts\conf"
set "XE_FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
set "FC_CACHEDIR=%TLROOT%\texmf-var\fonts\cache"
if exist "%TLROOT%\bin\windows\icu-data\icudt*l.dat" set "ICU_DATA=%TLROOT%\bin\windows\icu-data"
del /q "%PROFILE%\warmup.xdv" "%PROFILE%\warmup.log" "%PROFILE%\warmup.aux" >nul 2>nul
"%TLROOT%\bin\windows\xetexdaemon.exe" -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error -output-directory="%PROFILE%" "%PROFILE%\warmup.tex"
if errorlevel 1 exit /b %ERRORLEVEL%
if not exist "%PROFILE%\warmup.xdv" exit /b 3
exit /b 0
EOF
}

stage_texmf_dist() {
  local texmf_dist="$texlive_root/texmf-dist"
  need_dir "$texmf_dist"

  local texmf_files=(
    "dvipdfmx/dvipdfmx.cfg"
    "fonts/map/dvipdfmx/ckx.map"
    "fonts/misc/xetex/fontmapping/base/tex-text.tec"
    "fonts/map/fontname/texfonts.map"
    "tex/latex/ctex/ctexhook.sty"
    "web2c/fmtutil.cnf"
    "web2c/texmf.cnf"
  )
  for rel in "${texmf_files[@]}"; do
    copy_one "$texmf_dist" "$stage_root/runtime/texmf-dist" "$rel"
  done

  local texmf_dirs=(
    "fonts/afm/public/amsfonts"
    "fonts/afm/public/lm"
    "fonts/enc/dvips/lm"
    "fonts/map/dvipdfm/lm"
    "fonts/map/dvips/amsfonts"
    "fonts/map/dvips/lm"
    "fonts/opentype/public/lm"
    "fonts/opentype/public/lm-math"
    "fonts/source/public/amsfonts"
    "fonts/source/public/cm"
    "fonts/source/public/latex-fonts"
    "fonts/tfm/public/amsfonts"
    "fonts/tfm/public/cm"
    "fonts/tfm/public/latex-fonts"
    "fonts/tfm/public/lm"
    "fonts/type1/public/amsfonts"
    "fonts/type1/public/lm"
    "tex/latex/amsmath"
    "tex/latex/amsfonts"
    "tex/latex/base"
    "tex/latex/cancel"
    "tex/latex/tex-ini-files"
    "tex/latex/firstaid"
    "tex/latex/fontspec"
    "tex/latex/chemgreek"
    "tex/generic/babel"
    "tex/generic/config"
    "tex/generic/hyph-utf8"
    "tex/generic/hyphen"
    "tex/generic/iftex"
    "tex/generic/unicode-data"
    "tex/latex/graphics"
    "tex/latex/graphics-cfg"
    "tex/latex/graphics-def"
    "tex/latex/l3backend"
    "tex/latex/l3kernel"
    "tex/latex/l3packages/l3keys2e"
    "tex/latex/l3packages/xparse"
    "tex/latex/l3packages/xtemplate"
    "tex/latex/mathtools"
    "tex/latex/mhchem"
    "tex/latex/preview"
    "tex/latex/siunitx"
    "tex/latex/physics"
    "tex/latex/tools"
    "tex/latex/unicode-math"
    "tex/latex/xcolor"
    "tex/xelatex/xecjk"
    "tex/latex/lm"
    "tex/plain/amsfonts"
  )
  for rel in "${texmf_dirs[@]}"; do
    copy_tree "$texmf_dist" "$stage_root/runtime/texmf-dist" "$rel"
  done

  write_texmf_cnf_overlay
  write_language_config
}

stage_binaries_and_gui() {
  need_dir "$standalone_root"
  need_dir "$gui_root"
  need_dir "$cpp_daemon_root"
  need_file "$runtime_root/texmf-var/web2c/xetex/xelatexdaemon.fmt"
  need_file "$repo_root/texlive-xetex/prebuilt-msvc/share/icu-data/icudt78l.dat"

  mkdir -p \
    "$stage_root/gui" \
    "$stage_root/runtime/bin/windows/icu-data" \
    "$stage_root/runtime/bin/sdk" \
    "$stage_root/runtime/sdk/include" \
    "$stage_root/runtime/sdk/lib" \
    "$stage_root/runtime/texmf-var/web2c/xetex" \
    "$stage_root/runtime/texmf-var/fonts/cache"

  cp -p "$standalone_root/xetexdaemon.exe" "$stage_root/runtime/bin/windows/xetexdaemon.exe"
  cp -p "$standalone_root/xdvipdfmxdaemon.exe" "$stage_root/runtime/bin/windows/xdvipdfmxdaemon.exe"
  cp -p "$standalone_root/xetexdaemon.dll" "$stage_root/runtime/bin/windows/xetexdaemon.dll"
  cp -p "$standalone_root/dvipdfmxdaemon.dll" "$stage_root/runtime/bin/windows/dvipdfmxdaemon.dll"
  cp -p "$repo_root/texlive-xetex/prebuilt-msvc/share/icu-data/icudt78l.dat" \
    "$stage_root/runtime/bin/windows/icu-data/icudt78l.dat"

  cp -p "$cpp_daemon_root/stemtex-renderer.dll" "$stage_root/runtime/bin/sdk/stemtex-renderer.dll"
  cp -p "$cpp_daemon_root/stemtex-renderer.lib" "$stage_root/runtime/sdk/lib/stemtex-renderer.lib"
  cp -p "$repo_root/cpp-daemon/stemtex_renderer.h" "$stage_root/runtime/sdk/include/stemtex_renderer.h"

  cp -p "$runtime_root/texmf-var/web2c/xetex/xelatexdaemon.fmt" \
    "$stage_root/runtime/texmf-var/web2c/xetex/xelatexdaemon.fmt"

  cp -p "$repo_root/cpp-daemon/worker-template.tex" "$stage_root/runtime/worker-template.tex"
  cp -p "$repo_root/VERSION" "$stage_root/runtime/VERSION"
  cp -p "$repo_root/gui/assets/stemtex-renderer-gui.ico" "$stage_root/runtime/StemTeX.ico"
  write_run_script
  write_refresh_profile_cache_script
  write_fontconfig

  rm -rf "$stage_root/runtime/profiles" "$stage_root/gui/profiles"
  mkdir -p "$stage_root/gui/profiles"
  cp -a "$repo_root/gui/profiles/." "$stage_root/gui/profiles/"
  find "$stage_root/gui/profiles" -type f \( \
    -name '*.aux' -o -name '*.log' -o -name '*.pdf' -o -name '*.xdv' -o -name '*.synctex.gz' \
  \) -delete

  if [[ "${PRESERVE_GUI:-0}" != "1" ]]; then
    cp -a "$gui_root/." "$stage_root/gui/"
    rm -f \
      "$stage_root/gui/stemtex-renderer.dll" \
      "$stage_root/gui/stemtex-renderer.lib" \
      "$stage_root/gui/stemtex-renderer.exp"
  fi
}

rm -rf "$stage_root/runtime"
if [[ "${PRESERVE_GUI:-0}" != "1" ]]; then
  rm -rf "$stage_root/gui"
fi
mkdir -p "$stage_root/gui" "$stage_root/runtime/texmf-dist"

stage_texmf_dist
stage_binaries_and_gui

if command -v cygpath >/dev/null 2>&1; then
  echo "Staged StemTeX: $(cygpath -w "$stage_root")"
else
  echo "Staged StemTeX: $stage_root"
fi
du -sh "$stage_root" "$stage_root/gui" "$stage_root/runtime" "$stage_root/runtime/texmf-dist" 2>/dev/null || true
