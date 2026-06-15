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

function readNumber(name, fallback) {
  const value = Number(readOption(name, fallback));
  if (!Number.isFinite(value)) {
    throw new Error(`${name} must be a number`);
  }
  return value;
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

const runtimeRoot = path.resolve(repoRoot, readOption('--runtime', 'runtime'));
const outRoot = path.resolve(repoRoot, readOption('--out', 'out/worker-pool'));
const requestsDir = path.resolve(repoRoot, readOption('--requests', 'worker-prototype/requests'));
const workerTex = readOption('--worker', 'worker-prototype/worker-file-request-once.tex');
const poolSize = readNumber('--pool-size', 5);
const spacingMs = readNumber('--spacing-ms', 0);
const launcher = path.join(runtimeRoot, 'run-xelatex.bat');

const requestFiles = fs.readdirSync(requestsDir)
  .filter((name) => /^req\d+\.tex$/.test(name))
  .sort((a, b) => Number(a.match(/\d+/)[0]) - Number(b.match(/\d+/)[0]))
  .map((name) => path.relative(repoRoot, path.join(requestsDir, name)).replaceAll(path.sep, '/'));

if (requestFiles.length === 0) {
  throw new Error(`No req*.tex files found in ${requestsDir}`);
}

fs.rmSync(outRoot, { recursive: true, force: true });
fs.mkdirSync(outRoot, { recursive: true });

let nextWorkerId = 1;
const idleWorkers = [];
const allWorkers = new Set();
const waiters = [];
const workerStartups = [];

function now() {
  return performance.now();
}

function relativeMs(base) {
  return +(now() - base).toFixed(1);
}

function takeReadyWorker() {
  if (idleWorkers.length > 0) {
    return Promise.resolve(idleWorkers.shift());
  }
  return new Promise((resolve) => waiters.push(resolve));
}

function publishReady(worker) {
  if (waiters.length > 0) {
    waiters.shift()(worker);
    return;
  }
  idleWorkers.push(worker);
}

function spawnWorker() {
  const id = nextWorkerId++;
  const outDir = path.join(outRoot, `worker-${String(id).padStart(3, '0')}`);
  fs.mkdirSync(outDir, { recursive: true });

  const startTime = now();
  const childArgs = [
    '/c',
    launcher,
    '-interaction=errorstopmode',
    '-halt-on-error',
    `-jobname=snippet-${id}`,
    `-output-directory=${outDir}`,
    workerTex,
  ];

  const child = cp.spawn('cmd.exe', childArgs, {
    cwd: repoRoot,
    stdio: ['pipe', 'pipe', 'pipe'],
  });

  child.stdin.setDefaultEncoding('utf8');

  const worker = {
    id,
    child,
    outDir,
    startTime,
    readyTime: null,
    request: null,
    doneTime: null,
    exitTime: null,
    buffer: '',
    stdoutTail: '',
  };

  allWorkers.add(worker);

  function handleText(text) {
    worker.stdoutTail = (worker.stdoutTail + text).slice(-2000);
    worker.buffer += text;
    const lines = worker.buffer.split(/\r?\n/);
    worker.buffer = lines.pop();

    for (const line of lines) {
      if (line.includes('WORKER_READY') && worker.readyTime === null) {
        worker.readyTime = now();
        workerStartups.push({
          workerId: worker.id,
          startupMs: +(worker.readyTime - worker.startTime).toFixed(1),
        });
        publishReady(worker);
      }

      if (line.match(/WORKER_DONE:1/) && worker.request) {
        worker.doneTime = now();
      }
    }
  }

  child.stdout.on('data', (chunk) => handleText(chunk.toString('utf8')));
  child.stderr.on('data', (chunk) => handleText(chunk.toString('utf8')));

  child.on('exit', (code) => {
    worker.exitTime = now();
    worker.exitCode = code;
    allWorkers.delete(worker);
    if (worker.resolveExit) {
      worker.resolveExit(worker);
    }
  });

  return worker;
}

async function useWorkerForRequest(requestFile, requestIndex, benchmarkStart) {
  const arrivalTime = now();
  const worker = await takeReadyWorker();
  const dispatchTime = now();
  const pdfPath = path.join(worker.outDir, `snippet-${worker.id}.pdf`);

  worker.request = {
    requestFile,
    requestIndex,
    arrivalTime,
    dispatchTime,
  };

  const exitPromise = new Promise((resolve) => {
    worker.resolveExit = resolve;
  });

  worker.child.stdin.write(`${requestFile}\n`);
  const exited = await exitPromise;
  const exitTime = exited.exitTime || now();
  const pdfExists = fs.existsSync(pdfPath);
  const pdfSize = pdfExists ? fs.statSync(pdfPath).size : 0;

  spawnWorker();

  return {
    index: requestIndex,
    requestFile,
    workerId: worker.id,
    waitForReadyMs: +(dispatchTime - arrivalTime).toFixed(1),
    hotWorkerAgeMs: +(dispatchTime - worker.readyTime).toFixed(1),
    dispatchToDoneMs: worker.doneTime === null ? null : +(worker.doneTime - dispatchTime).toFixed(1),
    dispatchToPdfMs: +(exitTime - dispatchTime).toFixed(1),
    arrivalToPdfMs: +(exitTime - arrivalTime).toFixed(1),
    tArrivalMs: +(arrivalTime - benchmarkStart).toFixed(1),
    tPdfMs: +(exitTime - benchmarkStart).toFixed(1),
    exitCode: exited.exitCode,
    pdfPath: path.relative(repoRoot, pdfPath).replaceAll(path.sep, '/'),
    pdfSize,
  };
}

async function main() {
  const prewarmStart = now();
  for (let i = 0; i < poolSize; i += 1) {
    spawnWorker();
  }

  while (idleWorkers.length < poolSize) {
    await delay(10);
  }

  const benchmarkStart = now();
  const pending = [];

  for (let i = 0; i < requestFiles.length; i += 1) {
    if (spacingMs > 0 && i > 0) {
      await delay(spacingMs);
    }
    pending.push(useWorkerForRequest(requestFiles[i], i + 1, benchmarkStart));
  }

  const results = await Promise.all(pending);
  const benchmarkEnd = now();

  for (const worker of [...allWorkers]) {
    worker.child.kill();
  }

  const summary = {
    poolSize,
    spacingMs,
    prewarmMs: +(benchmarkStart - prewarmStart).toFixed(1),
    totalBenchmarkMs: +(benchmarkEnd - benchmarkStart).toFixed(1),
    avgArrivalToPdfMs: +(results.reduce((sum, item) => sum + item.arrivalToPdfMs, 0) / results.length).toFixed(1),
    avgDispatchToPdfMs: +(results.reduce((sum, item) => sum + item.dispatchToPdfMs, 0) / results.length).toFixed(1),
    workerStartups,
    results,
  };

  fs.writeFileSync(path.join(outRoot, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
