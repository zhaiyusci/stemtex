#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_root="${STEMTEX_RUNTIME:-$repo_root/dist/stemtex-texlive-daemon-static}"
profile_root="${STEMTEX_PROFILE:-$repo_root/gui/profiles/unicodemath_cjk}"
exe="$repo_root/build/cpp-daemon/Release/stemtex-renderer-smoke.exe"
timeout_s="${TIMEOUT:-90}"
runs="${RUNS:-2}"
spares="${SPARES:-2}"
mode="${1:-quick}"
if [[ $# -gt 0 ]]; then
  shift
fi

win_path() {
  cygpath -w "$1"
}

build_if_needed() {
  if [[ "${BUILD:-1}" != "0" || ! -x "$exe" ]]; then
    "$repo_root/scripts/build-cpp-daemon.sh"
  fi
}

run_smoke() {
  local label="$1"
  shift
  printf '\n==> smoke: %s\n' "$label"
  timeout "${timeout_s}s" "$exe" \
    --repo "$(win_path "$repo_root")" \
    --runtime "$(win_path "$runtime_root")" \
    --profile "$(win_path "$profile_root")" \
    --spares "$spares" \
    "$@"
}

build_if_needed

case "$mode" in
  quick)
    run_smoke validate --case validate
    run_smoke default --runs "$runs"
    run_smoke async --case async --runs 5
    run_smoke recover-no-worker --case recover-no-worker
    ;;
  errors)
    run_smoke bad-stress --case bad-stress --runs "${RUNS:-3}"
    run_smoke cancel --case cancel
    ;;
  timing)
    run_smoke default-hot --runs "${RUNS:-5}"
    run_smoke physics --case physics --runs 1
    run_smoke fonts --case fonts --runs 1
    run_smoke chem-text --case chem-text --runs 1
    ;;
  case)
    if [[ $# -lt 1 ]]; then
      echo "usage: $0 case <name> [extra smoke args...]" >&2
      exit 2
    fi
    case_name="$1"
    shift
    run_smoke "$case_name" --case "$case_name" --runs "$runs" "$@"
    ;;
  *)
    echo "usage: $0 [quick|errors|timing|case <name>]" >&2
    echo "environment: STEMTEX_RUNTIME, RUNS, SPARES, TIMEOUT, BUILD=0" >&2
    exit 2
    ;;
esac
