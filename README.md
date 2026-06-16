# xetex-live-worker

Experimental Windows XeLaTeX runtime and worker prototype for low-latency
snippet rendering.

The runtime produced by this repository is called **StemTeX**: a small
Windows-focused XeLaTeX service runtime for interactive STEM snippets.
The current StemTeX small-tree version is `0.1.0`.

The core idea is:

- build a small XeLaTeX runtime tree under `stemtex/`;
- treat fontconfig cache as runtime data;
- keep one XeTeX process hot with a fixed preamble;
- patch XeTeX so `-no-pdf` output is flushed after each `\shipout`;
- finalize the cumulative live XDV and convert only the newest page with
  `xdvipdfmx -s N-N`.

This repository intentionally does not vendor a full TeX Live source tree or a
full runtime tree. The scripts expect either an installed TeX Live or local
upstream source trees when rebuilding the engine.

## What We Changed In XeTeX

This project does not simply enable Web2C's old `-ipc` option.

Instead, it adds a new explicit XeTeX switch:

```text
--flush-output-on-shipout
```

When XeTeX runs with `-no-pdf --flush-output-on-shipout`, every `\shipout`
writes pending XDV bytes from XeTeX's in-memory DVI/XDV buffer to the `.xdv`
file and flushes the file handle. The XeTeX process keeps running.

It deliberately does not:

- start TeXView;
- open an IPC socket;
- send `ipcpage` messages;
- use the legacy `-ipc` or `-ipc-start` protocol.

The implementation reuses the old IPC buffer-flushing idea, but not the old IPC
transport. Internally the command-line option sets the existing `ipcon` flag to
`3`; the shipout path checks that value and performs the write/flush block
without calling `ipcpage()`.

That matters because stock XeTeX normally leaves a live worker's XDV unusable
until the job exits and writes the final postamble. With this patch, the worker
can copy the currently flushed live XDV, synthesize the missing postamble with
warmup font definitions, and ask `xdvipdfmx -s N-N` for only the newest page.

## IPC Archeology

Web2C still contains old IPC support for TeX. It was originally written by Tom
Rokicki for NeXT TeXView and adapted to Web2C by Shamim Mohamed. The source
comment describes the purpose: ship DVI output through a pipe/socket so a
previewer can display it incrementally.

In TeX Live 2026, classic `tex.exe --help` still shows:

```text
-ipc
-ipc-start
```

but `xetex.exe --help` does not. So the situation is not that IPC was fully
removed from Web2C; it is that the feature is historical, lightly documented,
and not part of the normal XeTeX/XDV workflow.

The old IPC path was aimed at DVI previewing, not at XeTeX's XDV-to-PDF
pipeline. It also carries platform-specific socket code and a TeXView-oriented
protocol. For this project, the useful piece was the page-boundary buffer
flush, not the transport protocol.

## Layout

```text
scripts/
  build-stemtex-runtime.ps1       Build the StemTeX runtime tree.
  build-windows-native.ps1        Build patched W32TeX-style xetex.dll.
worker-prototype/
  run-single-worker-live-pdf.js   Current live-PDF worker controller.
  run-worker-*.js                 Earlier worker and pool experiments.
  requests/                       UTF-8 snippet inputs.
test/
  preamble.tex
  test_1.tex ... test_5.tex
patches/
  *.patch                         Engine/fontconfig changes.
docs/
  Profiling and build notes from the prototype.
```

Generated directories are ignored:

```text
stemtex/
out/
ptx/
ktx/
texlive-source/
```

## Build A Small Runtime

From PowerShell:

```powershell
.\scripts\build-stemtex-runtime.ps1 -TeXLiveRoot C:\texlive\2026 -Destination .\stemtex -Clean
```

StemTeX always uses the patched daemon engine. Place the expected W32TeX-style
source/build trees at the repository root and build the engine first:

```powershell
.\scripts\build-windows-native.ps1 -Target All -Arch x64
.\scripts\build-stemtex-runtime.ps1 -TeXLiveRoot C:\texlive\2026 -Destination .\stemtex -Clean
```

The default preamble is aimed at short STEM snippets: `unicode-math`, `xeCJK`,
`mathtools`, `mhchem`, `physics`, `xcolor`, and `cancel`, using Windows
text/CJK fonts plus XITS Math.  The runtime tree also carries `siunitx` for
optional preamble variants, but it is not loaded by default because it conflicts
with `physics` over `\qty`.

The runtime build warms fontconfig cache with `test\test_5.tex` by default.
Refresh manually after adding packages/fonts:

```powershell
.\stemtex\refresh-font-cache.ps1 -Clean
```

## Run The Live Worker

The live worker requires a XeTeX build that supports:

```text
--flush-output-on-shipout
```

Then run:

```powershell
node .\worker-prototype\run-single-worker-live-pdf.js
```

Output goes to:

```text
out\single-worker-live-pdf
```

Each request emits a single-page PDF by default. Add `--cumulative` to emit a
cumulative debug PDF containing pages `1..N`.

The request protocol is intentionally narrow: stdin carries UTF-8-safe request
file paths, and each request file is treated as a small body snippet under the
fixed preamble. The worker resets visible counters and visual state for each
snippet (`page`, `equation`, `footnote`, normal font/size/color), but it is not
a general LaTeX sandbox. Warmup workers stop by receiving a literal
`\workerstop` line; live workers are killed by the controller after the requested
PDFs have been emitted, because the live path does not need a final full XDV
postamble.

## Run The Web Preview

The web preview wraps the same worker path behind a local HTTP server. It lets
you edit one snippet, adjust the text block width in points, render, and preview
the generated PDF on the right. The width control is clamped to `180-430pt` in
both the browser and the server.

The web preview uses the same latest-page path: it finalizes the cumulative live
XDV and calls `xdvipdfmx -s N-N` for the newest page.

```powershell
node .\webapp\server.js
```

If the runtime is not under `.\runtime`, point the server at it:

```powershell
$env:XETEX_RUNTIME="..\stemtex"
node .\webapp\server.js
```

Then open:

```text
http://localhost:5177
```

## Current Status

This is a research prototype. The fastest working path is the patched single
worker. In local profiling, the earlier whole-XDV latest-page path was roughly
50-170 ms after the worker was warm, depending on snippet content and
`xdvipdfmx` time. The abandoned independent-XDV experiment was removed from the
runtime path because standalone deltas must reconstruct too much XDV driver
state to be worth it here.

See:

```text
docs\XELATEX_PROFILING_NOTES.md
docs\WINDOWS_XETEX_BUILD_NOTES.md
```

