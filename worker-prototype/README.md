# XeLaTeX Worker Prototype

The worker loads a fixed preamble once, then reads request file names from
stdin. Request bodies are stored as UTF-8 `.tex` files; stdin carries only
ASCII paths to avoid Windows console and pipe encoding damage.

Run commands from the repository root after building `runtime/`:

```powershell
node .\worker-prototype\run-single-worker-live-pdf.js
```

The live PDF path requires a patched XeTeX with:

```text
--flush-output-on-shipout
```

The controller starts XeTeX with `-no-pdf`, copies the currently flushed
cumulative live XDV after each `\shipout`, appends a temporary postamble with
warmup font definitions, and calls `xdvipdfmx -s N-N` to return only the newest
page.

The worker no longer has a hard-coded request count. Warmup sends `\workerstop`
after the selected request files so XeTeX can write a complete XDV postamble for
fontdef extraction. The live worker is terminated by the controller after all
requested PDFs are produced.

Default output:

```text
out\single-worker-live-pdf
```

Useful options:

```powershell
node .\worker-prototype\run-single-worker-live-pdf.js --runtime .\runtime --out .\out\live
node .\worker-prototype\run-single-worker-live-pdf.js --cumulative
```

`--cumulative` is a debug mode that emits pages `1..N`. The default mode is
`latest-page`.

## Older Experiments

The stock-worker probe measures hot in-process typesetting but cannot produce a
usable live PDF until XeTeX exits:

```powershell
node .\worker-prototype\run-worker-file-request.js
```

The one-shot worker pool keeps multiple XeTeX processes warm. Each worker
serves one request and exits, then the controller starts a replacement:

```powershell
node .\worker-prototype\run-worker-pool.js
node .\worker-prototype\run-worker-pool.js --spacing-ms 1500
```

Measure the idle pressure of a prewarmed pool:

```powershell
node .\worker-prototype\measure-worker-pool-pressure.js
```

Current local measurements from the original prototype:

```text
pool=5 idle: 920.6 MB Working Set, 1419.8 MB Private Memory, ~0% CPU
pool=1 idle: 184.3 MB Working Set, 284.1 MB Private Memory, ~0% CPU
```

The single live worker is usually the better latency/memory tradeoff.
