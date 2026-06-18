#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_WIN="$(cygpath -w "$ROOT")"
RUNTIME_ROOT="${1:-$(cygpath -w "$ROOT/dist/stemtex-texlive-daemon-static")}"
RUNS="${RUNS:-5}"
TIMEOUT_S="${TIMEOUT:-90}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$ROOT/out/timing/cpp-renderer-$STAMP"
RAW_DIR="$OUT_DIR/raw"
EXE="$ROOT/build/cpp-daemon/Release/stemtex-renderer-smoke.exe"

mkdir -p "$RAW_DIR"

log() {
  printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"
}

find_node() {
  if [ -n "${NODE_JS:-}" ]; then
    printf '%s\n' "$NODE_JS"
    return
  fi
  if command -v node >/dev/null 2>&1; then
    command -v node
    return
  fi
  local bundled="/c/Users/jairy/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe"
  if [ -x "$bundled" ]; then
    printf '%s\n' "$bundled"
    return
  fi
  printf 'node\n'
}

run_case() {
  local name="$1"
  local runs="$2"
  shift 2
  local log_file="$RAW_DIR/$name.log"
  log "case=$name runs=$runs"
  set +e
  timeout "${TIMEOUT_S}s" "$EXE" --repo "$ROOT_WIN" --runtime "$RUNTIME_ROOT" --runs "$runs" "$@" >"$log_file" 2>&1
  local code=$?
  set -e
  printf '%s\n' "$code" >"$RAW_DIR/$name.exit"
  sed -n '1,80p' "$log_file"
  if [ "$code" -ne 0 ]; then
    log "case=$name exited with code $code"
  else
    log "case=$name ok"
  fi
}

log "repo=$ROOT"
log "runtime=$RUNTIME_ROOT"
log "output=$OUT_DIR"

log "building C++ renderer"
"$ROOT/scripts/build-cpp-daemon.sh"

rm -rf "$ROOT/out/cpp-renderer-state" "$ROOT/out/cpp-renderer-renders"

run_case default_hot "$RUNS"
run_case physics 1 --case physics
run_case fonts 1 --case fonts
run_case chem_text 1 --case chem-text
run_case bad_error 1 --case bad
run_case bad_then_good 2 --case bad-then-good-wait

log "generating report"
NODE_JS="$(find_node)"
"$NODE_JS" - "$OUT_DIR" "$RUNTIME_ROOT" "$RUNS" <<'JS'
const fs = require('fs');
const path = require('path');

const outDir = process.argv[2];
const runtimeRoot = process.argv[3];
const runCount = Number(process.argv[4]);
const rawDir = path.join(outDir, 'raw');

function read(name) {
  const logPath = path.join(rawDir, `${name}.log`);
  const exitPath = path.join(rawDir, `${name}.exit`);
  return {
    name,
    code: Number(fs.readFileSync(exitPath, 'utf8').trim()),
    text: fs.readFileSync(logPath, 'utf8'),
    logPath,
  };
}

function parseCase(name) {
  const c = read(name);
  const create = /createMs=(\d+)/.exec(c.text);
  const renderMs = [...c.text.matchAll(/run=(\d+) renderMs=(\d+)/g)].map(m => ({
    run: Number(m[1]),
    renderMs: Number(m[2]),
  }));
  const summaries = [...c.text.matchAll(/summary=(\{[^\r\n]+\})/g)].map(m => JSON.parse(m[1]));
  const pdfs = [...c.text.matchAll(/pdf=([^\r\n]+)/g)].map(m => m[1].trim());
  const error = /render failed: ([\s\S]*)$/.exec(c.text);
  return {
    name,
    code: c.code,
    createMs: create ? Number(create[1]) : null,
    renderMs,
    summaries,
    pdfs,
    errorTail: error ? error[1].slice(-2000) : null,
    log: path.relative(outDir, c.logPath).replaceAll('\\', '/'),
  };
}

function avg(xs) {
  if (!xs.length) return null;
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}

function min(xs) {
  return xs.length ? Math.min(...xs) : null;
}

function max(xs) {
  return xs.length ? Math.max(...xs) : null;
}

