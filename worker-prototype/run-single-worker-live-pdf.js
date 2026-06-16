const fs = require('fs');
const path = require('path');
const cp = require('child_process');

const repoRoot = path.resolve(__dirname, '..');
const args = process.argv.slice(2);
const WORKER_STOP = '\\workerstop';

function readOption(name, fallback) {
  const index = args.indexOf(name);
  if (index === -1 || index + 1 >= args.length) {
    return fallback;
  }
  return args[index + 1];
}

function be32(value) {
  const b = Buffer.alloc(4);
  b.writeInt32BE(value, 0);
  return b;
}

function be16(value) {
  const b = Buffer.alloc(2);
  b.writeUInt16BE(value, 0);
  return b;
}

function readXdvParts(xdvPath) {
  const xdv = fs.readFileSync(xdvPath);
  const firstBop = xdv.indexOf(Buffer.from([139]));
  if (firstBop < 0) {
    throw new Error(`No BOP found in ${xdvPath}`);
  }
  const postOffset = xdv.lastIndexOf(Buffer.from([248]));
  if (postOffset < 0) {
    throw new Error(`No postamble found in ${xdvPath}`);
  }
  const postPostOffset = xdv.indexOf(Buffer.from([249]), postOffset);
  if (postPostOffset < 0) {
    throw new Error(`No post_post found in ${xdvPath}`);
  }
  return {
    preamble: xdv.subarray(0, firstBop),
    fontdefs: xdv.subarray(postOffset + 29, postPostOffset),
  };
}

function findFirstBop(buffer, label) {
  const offset = buffer.indexOf(Buffer.from([139]));
  if (offset < 0) {
    throw new Error(`No BOP found in ${label}`);
  }
  return offset;
}

function finalizeXdvBody(xdvBody, finalizedPath, { fontdefs, pageCount, lastBop }) {
  if (xdvBody[0] !== 247) {
    throw new Error(`Expected complete XDV body for ${finalizedPath}`);
  }
  let finalLastBop = lastBop;
  if (finalLastBop === undefined) {
    finalLastBop = findFirstBop(xdvBody, finalizedPath);
  }
  if (finalLastBop < 0 || finalLastBop >= xdvBody.length || xdvBody[finalLastBop] !== 139) {
    throw new Error(`No BOP found in XDV body for ${finalizedPath}`);
  }

  const postOffset = xdvBody.length;
  const chunks = [
    xdvBody,
    Buffer.from([248]),
    be32(finalLastBop),
    be32(25400000),
    be32(473628672),
    be32(1000),
    be32(0x1d200000),
    be32(0x1d200000),
    be16(20),
    be16(pageCount),
    fontdefs,
    Buffer.from([249]),
    be32(postOffset),
    Buffer.from([7, 223, 223, 223, 223]),
  ];
  let out = Buffer.concat(chunks);
  while (out.length % 4 !== 0) {
    out = Buffer.concat([out, Buffer.from([223])]);
  }
  fs.writeFileSync(finalizedPath, out);
  return out.length;
}

function finalizePartialXdv(partialPath, finalizedPath, parts, pageCount, lastBop) {
  const body = fs.readFileSync(partialPath);
  return finalizeXdvBody(body, finalizedPath, { fontdefs: parts.fontdefs, pageCount, lastBop });
}

function readFileRange(filePath, start, end) {
  const length = end - start;
  if (length <= 0) {
    throw new Error(`No new XDV bytes available in ${filePath}`);
  }
  const fd = fs.openSync(filePath, 'r');
  try {
    const buffer = Buffer.alloc(length);
    fs.readSync(fd, buffer, 0, length, start);
    return buffer;
  } finally {
    fs.closeSync(fd);
  }
}

function xdvJobBaseName(workerTex) {
  return path.basename(workerTex).replace(/\.tex$/i, '');
}

