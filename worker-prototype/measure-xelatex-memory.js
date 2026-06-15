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

const outRoot = path.resolve(repoRoot, readOption('--out', 'out/xelatex-memory'));
const runtimeRoot = path.resolve(repoRoot, readOption('--runtime', 'runtime'));
const launcher = path.join(runtimeRoot, 'run-xelatex.bat');

fs.rmSync(outRoot, { recursive: true, force: true });
fs.mkdirSync(outRoot, { recursive: true });

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function snapshot(rootPid) {
  const script = `
$ErrorActionPreference = 'SilentlyContinue'
$rootIds = @(${rootPid})
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

function summarize(processes) {
  return {
    totalWorkingSetMB: +(processes.reduce((sum, p) => sum + Number(p.WorkingSet64 || 0), 0) / 1024 / 1024).toFixed(1),
    totalPrivateMB: +(processes.reduce((sum, p) => sum + Number(p.PrivateMemorySize64 || 0), 0) / 1024 / 1024).toFixed(1),
    processes: processes.map((p) => ({
      id: p.Id,
      name: p.ProcessName,
      workingSetMB: +(Number(p.WorkingSet64 || 0) / 1024 / 1024).toFixed(1),
      privateMB: +(Number(p.PrivateMemorySize64 || 0) / 1024 / 1024).toFixed(1),
      cpuSeconds: +(Number(p.CPU || 0)).toFixed(3),
    })),
  };
}

function spawnCase(name, texFile = null) {
  const outDir = path.join(outRoot, name);
  fs.mkdirSync(outDir, { recursive: true });
  const args = [
    '/c',
    launcher,
    '-interaction=errorstopmode',
    '-halt-on-error',
    `-output-directory=${outDir}`,
  ];
  if (texFile) {
    args.push(texFile);
  }
  const child = cp.spawn('cmd.exe', args, {
    cwd: repoRoot,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  child.stdin.setDefaultEncoding('utf8');
  return { name, child, buffer: '', ready: false };
}

async function measurePromptOnly() {
  const item = spawnCase('prompt-only');
  await delay(1200);
  const sample = summarize(snapshot(item.child.pid));
  item.child.kill();
  return sample;
}

async function measureReady(name, texFile) {
  const item = spawnCase(name, texFile);
  const start = performance.now();
  item.child.stdout.on('data', (chunk) => {
    item.buffer += chunk.toString('utf8');
    if (item.buffer.includes('WORKER_READY')) {
      item.ready = true;
    }
  });
  item.child.stderr.on('data', (chunk) => {
    item.buffer += chunk.toString('utf8');
    if (item.buffer.includes('WORKER_READY')) {
      item.ready = true;
    }
  });
  while (!item.ready && performance.now() - start < 10000) {
    await delay(25);
  }
  await delay(300);
  const sample = summarize(snapshot(item.child.pid));
  item.child.kill();
  return {
    readyMs: +(performance.now() - start).toFixed(1),
    ...sample,
  };
}

async function main() {
  const summary = {
    promptOnly: await measurePromptOnly(),
    minimalWorker: await measureReady('minimal-worker', 'worker-prototype/worker-minimal-once.tex'),
    fullPreambleWorker: await measureReady('full-preamble-worker', 'worker-prototype/worker-file-request-once.tex'),
  };
  fs.writeFileSync(path.join(outRoot, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
