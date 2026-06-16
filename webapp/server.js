const fs = require('fs');
const http = require('http');
const path = require('path');
const cp = require('child_process');
const { URL } = require('url');

const repoRoot = path.resolve(__dirname, '..');
const publicRoot = path.join(__dirname, 'public');
const port = Number(process.env.PORT || 5177);
const runtimeRoot = path.resolve(repoRoot, process.env.XETEX_RUNTIME || findDefaultRuntime());
const workerTex = 'webapp/worker-webapp.tex';
const rendersRoot = path.join(repoRoot, 'out', 'webapp-renders');
const stateRoot = path.join(repoRoot, 'out', 'webapp-worker-state');
const WORKER_STOP = '\\workerstop';
const DEFAULT_WARMUP_SNIPPET = [
  '这是一个小段中文和数学的预览：$E=mc^2$，以及 \\textcolor{blue}{蓝色文字}。',
  '',
  '\\[',
  '  \\int_0^1 x^2\\,dx = \\frac{1}{3}',
  '\\]',
  '',
  '\\ce{H2O} 与 $\\ip{1}{0}$。',
  '',
].join('\n');

function findDefaultRuntime() {
  const candidates = ['stemtex', 'runtime', '../stemtex'];
  for (const candidate of candidates) {
    const full = path.resolve(repoRoot, candidate);
    if (fs.existsSync(path.join(full, 'run-xelatexdaemon.bat'))) {
      return candidate;
    }
  }
  return 'runtime';
}

function send(res, status, body, type = 'text/plain; charset=utf-8') {
  res.writeHead(status, { 'Content-Type': type });
  res.end(body);
}

