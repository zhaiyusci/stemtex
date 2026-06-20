#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_root="${1:-$repo_root/dist/stemtex-texlive-daemon-static}"
runtime_root="$(cd "$runtime_root" && pwd)"
profile_root="${2:-${STEMTEX_PROFILE:-}}"
if [[ -z "$profile_root" ]]; then
  echo "usage: $0 <runtime-root> <profile-root>" >&2
  echo "or set STEMTEX_PROFILE=/path/to/profile" >&2
  exit 2
fi
profile_root="$(cd "$profile_root" && pwd)"

bin="$runtime_root/bin/windows"
fmt_dir="$runtime_root/texmf-var/web2c/xetex"
warmup_dir="$profile_root"
output_dir="$profile_root"
cache_dir="$runtime_root/texmf-var/fonts/cache"
conf_dir="$runtime_root/texmf-var/fonts/conf"

mkdir -p "$output_dir" "$cache_dir" "$conf_dir/conf.d"

root_xml="$(/usr/bin/cygpath -m "$runtime_root")"
cat >"$conf_dir/fonts.conf" <<EOF
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

cat >"$conf_dir/conf.d/51-local.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig></fontconfig>
EOF

texmfroot_win="$(/usr/bin/cygpath -w "$runtime_root")"
texmfcnf_win="$(/usr/bin/cygpath -w "$runtime_root/texmf-dist/web2c")"
fmt_dir_win="$(/usr/bin/cygpath -w "$fmt_dir")"
icu_data_win="$(/usr/bin/cygpath -w "$bin/icu-data")"
conf_dir_win="$(/usr/bin/cygpath -w "$conf_dir")"
cache_dir_win="$(/usr/bin/cygpath -w "$cache_dir")"
output_dir_win="$(/usr/bin/cygpath -w "$output_dir")"

export PATH="$bin:/c/Windows/System32"
export TEXMFROOT="$texmfroot_win"
export TEXMFCNF="$texmfcnf_win"
export TEXFORMATS="$fmt_dir_win;$fmt_dir_win\\"
export ICU_DATA="$icu_data_win"
export XE_FONTCONFIG_PATH="$conf_dir_win"
export FONTCONFIG_PATH="$XE_FONTCONFIG_PATH"
export XE_FC_CACHEDIR="$cache_dir_win"
export FC_CACHEDIR="$XE_FC_CACHEDIR"
unset FONTCONFIG_NO_CACHE_REFRESH

/usr/bin/rm -f "$output_dir/warmup.xdv" "$output_dir/warmup.log" "$output_dir/warmup.aux"

(
  cd "$warmup_dir"
  "$bin/xetexdaemon.exe" -fmt=xelatexdaemon -no-pdf -interaction=nonstopmode -halt-on-error \
    -output-directory="$output_dir_win" warmup.tex
)

test -f "$output_dir/warmup.xdv"
/usr/bin/ls -lh "$output_dir/warmup.xdv"
