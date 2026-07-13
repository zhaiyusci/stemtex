# Generated-C XeTeX Daemon Bundle

This directory contains the generated-C XeTeX and ordinary C `xdvipdfmx`
sources used by StemTeX.  It avoids repeating TeX Live's WEB/CWEB conversion
step during normal builds.

The top-level StemTeX application and installer build is CMake-driven and
consumes the binaries produced under `out/standalone-msvc/`.  Maintainers can
rebuild those binaries with the MSVC static-dependency route:

```sh
./build-standalone-msvc.sh
```

The dvisvgm-based SVG daemon is rebuilt separately with PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-dvisvgmdaemon-msvc.ps1
```

They produce TeX Live style Windows wrappers backed by DLL entrypoints:

```text
out/standalone-msvc/
  xetexdaemon.dll
  xetexdaemon.exe
  dvipdfmxdaemon.dll
  xdvipdfmxdaemon.exe
  dvisvgmdaemon.dll
  dvisvgmdaemon.exe
```

`dvipdfmxdaemon.dll` is used as a hot converter by the C++ renderer. Fatal
xdvipdfmx errors are trapped inside the daemon entrypoint instead of terminating
the host process. Some known bad-DVI conditions are treated as recoverable: for
example, selecting an undefined font records a missing-font issue, omits
affected output, and lets PDF generation continue. In that case
`dvipdfmxdaemon_convert` returns `2` (`RECOVERABLE_ERROR`) after writing the PDF.
The DLL also exports `dvipdfmxdaemon_last_issue_flags` and
`dvipdfmxdaemon_last_issue_message` so the renderer can report the condition to
host applications.

`dvisvgmdaemon.dll` provides the same process-warm pattern for XDV-to-SVG
conversion. It initializes kpathsea/font maps once and resets document-local
dvisvgm state before each request. It exports:

```text
dvisvgmdaemon_init
dvisvgmdaemon_convert
dvisvgmdaemon_shutdown
dvisvgmdaemon_last_error_message
```

Install those binaries into the StemTeX side tree with:

```sh
./install-msvc-standalone-to-side-tree.sh
```

The default destination is:

```text
../dist/stemtex-texlive-daemon-static
```

## Source Layout

```text
src/web2c/                  Generated XeTeX C/C++ and support sources.
src/dvipdfm-x/              xdvipdfmx C sources.
src/dvisvgm/                dvisvgm source subset and MSVC daemon shim.
src/libpaper/               Small libpaper source used by xdvipdfmx.
src/libs/                   TeX Live library source snapshots needed here.
src/texk/                   TeX Live texk source snapshots needed here.
src/windows_mingw_wrapper/  Small calldll wrapper source.

prebuilt-msvc/              Static MSVC libs and headers required to build.
third_party-msvc-src/       Third-party source snapshots for rebuilding libs.
```

`prebuilt-msvc/` intentionally contains no `.exe`, `.dll`, `.obj`, `.pdb`, or
other build products.  It is a build input directory: headers, static import
libraries, and ICU data only.

`third_party-msvc-src/` stores source snapshots for zlib, libpng, expat,
graphite2, freetype, ICU, harfbuzz, and fontconfig.  Build outputs from those
trees should not be committed.

## Rebuilding Dependencies

Most day-to-day work should not need this.  To rebuild third-party static
libraries with MSVC:

```sh
./build-thirdparty-msvc.sh
./build-texlive-libs-msvc.sh
```

Then rebuild the daemon bundle:

```sh
./build-standalone-msvc.sh
```

Rebuild the SVG daemon with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-dvisvgmdaemon-msvc.ps1
```

## Daemon Runtime Switches

The generated-C source under `src/web2c` carries the StemTeX daemon switches:

```text
--flush-output-on-shipout
--no-font-cache-refresh
```

The corresponding patch record is:

```text
../patches/texlive-generated-daemon-runtime-switches.patch
```