const cases = ['default_hot', 'physics', 'fonts', 'chem_text', 'bad_error', 'bad_then_good'].map(parseCase);
const defaultCase = cases.find(c => c.name === 'default_hot');
const hot = defaultCase.summaries;
const summary = {
  generatedAt: new Date().toISOString(),
  repoRoot: path.resolve(outDir, '../../..'),
  runtimeRoot,
  runCount,
  cases,
  aggregates: {
    defaultHot: {
      createMs: defaultCase.createMs,
      requestToPdfMs: {
        avg: avg(hot.map(s => s.requestToPdfMs)),
        min: min(hot.map(s => s.requestToPdfMs)),
        max: max(hot.map(s => s.requestToPdfMs)),
      },
      xdvipdfmxMs: {
        avg: avg(hot.map(s => s.xdvipdfmxMs)),
        min: min(hot.map(s => s.xdvipdfmxMs)),
        max: max(hot.map(s => s.xdvipdfmxMs)),
      },
      finalizeXdvMs: {
        avg: avg(hot.map(s => s.finalizeXdvMs)),
        min: min(hot.map(s => s.finalizeXdvMs)),
        max: max(hot.map(s => s.finalizeXdvMs)),
      },
      newXdvBytes: {
        avg: avg(hot.map(s => s.newXdvBytes)),
        min: min(hot.map(s => s.newXdvBytes)),
        max: max(hot.map(s => s.newXdvBytes)),
      },
      cumulativeXdvBytes: {
        first: hot[0]?.cumulativeXdvBytes ?? null,
        last: hot.at(-1)?.cumulativeXdvBytes ?? null,
      },
    },
  },
};

fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(summary, null, 2));

function fmt(n) {
  if (n === null || n === undefined || Number.isNaN(n)) return 'n/a';
  return Number.isInteger(n) ? String(n) : n.toFixed(1);
}

const lines = [];
lines.push('# StemTeX C++ Renderer Timing Report');
lines.push('');
lines.push(`Generated: ${summary.generatedAt}`);
lines.push(`Runtime: \`${runtimeRoot}\``);
lines.push(`Runs: ${runCount}`);
lines.push('');
lines.push('## Summary');
lines.push('');
lines.push(`- Cold create/warmup/live-worker startup: ${fmt(summary.aggregates.defaultHot.createMs)} ms`);
lines.push(`- Hot request-to-PDF: avg ${fmt(summary.aggregates.defaultHot.requestToPdfMs.avg)} ms, min ${fmt(summary.aggregates.defaultHot.requestToPdfMs.min)} ms, max ${fmt(summary.aggregates.defaultHot.requestToPdfMs.max)} ms`);
lines.push(`- xdvipdfmx: avg ${fmt(summary.aggregates.defaultHot.xdvipdfmxMs.avg)} ms, min ${fmt(summary.aggregates.defaultHot.xdvipdfmxMs.min)} ms, max ${fmt(summary.aggregates.defaultHot.xdvipdfmxMs.max)} ms`);
lines.push(`- XDV finalization: avg ${fmt(summary.aggregates.defaultHot.finalizeXdvMs.avg)} ms`);
lines.push(`- New XDV bytes per request: avg ${fmt(summary.aggregates.defaultHot.newXdvBytes.avg)} bytes`);
lines.push(`- Cumulative XDV bytes: ${fmt(summary.aggregates.defaultHot.cumulativeXdvBytes.first)} -> ${fmt(summary.aggregates.defaultHot.cumulativeXdvBytes.last)}`);
lines.push('');
lines.push('## Cases');
lines.push('');
lines.push('| Case | Exit | Create ms | Render ms | Request-to-PDF ms | xdvipdfmx ms | New XDV bytes | PDF bytes |');
lines.push('| --- | ---: | ---: | --- | --- | --- | --- | --- |');
for (const c of cases) {
  const r = c.renderMs.map(x => x.renderMs);
  const req = c.summaries.map(s => s.requestToPdfMs);
  const conv = c.summaries.map(s => s.xdvipdfmxMs);
  const delta = c.summaries.map(s => s.newXdvBytes);
  const pdf = c.summaries.map(s => s.pdfBytes);
  lines.push(`| ${c.name} | ${c.code} | ${fmt(c.createMs)} | ${r.map(fmt).join(', ') || 'n/a'} | ${req.map(fmt).join(', ') || 'n/a'} | ${conv.map(fmt).join(', ') || 'n/a'} | ${delta.map(fmt).join(', ') || 'n/a'} | ${pdf.map(fmt).join(', ') || 'n/a'} |`);
}
lines.push('');
lines.push('## Notes');
lines.push('');
lines.push('- `default_hot` keeps one renderer process alive and sends 5 render requests after the initial warmup.');
lines.push('- `physics`, `fonts`, and `chem_text` each measure a fresh renderer startup plus one render, so their create time is cold-path cost.');
lines.push('- `bad_error` is expected to fail; it verifies that a TeX error returns quickly with a useful error path instead of hanging.');
lines.push('- `bad_then_good` first sends a bad snippet, then immediately sends a good one to verify hot-spare failover.');
lines.push('- Current PDF conversion path is conservative: cumulative XDV plus `xdvipdfmx -s N-N` for the latest page.');
lines.push('');
lines.push('Raw logs are under `raw/` next to this report.');
fs.writeFileSync(path.join(outDir, 'report.md'), lines.join('\n') + '\n');

console.log(path.join(outDir, 'summary.json'));
console.log(path.join(outDir, 'report.md'));
JS

log "done"
printf 'summary: %s\n' "$OUT_DIR/summary.json"
printf 'report:  %s\n' "$OUT_DIR/report.md"
