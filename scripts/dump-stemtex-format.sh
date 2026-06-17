#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage_arg="${1:-$repo_root/dist/stemtex-installer-stage}"
stage="$(cd "$stage_arg" && pwd)"
texlive_root="${TEXLIVE_ROOT:-/c/texlive/2026}"

bin="$stage/bin/windows"
fmt_dir="$stage/texmf-var/web2c/xetex"
ini_src="$texlive_root/texmf-dist/tex/latex/tex-ini-files/xelatex.ini"
ini_dst="$stage/texmf-dist/tex/latex/tex-ini-files/xelatex.ini"

mkdir -p "$(dirname "$ini_dst")" "$fmt_dir"
cp -f "$ini_src" "$ini_dst"
rm -f "$fmt_dir/xelatexdaemon.fmt"
cat >"$fmt_dir/language.dat" <<'EOF'
english hyphen.tex
=usenglish
=USenglish
=american
nohyphenation zerohyph.tex
EOF

export PATH="$bin:/c/Windows/System32:$PATH"
export TEXMFROOT="$(cygpath -w "$stage")"
export TEXMFCNF="$(cygpath -w "$stage/texmf-dist/web2c")"
export TEXFORMATS="$(cygpath -w "$fmt_dir");$(cygpath -w "$fmt_dir")\\"
export TEXINPUTS=".;$(cygpath -w "$stage/texmf-dist/tex/xelatex")//;$(cygpath -w "$stage/texmf-dist/tex/latex")//;$(cygpath -w "$stage/texmf-dist/tex/generic")//;$(cygpath -w "$stage/texmf-dist/tex/plain")//;$(cygpath -w "$stage/texmf-dist/web2c")//;"
export XE_FONTCONFIG_PATH="$(cygpath -w "$stage/texmf-var/fonts/conf")"
export FONTCONFIG_PATH="$XE_FONTCONFIG_PATH"
export XE_FC_CACHEDIR="$(cygpath -w "$stage/texmf-var/fonts/cache")"
export FC_CACHEDIR="$XE_FC_CACHEDIR"
export ICU_DATA="$(cygpath -w "$bin/icu-data")"

(
  cd "$fmt_dir"
  "$bin/xetexdaemon.exe" -ini -etex -jobname=xelatexdaemon "$(cygpath -w "$ini_dst")" </dev/null
)

test -f "$fmt_dir/xelatexdaemon.fmt"
ls -lh "$fmt_dir/xelatexdaemon.fmt"
