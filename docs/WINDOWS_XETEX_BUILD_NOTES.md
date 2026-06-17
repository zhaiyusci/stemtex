# Windows XeTeX Build Notes

This workspace has two related but separate pieces:

- a native Windows build of XeTeX from source, producing `xetex.dll`;
- a small XeLaTeX runtime tree, enough to run the current test documents.
- a standalone upstream TeX Live source bundle under `texlive-xetex`, built from
  generated/ordinary C and C++ sources without repeating WEB/CWEB conversion.

The source build lives under `ptx`.  The small runtime tree lives under
`runtime`.

## Native XeTeX Build

### TeX Live upstream standalone experiment

The TeX Live upstream source experiment builds only the XeTeX executable target
with MSYS2 UCRT64/MinGW:

```sh
JOBS=16 scripts/build-texlive-xetex-ucrt64.sh
```

Defaults:

```text
TL_SRC     ../texlive-source
BUILD_DIR  ../tlbuild-xetex-ucrt64-mingw
JOBS       16
CLEAN      0
USE_GENERATED 1
USE_PREBUILT_LIBS 1
```

The top-level standalone bundle builds directly from the copied/generated
sources and outputs TeX Live Windows style DLL-backed wrappers for both the
engine and converter:

```sh
cd texlive-xetex
JOBS=16 ./build-standalone-ucrt64.sh
```

```text
texlive-xetex\out\standalone-ucrt64\xetex.dll
texlive-xetex\out\standalone-ucrt64\xetex.exe
texlive-xetex\out\standalone-ucrt64\dvipdfmxdaemon.dll
texlive-xetex\out\standalone-ucrt64\xdvipdfmxdaemon.exe
```

The `xdvipdfmx.exe` path uses ordinary `texk/dvipdfm-x` C sources plus the
small `libpaper` C sources. It does not involve WEB/CWEB conversion.

The wrappers follow the same import pattern as `C:\texlive\2026\bin\windows`:

```text
xetex.exe     imports xetex.dll:dllxetexmain
xdvipdfmxdaemon.exe imports dvipdfmxdaemon.dll:dlldvipdfmxmain
```

Use a clean build directory when needed:

```sh
CLEAN=1 JOBS=16 scripts/build-texlive-xetex-ucrt64.sh
```

The script seeds generated web2c outputs from:

```text
texlive-xetex\web2c
```

This skips the `tie`/`otangle`/`web2c convert` literate-programming step during
normal builds. To force regeneration from the original WEB/CWEB files:

```sh
USE_GENERATED=0 JOBS=16 scripts/build-texlive-xetex-ucrt64.sh
```

The script also seeds UCRT64 static libraries and generated dependency headers
from:

```text
texlive-xetex\prebuilt-ucrt64
```

This avoids rebuilding `kpathsea`, `ptexenc`, `teckit`, `pplib`, and the small
web2c support libraries in ordinary rebuilds. To force rebuilding them:

```sh
USE_PREBUILT_LIBS=0 JOBS=16 scripts/build-texlive-xetex-ucrt64.sh
```

The script configures TeX Live with:

```text
--build=x86_64-w64-mingw32
--host=x86_64-w64-mingw32
--disable-all-pkgs
--enable-web2c
--enable-xetex
--disable-xetex-synctex
```

It uses system MSYS2 UCRT libraries for zlib, libpng, freetype2, ICU, graphite2,
and HarfBuzz. It builds only the internal pieces XeTeX still needs from the TeX
Live tree:

```text
texk/kpathsea
texk/ptexenc
libs/teckit
libs/pplib
texk/web2c target xetex.exe
```

The output is:

```text
..\tlbuild-xetex-ucrt64-mingw\texk\web2c\xetex.exe
```

Two Windows-native build details matter:

- build the real target `xetex.exe`, not bare `xetex`, because GNU make can
  otherwise choose its built-in Pascal rule;
- the generated `texk/web2c/Makefile` contains Unix path lists such as
  `WEBINPUTS=.:$(srcdir)`, but the generated tools are native Windows
  executables. The script patches the build-directory Makefile to quote
  semicolon-separated `WEBINPUTS` values.

The upstream web2c bootstrap rules may try to update these source files:

```text
texk/web2c/tangleboot.pin
texk/web2c/ctangleboot.cin
texk/web2c/cwebboot.cin
```

The script backs them up before the build and restores them afterward if the
build touched them.

### W32TeX xetexdaemon.dll build

The native build script is:

