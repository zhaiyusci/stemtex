# Windows XeTeX Build Notes

StemTeX currently builds its daemon engine from the generated-C source bundle in
`texlive-xetex/`.  The preferred Windows route is MSVC with static third-party
dependencies.

This is a lower-level maintainer document. For the normal CMake application,
GUI, staging, and installer flow, use
[Building and packaging](BUILDING_AND_PACKAGING.md).

## Current Engine Rebuild Route

The normal application and installer build is driven by the top-level CMake
project and does not require bash. The generated-C XeTeX/xdvipdfmx engine and
static-dependency rebuild scripts documented below currently do require an
MSYS2/Cygwin-style bash environment in addition to Visual Studio. They produce
inputs consumed later by the bash-free CMake application build.

Build the daemon bundle from the generated-C source snapshot, using the
installed Visual Studio toolchain:

```bash
cd /path/to/stemtex
./texlive-xetex/build-standalone-msvc.sh
```

Outputs:

```text
texlive-xetex/out/standalone-msvc/xetexdaemon.dll
texlive-xetex/out/standalone-msvc/xetexdaemon.exe
texlive-xetex/out/standalone-msvc/dvipdfmxdaemon.dll
texlive-xetex/out/standalone-msvc/xdvipdfmxdaemon.exe
```

Build the dvisvgm-based SVG daemon with PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\texlive-xetex\build-dvisvgmdaemon-msvc.ps1
```

Additional outputs:

```text
texlive-xetex/out/standalone-msvc/dvisvgmdaemon.dll
texlive-xetex/out/standalone-msvc/dvisvgmdaemon.exe
```

The `.exe` files are small `calldll` wrappers.  The actual engine/converter
code lives in the DLLs:

```text
xetexdaemon.exe     -> xetexdaemon.dll:dllxetexmain
xdvipdfmxdaemon.exe -> dvipdfmxdaemon.dll:dlldvipdfmxmain
dvisvgmdaemon.exe   -> dvisvgmdaemon.dll:dlldvisvgmmain
```

`dvipdfmxdaemon.dll` also exports a StemTeX hot-start API:

```text
dvipdfmxdaemon_init
dvipdfmxdaemon_convert
dvipdfmxdaemon_shutdown
```

The C++ renderer uses this API instead of repeatedly calling
`dlldvipdfmxmain`.  `init` performs the expensive kpathsea/config/fontmap setup
once; each `convert` handles one XDV-to-PDF request; `shutdown` closes the
cached fontmaps.  The old `dlldvipdfmxmain` export remains for the command-line
wrapper.

`dvisvgmdaemon.dll` exports a matching hot-start API for XDV-to-SVG conversion:

```text
dvisvgmdaemon_init
dvisvgmdaemon_convert
dvisvgmdaemon_shutdown
dvisvgmdaemon_last_error_message
```

The converter accepts a small command-line subset through
`dvisvgmdaemon_convert`: `--page`, `--bbox`, `--output`, `--exact-bbox`,
`--no-fonts`, and one XDV input path. The renderer passes
`--bbox=papersize` so SVG output uses the same `pdf:pagesize` page box that
xdvipdfmx uses for PDF output. It keeps kpathsea/font-map initialization warm
but resets dvisvgm document state, including FreeType's current font handle,
before each conversion.

Install the built binaries into the static StemTeX side tree:

```bash
./texlive-xetex/install-msvc-standalone-to-side-tree.sh
```

The maintained CMake install tree copies profile warmup sources and installs
`runtime/refresh-profile-cache.bat`. The installer runs that batch file for the
maintained `unicodemath_cjk` profile during installation. For a manual staged
tree, run the same installed helper from the runtime directory; it defaults to
the staged `..\gui\profiles\unicodemath_cjk` profile:

```bat
cd /d C:\path\to\StemTeX\runtime
refresh-profile-cache.bat
```

## Source Inputs

The build does not read a full TeX Live checkout during normal operation.  It
uses:

```text
texlive-xetex/src/web2c
texlive-xetex/src/dvipdfm-x
texlive-xetex/src/dvisvgm
texlive-xetex/src/libpaper
texlive-xetex/src/libs
texlive-xetex/src/texk
texlive-xetex/src/windows_mingw_wrapper
```

`src/web2c` contains generated XeTeX C/C++ files, so the build skips the
WEB/CWEB literate-programming conversion step.

The checked-in dependency bundle is:

```text
texlive-xetex/prebuilt-msvc
```

It contains static libraries, headers, and ICU data only.  It should not contain
`.exe`, `.dll`, `.obj`, `.pdb`, or other build outputs.

## Rebuilding Static Dependencies

The source snapshots for third-party libraries live under:

```text
texlive-xetex/third_party-msvc-src
```

Rebuild them with:

```bash
./texlive-xetex/build-thirdparty-msvc.sh
./texlive-xetex/build-texlive-libs-msvc.sh
```

This regenerates the static dependency inputs used by
`build-standalone-msvc.sh`.  Build products inside `third_party-msvc-src/` are
not source and should not be committed.

The dvisvgm daemon uses the same checked-in static dependency inputs and can be
rebuilt after that with `build-dvisvgmdaemon-msvc.ps1`.

## Runtime Switches

The generated-C source carries StemTeX's daemon switches:

```text
--flush-output-on-shipout
--no-font-cache-refresh
```

Patch record:

```text
patches/texlive-generated-daemon-runtime-switches.patch
```

`--flush-output-on-shipout` makes live `-no-pdf` XeTeX output usable after each
page by flushing pending XDV bytes at `\shipout`.

`--no-font-cache-refresh` makes normal rendering rely on installation-time
fontconfig cache generation instead of refreshing cache during interactive
requests.

`src/dvipdfm-x` also carries a small hot-start fix in `dvi_close()`: after
freeing `def_fonts` and `loaded_fonts`, it resets both the count and capacity
fields.  Without this, the second `dvipdfmxdaemon_convert` can reuse stale
capacity values and crash in `dvi_init()`.

## StemTeX Runtime Tree

The CMake-installed runtime uses this shape:

```text
runtime/
  bin/windows/
    stemtex-worker-host.exe
    xetexdaemon.exe
    xetexdaemon.dll
    xdvipdfmxdaemon.exe
    dvipdfmxdaemon.dll
    dvisvgmdaemon.exe
    dvisvgmdaemon.dll
  gui/profiles/
  texmf-dist/
  texmf-var/
```

The shipped runtime intentionally does not include stock `xetex.exe`,
`xelatex.exe`, or `xetex.dll`.

The daemon format is named:

```text
texmf-var/web2c/xetex/xelatexdaemon.fmt
```

Preambles and matching warmup files live in profile directories:

```text
gui/profiles/<name>/preamble.tex
gui/profiles/<name>/warmup.tex
```

The renderer requires the host or GUI to pass a profile directory explicitly.
The preamble loads `preview` with `active,tightpage`; the live worker wraps each
request in a `preview` environment so the resulting PDF page is already cropped
to the snippet content.
