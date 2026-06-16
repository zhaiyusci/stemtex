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
const outRoot = path.resolve(repoRoot, readOption('--out', 'out/worker-pool-pressure'));
const workerTex = readOption('--worker', 'worker-prototype/worker-file-request-once.tex');
const poolSize = readNumber('--pool-size', 5);
const idleMs = readNumber('--idle-ms', 5000);
const sampleMs = readNumber('--sample-ms', 250);
const launcher = path.join(runtimeRoot, 'run-xelatexdaemon.bat');

fs.rmSync(outRoot, { recursive: true, force: true });
fs.mkdirSync(outRoot, { recursive: true });

const workers = [];
const startups = [];

function now() {
  return performance.now();
}

function spawnWorker(index) {
  const outDir = path.join(outRoot, `worker-${String(index).padStart(3, '0')}`);
  fs.mkdirSync(outDir, { recursive: true });
  const startTime = now();
  const child = cp.spawn('cmd.exe', [
    '/c',
    launcher,
    '-interaction=errorstopmode',
    '-halt-on-error',
    `-jobname=idle-${index}`,
    `-output-directory=${outDir}`,
    workerTex,
  ], {
    cwd: repoRoot,
    stdio: ['pipe', 'pipe', 'pipe'],
  });

  child.stdin.setDefaultEncoding('utf8');

  const worker = {
    index,
    child,
    startTime,
    readyTime: null,
    buffer: '',
  };

  function handleText(text) {
    worker.buffer += text;
    const lines = worker.buffer.split(/\r?\n/);
    worker.buffer = lines.pop();
    for (const line of lines) {
      if (line.includes('WORKER_READY') && worker.readyTime === null) {
        worker.readyTime = now();
        startups.push({
          workerId: index,
          startupMs: +(worker.readyTime - worker.startTime).toFixed(1),
        });
      }
    }
  }

  child.stdout.on('data', (chunk) => handleText(chunk.toString('utf8')));
  child.stderr.on('data', (chunk) => handleText(chunk.toString('utf8')));
  workers.push(worker);
}

function processSnapshot(rootPids) {
  if (rootPids.length === 0) {
    return [];
  }

  const script = `
$ErrorActionPreference = 'SilentlyContinue'
$rootIds = @(${rootPids.join(',')})
$all = Get-CimInstance Win32_Process
$ids = New-Object 'System.Collections.Generic.HashSet[int]'
foreach ($id in $rootIds) { [void]$ids.Add([int]$id) }
$changed = $true
while ($changed) {
  $changed = $false
  foreach ($p in $all) {
    if ($ids.Contains([int]$p.ParentProcessId) -and -not $ids.Contains([int]$p.ProcessId)) {
      [void]$ids.Add([int]$p.ProcessId)
      $changed = $true
    }
  }
}
$items = foreach ($id in $ids) {
  Get-Process -Id $id | Select-Object Id,ProcessName,CPU,WorkingSet64,PrivateMemorySize64
}
$items | ConvertTo-Json -Compress
`;

  const result = cp.spawnSync('powershell.exe', ['-NoProfile', '-Command', script], {
    encoding: 'utf8',
    windowsHide: true,
  });

  if (result.status !== 0 || result.stdout.trim() === '') {
    return [];
  }

  const parsed = JSON.parse(result.stdout);
  return Array.isArray(parsed) ? parsed : [parsed];
}

function summarizeProcesses(processes) {
  const totalWorkingSetBytes = processes.reduce((sum, item) => sum + Number(item.WorkingSet64 || 0), 0);
  const totalPrivateBytes = processes.reduce((sum, item) => sum + Number(item.PrivateMemorySize64 || 0), 0);
  const totalCpuSeconds = processes.reduce((sum, item) => sum + Number(item.CPU || 0), 0);
  return {
    processCount: processes.length,
    totalWorkingSetMB: +(totalWorkingSetBytes / 1024 / 1024).toFixed(1),
    totalPrivateMB: +(totalPrivateBytes / 1024 / 1024).toFixed(1),
    totalCpuSeconds: +totalCpuSeconds.toFixed(3),
    processes: processes.map((item) => ({
      id: item.Id,
      name: item.ProcessName,
      cpuSeconds: +(Number(item.CPU || 0)).toFixed(3),
      workingSetMB: +(Number(item.WorkingSet64 || 0) / 1024 / 1024).toFixed(1),
      privateMB: +(Number(item.PrivateMemorySize64 || 0) / 1024 / 1024).toFixed(1),
    })),
  };
}

async function waitForReady() {
  while (workers.some((worker) => worker.readyTime === null)) {
    await delay(20);
  }
}

function maxBy(samples, key) {
  return Math.max(...samples.map((item) => item[key] || 0));
}

function avgCpuPercent(samples) {
  if (samples.length < 2) {
    return 0;
  }
  const first = samples[0];
  const last = samples[samples.length - 1];
  const cpuDelta = last.totalCpuSeconds - first.totalCpuSeconds;
  const wallDelta = (last.tMs - first.tMs) / 1000;
  if (wallDelta <= 0) {
    return 0;
  }
  return +((cpuDelta / wallDelta) * 100).toFixed(1);
}

async function main() {
  const startTime = now();
  for (let i = 1; i <= poolSize; i += 1) {
    spawnWorker(i);
  }

  const prewarmSamples = [];
  while (workers.some((worker) => worker.readyTime === null)) {
    const processes = processSnapshot(workers.map((worker) => worker.child.pid));
    prewarmSamples.push({
      tMs: +(now() - startTime).toFixed(1),
      ...summarizeProcesses(processes),
    });
    await delay(sampleMs);
  }
  const readyAtMs = +(now() - startTime).toFixed(1);

  const idleStart = now();
  const idleSamples = [];
  while (now() - idleStart < idleMs) {
    const processes = processSnapshot(workers.map((worker) => worker.child.pid));
    idleSamples.push({
      tMs: +(now() - idleStart).toFixed(1),
      ...summarizeProcesses(processes),
    });
    await delay(sampleMs);
  }

  for (const worker of workers) {
    worker.child.kill();
  }

  const idleLast = idleSamples[idleSamples.length - 1];
  const summary = {
    poolSize,
    idleMs,
    sampleMs,
    readyAtMs,
    startups,
    prewarm: {
      maxWorkingSetMB: +maxBy(prewarmSamples, 'totalWorkingSetMB').toFixed(1),
      maxPrivateMB: +maxBy(prewarmSamples, 'totalPrivateMB').toFixed(1),
      approxAvgCpuPercent: avgCpuPercent(prewarmSamples),
    },
    idle: {
      samples: idleSamples.length,
      lastWorkingSetMB: idleLast.totalWorkingSetMB,
      lastPrivateMB: idleLast.totalPrivateMB,
      maxWorkingSetMB: +maxBy(idleSamples, 'totalWorkingSetMB').toFixed(1),
      maxPrivateMB: +maxBy(idleSamples, 'totalPrivateMB').toFixed(1),
      approxAvgCpuPercent: avgCpuPercent(idleSamples),
      lastProcesses: idleLast.processes,
    },
  };

  fs.writeFileSync(path.join(outRoot, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
  fs.writeFileSync(path.join(outRoot, 'idle-samples.json'), `${JSON.stringify(idleSamples, null, 2)}\n`);
  fs.writeFileSync(path.join(outRoot, 'prewarm-samples.json'), `${JSON.stringify(prewarmSamples, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
}

main().catch((error) => {
  for (const worker of workers) {
    worker.child.kill();
  }
  console.error(error);
  process.exitCode = 1;
});