```powershell
.\scripts\build-windows-native.ps1 -Target All -Arch x64
```

Important targets:

```powershell
.\scripts\build-windows-native.ps1 -Target XeTeX -Arch x64
.\scripts\build-windows-native.ps1 -Target Launchers -Arch x64
```

`XeTeX` builds the real engine DLL:

```text
ptx\texk\web2c\xetex.dll
```

`Launchers` builds the small DLL-loading launchers:

```text
ktx\texk\calldll\xetex.exe
ktx\texk\calldll\xelatex.exe
```

The launchers are not the engine.  They call `dllxetexmain` from whichever
`xetex.dll` is found by Windows DLL search rules.  For testing the self-built
engine, put this directory before any TeX Live binary directory in `PATH`:

```text
ptx\texk\web2c
```

The build script discovers Visual Studio through `vswhere` or installed
Visual Studio paths.  It also bootstraps GNU make into `.build-tools` when
needed and creates a local no-space junction for Git for Windows `usr\bin`,
because several W32TeX makefiles use `sh.exe` and GNU make handles paths with
spaces poorly in this tree.

## Source Changes Made

An explicit XeTeX command-line switch was added:

```text
--no-font-cache-refresh
```

It sets:

```text
FONTCONFIG_NO_CACHE_REFRESH=1
```

Relevant files:

```text
ptx\texk\web2c\lib\texmfmp.c
ptx\texk\web2c\texmfmp-help.h
ptx\libs\fontconfig\src\fccache.c
ptx\libs\fontconfig\src\fcdir.c
ptx\libs\fontconfig\src\fcint.h
```

The fontconfig patch makes cache writes and fallback rescans opt out when
`FONTCONFIG_NO_CACHE_REFRESH` is true.  Existing caches can still be read.

A placeholder make include was also added:

```text
ptx\texk\make\paths.mk
```

This satisfies a dependency in the local Windows web2c makefiles.  The XeTeX
build path here does not require variables from that file.

## StemTeX Runtime Tree

The current trimmed runtime is called StemTeX:

```text
stemtex\
  run-xelatexdaemon.bat
  refresh-font-cache.ps1
  bin\windows\
  texmf-dist\
  texmf-var\
```

`bin\windows` contains the engine/launcher/runtime DLL layer:

```text
xetexdaemon.exe
xetexdaemon.dll
xelatexdaemon.bat
xdvipdfmxdaemon.exe
dvipdfmxdaemon.dll
kpsewhich.exe
kpathsealibw64.dll
icudt76.dll
icu-data\icudt76l.dat
VC runtime DLLs
```

`texmf-dist` contains the reduced TeX tree:

```text
web2c\texmf.cnf
web2c\fmtutil.cnf
tex\latex\...
tex\xelatex\xecjk\...
fonts\opentype\public\xits\...
fonts\opentype\public\lm\...      minimal text-font compatibility files
fonts\tfm\public\cm\...
dvipdfmx\dvipdfmx.cfg
```

`texmf-var` contains generated/runtime state:

```text
web2c\xetex\xelatex.fmt
fonts\conf\fonts.conf
fonts\cache\...
```

The StemTeX tree is assembled as a runtime, not as source.  It always uses the
patched daemon engine: `xetexdaemon.exe` loads `xetexdaemon.dll`.  The shipped
runtime intentionally does not include `xetex.exe`, `xelatex.exe`, or
`xetex.dll`.

## Rebuilding The StemTeX Runtime Tree

Build the patched engine first, then assemble StemTeX:

```powershell
.\scripts\build-windows-native.ps1 -Target All -Arch x64
.\scripts\build-stemtex-runtime.ps1 -Destination .\stemtex -Clean
```

Parameters:

```text
-TeXLiveRoot        full TeX Live root, for example C:\texlive\2026
-Destination        output StemTeX tree
-SourceRoot         this workspace root
-CacheWarmupTex     warmup document, defaulting to test\test_5.tex
-SkipFontCacheWarmup
-Clean              delete destination before rebuilding
```

Without `-TeXLiveRoot`, the script tries `$env:TEXLIVE_ROOT`, then
`kpsewhich -var-value=TEXMFROOT`.

The script copies:

```text
ptx\texk\web2c\xetex.dll              -> bin\windows\xetexdaemon.dll
ktx\texk\calldll\xetexdaemon.exe      -> bin\windows\xetexdaemon.exe
ptx\libs\icu-src\source\data\in\icudt76l.dat
ptx\libs\icu-src\bin64\icudt76.dll
```

