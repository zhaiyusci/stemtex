const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const repoRoot = path.resolve(__dirname, '..');
const args = process.argv.slice(2);

function readOption(name, fallback) {
  const index = args.indexOf(name);
  if (index === -1 || index + 1 >= args.length) {
    return fallback;
  }
  return args[index + 1];
}

const runtimeRoot = path.resolve(repoRoot, readOption('--runtime', 'runtime'));
const outDir = path.resolve(repoRoot, readOption('--out', 'out/worker-file-request'));
const requestsDir = path.resolve(repoRoot, readOption('--requests', 'worker-prototype/requests'));
const workerTex = readOption('--worker', 'worker-prototype/worker-file-request.tex');
const requestFiles = fs.readdirSync(requestsDir)
  .filter((name) => /^req\d+\.tex$/.test(name))
  .sort((a, b) => Number(a.match(/\d+/)[0]) - Number(b.match(/\d+/)[0]))
  .map((name) => path.relative(repoRoot, path.join(requestsDir, name)).replaceAll(path.sep, '/'));

if (requestFiles.length === 0) {
  throw new Error(`No req*.tex files found in ${requestsDir}`);
}

fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(outDir, { recursive: true });

const launcher = path.join(runtimeRoot, 'run-xelatex.bat');
const childArgs = [
  '/c',
  launcher,
  '-interaction=errorstopmode',
  '-halt-on-error',
  `-output-directory=${outDir}`,
  workerTex,
];

const child = cp.spawn('cmd.exe', childArgs, {
  cwd: repoRoot,
  stdio: ['pipe', 'pipe', 'pipe'],
});

child.stdin.setDefaultEncoding('utf8');

let buffer = '';
let readyTime = null;
let pending = null;
const startTime = performance.now();
const results = [];

function sendNext() {
  if (pending || results.length >= requestFiles.length) {
    return;
  }

  const file = requestFiles[results.length];
  pending = {
    index: results.length + 1,
    file,
    startTime: performance.now(),
  };
  child.stdin.write(`${file}\n`);
}

function handleText(text) {
  buffer += text;
  const lines = buffer.split(/\r?\n/);
  buffer = lines.pop();

  for (const line of lines) {
    if (line.includes('WORKER_READY') && readyTime === null) {
      readyTime = performance.now();
      sendNext();
    }

    const done = line.match(/WORKER_DONE:(\d+)/);
    if (done && pending) {
      results.push({
        index: pending.index,
        ms: +(performance.now() - pending.startTime).toFixed(1),
        file: pending.file,
      });
      pending = null;
      sendNext();
    }
  }
}

child.stdout.on('data', (chunk) => handleText(chunk.toString('utf8')));
child.stderr.on('data', (chunk) => handleText(chunk.toString('utf8')));

child.on('exit', (code) => {
  const totalMs = +(performance.now() - startTime).toFixed(1);
  const summary = {
    code,
    startupMs: readyTime === null ? null : +(readyTime - startTime).toFixed(1),
    totalMs,
    avgRequestMs: results.length === 0
      ? null
      : +(results.reduce((sum, item) => sum + item.ms, 0) / results.length).toFixed(1),
    results,
    files: fs.readdirSync(outDir).sort(),
  };

  fs.writeFileSync(path.join(outDir, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
});