function sendJson(res, status, value) {
  send(res, status, JSON.stringify(value), 'application/json; charset=utf-8');
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > 512 * 1024) {
        reject(new Error('Request body is too large.'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function safeWidth(value) {
  const width = Number(value);
  if (!Number.isFinite(width)) {
    return 360;
  }
  return Math.max(180, Math.min(430, Math.round(width)));
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
  let out = Buffer.concat([
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
  ]);
  while (out.length % 4 !== 0) {
    out = Buffer.concat([out, Buffer.from([223])]);
  }
  fs.writeFileSync(finalizedPath, out);
  return out.length;
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

function launcherPath() {
  return path.join(runtimeRoot, 'run-xelatexdaemon.bat');
}

function xdvipdfmxPath() {
  return path.join(runtimeRoot, 'bin', 'windows', 'xdvipdfmx.exe');
}

function spawnXeTeX(outDir) {
  return cp.spawn('cmd.exe', [
    '/c',
    launcherPath(),
    '-interaction=errorstopmode',
    '-halt-on-error',
    '-no-pdf',
    '-flush-output-on-shipout',
    `-output-directory=${outDir}`,
    workerTex,
  ], { cwd: repoRoot, stdio: ['pipe', 'pipe', 'pipe'] });
}

function runWarmup(snippet = DEFAULT_WARMUP_SNIPPET) {
  const outDir = path.join(stateRoot, 'fontdefs-warmup');
  const requestDir = path.join(stateRoot, 'warmup-request');
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.rmSync(requestDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });
  fs.mkdirSync(requestDir, { recursive: true });
  const requestPath = path.join(requestDir, 'req1.tex');
  fs.writeFileSync(requestPath, snippet, 'utf8');
  const child = spawnXeTeX(outDir);
  child.stdin.setDefaultEncoding('utf8');
  let buffer = '';
  let sent = false;
  let stdout = '';
  let stderr = '';

  return new Promise((resolve, reject) => {
    function sendWarmup() {
      if (sent) {
        return;
      }
      sent = true;
      child.stdin.write('360pt\n');
      child.stdin.write(`${path.relative(repoRoot, requestPath).replaceAll(path.sep, '/')}\n`);
    }

    function handleText(text) {
      buffer += text;
      const lines = buffer.split(/\r?\n/);
      buffer = lines.pop();
      for (const line of lines) {
        if (line.includes('WORKER_READY')) {
          sendWarmup();
        } else if (line.match(/WORKER_DONE:(\d+)/)) {
          child.stdin.write(`${WORKER_STOP}\n`);
        }
      }
    }

    child.stdout.on('data', (chunk) => {
      const text = chunk.toString('utf8');
      stdout += text;
      handleText(text);
    });
    child.stderr.on('data', (chunk) => {
      const text = chunk.toString('utf8');
      stderr += text;
      handleText(text);
    });
    child.on('exit', (code) => {
      if (code !== 0) {
        reject(new Error(`Warmup exited with code ${code}\n${stdout}\n${stderr}`));
        return;
      }
      resolve(readXdvParts(path.join(outDir, 'worker-webapp.xdv')));
    });
  });
}

class LiveWorker {
  constructor(xdvParts) {
    this.xdvParts = xdvParts;
    this.outDir = path.join(stateRoot, 'live');
    this.child = null;
    this.buffer = '';
    this.ready = false;
    this.pending = null;
    this.queue = Promise.resolve();
    this.lastXdvOffset = 0;
    this.nextRequest = 0;
  }

  async start() {
    fs.rmSync(this.outDir, { recursive: true, force: true });
    fs.mkdirSync(this.outDir, { recursive: true });
    this.child = spawnXeTeX(this.outDir);
    this.child.stdin.setDefaultEncoding('utf8');
    this.child.stdout.on('data', (chunk) => this.handleText(chunk.toString('utf8')));
    this.child.stderr.on('data', (chunk) => this.handleText(chunk.toString('utf8')));
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('Live worker did not become ready.')), 30000);
      this.onReady = () => {
        clearTimeout(timer);
        resolve();
      };
    });
  }

  handleText(text) {
    this.buffer += text;
    const lines = this.buffer.split(/\r?\n/);
    this.buffer = lines.pop();
    for (const line of lines) {
      if (line.includes('WORKER_READY')) {
        this.ready = true;
        if (this.onReady) {
          this.onReady();
          this.onReady = null;
        }
      }
      const done = line.match(/WORKER_DONE:(\d+)/);
      if (done && this.pending) {
        const pending = this.pending;
        this.pending = null;
        this.finishRequest(pending).then(pending.resolve, pending.reject);
      }
    }
  }

  render(snippet, widthPt) {
    this.queue = this.queue.then(() => this.renderNow(snippet, widthPt));
    return this.queue;
  }

  renderNow(snippet, widthPt) {
    return new Promise((resolve, reject) => {
      const id = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
      const renderDir = path.join(rendersRoot, id);
      const requestDir = path.join(renderDir, 'requests');
      fs.mkdirSync(requestDir, { recursive: true });
      const requestPath = path.join(requestDir, 'req1.tex');
      fs.writeFileSync(requestPath, snippet, 'utf8');
      this.pending = {
        id,
        renderDir,
        widthPt,
        start: performance.now(),
        resolve,
        reject,
      };
      this.child.stdin.write(`${widthPt}pt\n`);
      this.child.stdin.write(`${path.relative(repoRoot, requestPath).replaceAll(path.sep, '/')}\n`);
    });
  }

  async finishRequest(pending) {
    const xdvPath = path.join(this.outDir, 'worker-webapp.xdv');
    const currentXdvSize = fs.statSync(xdvPath).size;
    const cumulative = readFileRange(xdvPath, 0, currentXdvSize);
    const delta = readFileRange(xdvPath, this.lastXdvOffset, currentXdvSize);
    const lastBop = this.lastXdvOffset + findFirstBop(delta, `${xdvPath} delta`);
    this.lastXdvOffset = currentXdvSize;

    const liveOut = path.join(pending.renderDir, 'out', 'live');
    fs.mkdirSync(liveOut, { recursive: true });
    const requestNo = ++this.nextRequest;
    const cumulativePath = path.join(liveOut, 'snippet-1-cumulative.xdv');
    const finalPath = path.join(liveOut, 'snippet-1-final.xdv');
    const pdfPath = path.join(liveOut, 'snippet-1.pdf');
    fs.writeFileSync(cumulativePath, cumulative);
    const finalizeStart = performance.now();
    const finalXdvBytes = finalizeXdvBody(cumulative, finalPath, { ...this.xdvParts, pageCount: requestNo, lastBop });
    const convertStart = performance.now();
    const convert = cp.spawnSync(xdvipdfmxPath(), ['-q', '-s', `${requestNo}-${requestNo}`, '-o', pdfPath, finalPath], {
      cwd: repoRoot,
      encoding: 'utf8',
      timeout: 10000,
    });
    const end = performance.now();
    if (convert.status !== 0) {
      throw new Error(`xdvipdfmx failed: ${(convert.stderr || '').trim()}`);
    }
    const summary = {
      pdfMode: 'webapp-live-worker-latest-page',
      widthPt: pending.widthPt,
      requestToPdfMs: +(end - pending.start).toFixed(1),
      finalizeXdvMs: +(convertStart - finalizeStart).toFixed(1),
      xdvipdfmxMs: +(end - convertStart).toFixed(1),
      cumulativeXdvBytes: cumulative.length,
      newXdvBytes: delta.length,
      finalXdvBytes,
      pdfBytes: fs.statSync(pdfPath).size,
      workerRequest: requestNo,
    };
    fs.writeFileSync(path.join(pending.renderDir, 'out', 'summary.json'), `${JSON.stringify(summary, null, 2)}\n`);
    return {
      id: pending.id,
      pdfUrl: `/renders/${pending.id}/out/live/snippet-1.pdf`,
      summary: { results: [summary], ...summary },
    };
  }

  stop() {
    if (this.child && !this.child.killed) {
      this.child.kill();
    }
  }
}