The full ICU data file is important.  The stub `icudt76.dll` alone is not
enough: XeTeX then fails during startup of ICU converters with:

```text
internal error; cannot read font names
```

The wording is misleading.  In this case, fontconfig was not the failing
piece; ICU could not open converters such as `macintosh`, `UTF16BE`, and
`UTF8`.

## Format Files

`xelatex.fmt` is tied to the exact XeTeX executable/string pool.  After changing
or rebuilding XeTeX, an old format can fail with:

```text
Fatal format file error; I'm stymied
made by different executable version, strings are different
```

The fix is to dump a new format using the same daemon engine that will run it.
The StemTeX build script does this automatically.

The manual equivalent is:

```powershell
xetexdaemon.exe -ini -etex -jobname=xelatex xelatex.ini
```

Make sure `TEXFORMATS` points to the directory where the new
`xelatex.fmt` should be written.

## Fontconfig Runtime

For this W32TeX/fontconfig tree, these variables matter:

```text
XE_FONTCONFIG_PATH
FONTCONFIG_PATH
XE_FC_CACHEDIR
FC_CACHEDIR
```

The runtime batch file sets them to the small tree:

```text
texmf-var\fonts\conf
texmf-var\fonts\cache
```

`--no-font-cache-refresh` makes the self-built XeTeX use existing fontconfig
caches only.  It does not change the document output by itself.  It is useful
when treating font caches as part of the runtime image.

## Testing

The self-built DLL was tested against:

```text
test\test_1.tex
test\test_2.tex
test\test_3.tex
test\test_4.tex
```

The successful test setup used:

- `ptx\texk\web2c\xetex.dll`;
- a freshly dumped `xelatex.fmt`;
- full ICU data from `ptx\libs\icu-src\source\data\in\icudt76l.dat`;
- fontconfig path/cache variables set;
- `--no-font-cache-refresh`;
- `xdvipdfmx` for the final XDV-to-PDF step.

The last run of `test_4.tex` succeeded after the document was changed to avoid
glyphs missing from Fandol:

```text
XeTeXExit  = 0
DriverExit = 0
PDF        = yes
Missing character lines: none
```

Output was written under:

```text
selfbuild-test\
```

## Current Caveats

The StemTeX runtime script is intentionally conservative: it copies a selected
set of packages and fonts known to cover the current tests.  If a new document
uses more packages or fonts, add the corresponding directories/files to
`scripts\build-stemtex-runtime.ps1`.

The StemTeX build script prepends a runtime override block to
`texmf-dist\web2c\texmf.cnf`.  This removes the full TeX Live `!!` database-only
assumptions and makes package lookup relative to the small tree itself.  The
script was verified by rebuilding the runtime tree and running documents
through that tree's own `run-xelatexdaemon.bat`.

The script also warms the fontconfig cache during the build.  By default it
uses:

```text
test\test_5.tex
```

as the warmup document, because that document exercises the intended fixed
preamble, including `mathtools`, `mhchem`, `physics`, `xcolor`, and `cancel`.
The warmup run does not
use `--no-font-cache-refresh`, so cache files are written under:

```text
texmf-var\fonts\cache
```

The generated `run-xelatexdaemon.bat` uses `--no-font-cache-refresh` for normal
runtime execution.

## Manual Font Cache Refresh

The small runtime tree now includes a manual refresh entry point:

```text
refresh-font-cache.bat
refresh-font-cache.ps1
cache-warmup\warmup.tex
```

The default use is:

```cmd
refresh-font-cache.bat
```

It runs XeLaTeX without `--no-font-cache-refresh`, writes fontconfig caches under:

```text
texmf-var\fonts\cache
```

and writes temporary warmup output under:

```text
texmf-var\cache-warmup
```

The script can also take a custom warmup document:

```powershell
.\refresh-font-cache.ps1 -WarmupTex C:\path\to\warmup.tex -Clean
```

Use this after adding fonts, changing fontconfig configuration, or extending the
default package/font set in a way that should be exercised before normal
runtime calls.  Normal document compilation should continue to use
`run-xelatexdaemon.bat`, which keeps cache refresh disabled.

Do not confuse the three layers:

```text
xetexdaemon.dll   real engine, copied from the patched xetex.dll build output
xetexdaemon.exe   launcher that loads the same-basename DLL
xelatex.fmt       preloaded LaTeX format tied to the engine build
```

When changing XeTeX source, rebuild `xetex.dll` and redump `xelatex.fmt`.