function runWarmup({ launcher, outDir, workerTex, requests }) {
  fs.mkdirSync(outDir, { recursive: true });
  const child = cp.spawn('cmd.exe', [
    '/c',
    launcher,
    '-interaction=errorstopmode',
    '-halt-on-error',
    '-no-pdf',
    '-flush-output-on-shipout',
    `-output-directory=${outDir}`,
    workerTex,
  ], { cwd: repoRoot, stdio: ['pipe', 'pipe', 'pipe'] });

  child.stdin.setDefaultEncoding('utf8');
  let buffer = '';
  let sent = 0;
  let done = 0;

  return new Promise((resolve, reject) => {
    function sendNext() {
      if (sent < requests.length) {
        child.stdin.write(`${requests[sent++]}\n`);
      } else {
        child.stdin.write(`${WORKER_STOP}\n`);
      }
    }

    function handleText(text) {
      buffer += text;
      const lines = buffer.split(/\r?\n/);
      buffer = lines.pop();
      for (const line of lines) {
        if (line.includes('WORKER_READY')) {
          sendNext();
        }
        if (line.match(/WORKER_DONE:(\d+)/)) {
          done += 1;
          sendNext();
        }
      }
    }

    child.stdout.on('data', (chunk) => handleText(chunk.toString('utf8')));
    child.stderr.on('data', (chunk) => handleText(chunk.toString('utf8')));
    child.on('exit', (code) => {
      if (code !== 0) {
        reject(new Error(`warmup worker exited with code ${code}`));
        return;
      }
      resolve({ done });
    });
  });
}