let appReady;
let liveWorker;

async function ensureWorker() {
  if (!appReady) {
    appReady = (async () => {
      fs.mkdirSync(rendersRoot, { recursive: true });
      fs.mkdirSync(stateRoot, { recursive: true });
      const xdvParts = await runWarmup();
      liveWorker = new LiveWorker(xdvParts);
      await liveWorker.start();
      return liveWorker;
    })();
  }
  return appReady;
}

async function rebuildWorkerForSnippet(snippet) {
  if (liveWorker) {
    liveWorker.stop();
    liveWorker = null;
  }
  appReady = (async () => {
    const xdvParts = await runWarmup(snippet);
    liveWorker = new LiveWorker(xdvParts);
    await liveWorker.start();
    return liveWorker;
  })();
  return appReady;
}

function serveStatic(req, res, pathname) {
  const relative = pathname === '/' ? 'index.html' : pathname.slice(1);
  const full = path.resolve(publicRoot, relative);
  if (!full.startsWith(publicRoot) || !fs.existsSync(full) || fs.statSync(full).isDirectory()) {
    send(res, 404, 'Not found');
    return;
  }
  const ext = path.extname(full).toLowerCase();
  const types = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
  };
  send(res, 200, fs.readFileSync(full), types[ext] || 'application/octet-stream');
}

function serveRenderFile(res, pathname) {
  const relative = pathname.replace(/^\/renders\//, '');
  const full = path.resolve(rendersRoot, relative);
  if (!full.startsWith(rendersRoot) || !fs.existsSync(full) || fs.statSync(full).isDirectory()) {
    send(res, 404, 'Not found');
    return;
  }
  send(res, 200, fs.readFileSync(full), 'application/pdf');
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);
    if (req.method === 'POST' && url.pathname === '/api/render') {
      const body = JSON.parse(await readBody(req));
      const snippet = String(body.snippet || '').trim();
      if (!snippet) {
        sendJson(res, 400, { error: 'Snippet is empty.' });
        return;
      }
      const worker = await ensureWorker();
      try {
        sendJson(res, 200, await worker.render(snippet, safeWidth(body.widthPt)));
      } catch (error) {
        if (!String(error.message).includes("hasn't been defined")) {
          throw error;
        }
        const refreshedWorker = await rebuildWorkerForSnippet(snippet);
        sendJson(res, 200, await refreshedWorker.render(snippet, safeWidth(body.widthPt)));
      }
      return;
    }
    if (req.method === 'GET' && url.pathname.startsWith('/renders/')) {
      serveRenderFile(res, url.pathname);
      return;
    }
    if (req.method === 'GET') {
      serveStatic(req, res, url.pathname);
      return;
    }
    send(res, 405, 'Method not allowed');
  } catch (error) {
    sendJson(res, 500, { error: error.message });
  }
});

process.on('exit', () => {
  if (liveWorker) {
    liveWorker.stop();
  }
});

process.on('SIGINT', () => process.exit(0));
process.on('SIGTERM', () => process.exit(0));

server.listen(port, () => {
  console.log(`XeTeX snippet app: http://localhost:${port}`);
  console.log(`Runtime: ${runtimeRoot}`);
  ensureWorker()
    .then(() => console.log('Live XeTeX worker is warm.'))
    .catch((error) => console.error(error));
});
