# xetex-live-worker

Experimental Windows XeLaTeX runtime and worker prototype for low-latency
snippet rendering.

The core idea is:

- build or provide a small XeLaTeX runtime tree under `runtime/`;
- treat fontconfig cache as runtime data;
- keep one XeTeX process hot with a fixed preamble;
- patch XeTeX so `-no-pdf` output is flushed after each `\shipout`;
- convert only the newest XDV page with `xdvipdfmx -s N-N`.

This repository intentionally does not vendor a full TeX Live source tree or a
full runtime tree. The scripts expect either an installed TeX Live or local
upstream source trees when rebuilding the engine.

## Layout

```text
scripts/
  build-mini-texlive-xetex.ps1    Build the small runtime tree.
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
runtime/
out/
ptx/
ktx/
texlive-source/
```

## Build A Small Runtime

From PowerShell:

```powershell
.\scripts\build-mini-texlive-xetex.ps1 -TeXLiveRoot C:\texlive\2026 -Destination .\runtime -Clean
```

To use a locally built patched W32TeX-style engine, place the expected source
trees at the repository root and build first:

```powershell
.\scripts\build-windows-native.ps1 -Target All -Arch x64
.\scripts\build-mini-texlive-xetex.ps1 -TeXLiveRoot C:\texlive\2026 -Destination .\runtime -UseSelfBuiltXeTeX -Clean
```

The runtime build warms fontconfig cache with `test\test_5.tex` by default.
Refresh manually after adding packages/fonts:

```powershell
.\runtime\refresh-font-cache.ps1 -Clean
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

Each request emits a latest-page PDF by default. Add `--cumulative` to emit
pages `1..N`.

## Current Status

This is a research prototype. The fastest working path is the patched single
worker. In local profiling, the request-to-latest-page-PDF path was roughly
50-170 ms after the worker was warm, depending on snippet content and
`xdvipdfmx` time.

See:

```text
docs\XELATEX_PROFILING_NOTES.md
docs\WINDOWS_XETEX_BUILD_NOTES.md
```