async function main() {
  const runtimeRoot = path.resolve(repoRoot, readOption('--runtime', 'runtime'));
  const outRoot = path.resolve(repoRoot, readOption('--out', 'out/single-worker-live-pdf'));
  const requestsDir = path.resolve(repoRoot, readOption('--requests', 'worker-prototype/requests'));
  const workerTex = readOption('--worker', 'worker-file-request-prototype.tex');
  const cumulativePdf = args.includes('--cumulative');
  const pdfMode = cumulativePdf ? 'cumulative' : 'latest-page';
  const launcher = path.join(runtimeRoot, 'run-xelatex.bat');
  const xdvipdfmx = path.join(runtimeRoot, 'bin', 'windows', 'xdvipdfmx.exe');
  const jobBaseName = xdvJobBaseName(workerTex);
  const requestFiles = fs.readdirSync(requestsDir)
    .filter((name) => /^req\d+\.tex$/.test(name))
    .sort((a, b) => Number(a.match(/\d+/)[0]) - Number(b.match(/\d+/)[0]))
    .map((name) => path.relative(repoRoot, path.join(requestsDir, name)).replaceAll(path.sep, '/'));

  fs.rmSync(outRoot, { recursive: true, force: true });
  fs.mkdirSync(outRoot, { recursive: true });

  const warmupDir = path.join(outRoot, 'fontdefs-warmup');
  const warmupStart = performance.now();
  await runWarmup({ launcher, outDir: warmupDir, workerTex, requests: requestFiles });
  const warmupMs = +(performance.now() - warmupStart).toFixed(1);
  const xdvParts = readXdvParts(path.join(warmupDir, `${jobBaseName}.xdv`));
  fs.writeFileSync(path.join(outRoot, 'preamble.bin'), xdvParts.preamble);
  fs.writeFileSync(path.join(outRoot, 'fontdefs.bin'), xdvParts.fontdefs);

  const liveDir = path.join(outRoot, 'live');
  fs.mkdirSync(liveDir, { recursive: true });
  const child = cp.spawn('cmd.exe', [
    '/c',
    launcher,
    '-interaction=errorstopmode',
    '-halt-on-error',
    '-no-pdf',
    '-flush-output-on-shipout',
    `-output-directory=${liveDir}`,
    workerTex,
  ], { cwd: repoRoot, stdio: ['pipe', 'pipe', 'pipe'] });

  child.stdin.setDefaultEncoding('utf8');
  let buffer = '';
  let sent = 0;
  let done = 0;
  let readyTime = null;
  let pending = null;
  let lastXdvOffset = 0;
  const liveStart = performance.now();
  const results = [];

  function sendNext() {
    if (sent >= requestFiles.length || pending) {
      return;
    }
    pending = {
      index: sent + 1,
      file: requestFiles[sent],
      start: performance.now(),
    };
    child.stdin.write(`${requestFiles[sent++]}\n`);
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

      if (line.match(/WORKER_DONE:(\d+)/) && pending) {
        done += 1;
        const doneTime = performance.now();
        const xdvPath = path.join(liveDir, `${jobBaseName}.xdv`);
        const snapshotPath = path.join(liveDir, `snippet-${done}.xdv`);
        const finalizedPath = path.join(liveDir, `snippet-${done}-final.xdv`);
        const pdfPath = path.join(liveDir, `snippet-${done}.pdf`);
        const readStart = performance.now();
        const currentXdvSize = fs.statSync(xdvPath).size;
        const delta = readFileRange(xdvPath, lastXdvOffset, currentXdvSize);
        const deltaBop = findFirstBop(delta, `${xdvPath} new bytes`);
        fs.copyFileSync(xdvPath, snapshotPath);
        const readEnd = performance.now();
        const newXdvBytes = currentXdvSize - lastXdvOffset;
        const finalizeStart = performance.now();
        const finalXdvBytes = finalizePartialXdv(snapshotPath, finalizedPath, xdvParts, done, lastXdvOffset + deltaBop);
        const finalizeEnd = performance.now();
        lastXdvOffset = currentXdvSize;
        const convertStart = performance.now();
        const convertArgs = ['-q'];
        if (!cumulativePdf) {
          convertArgs.push('-s', `${done}-${done}`);
        }
        convertArgs.push('-o', pdfPath, finalizedPath);
        const convert = cp.spawnSync(xdvipdfmx, convertArgs, {
          cwd: repoRoot,
          encoding: 'utf8',
          timeout: 10000,
        });
        const end = performance.now();
        results.push({
          index: pending.index,
          file: pending.file,
          typesetToDoneMs: +(doneTime - pending.start).toFixed(1),
          readDeltaMs: +(readEnd - readStart).toFixed(1),
          finalizeXdvMs: +(finalizeEnd - finalizeStart).toFixed(1),
          requestToPdfMs: +(end - pending.start).toFixed(1),
          xdvipdfmxMs: +(end - convertStart).toFixed(1),
          newXdvBytes,
          finalXdvBytes,
          convertCode: convert.status,
          pdfBytes: fs.existsSync(pdfPath) ? fs.statSync(pdfPath).size : 0,
          stderrTail: (convert.stderr || '').trim().slice(-500),
        });
        pending = null;
        sendNext();
      }
    }
  }

  child.stdout.on('data', (chunk) => handleText(chunk.toString('utf8')));
  child.stderr.on('data', (chunk) => handleText(chunk.toString('utf8')));

  while (results.length < requestFiles.length) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  const liveDoneMs = +(performance.now() - liveStart).toFixed(1);
  child.kill();
  await new Promise((resolve) => child.on('exit', resolve));

  const summary = {
    warmupMs,
    preambleBytes: xdvParts.preamble.length,
    fontdefsBytes: xdvParts.fontdefs.length,
    startupMs: readyTime === null ? null : +(readyTime - liveStart).toFixed(1),
    liveDoneMs,
    pdfMode,
    avgRequestToPdfMs: +(results.reduce((sum, item) => sum + item.requestToPdfMs, 0) / results.length).toFixed(1),
    avgXdvipdfmxMs: +(results.reduce((sum, item) => sum + item.xdvipdfmxMs, 0) / results.length).toFixed(1),
    avgFinalizeXdvMs: +(results.reduce((sum, item) => sum + item.finalizeXdvMs, 0) / results.length).toFixed(1),
    results,
  };
  fs.writeFileSync(path.join(outRoot, 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
  console.log(JSON.stringify(summary, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
