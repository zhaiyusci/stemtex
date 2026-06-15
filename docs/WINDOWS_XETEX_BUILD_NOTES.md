# Windows XeTeX Build Notes

This workspace has two related but separate pieces:

- a native Windows build of XeTeX from source, producing `xetex.dll`;
- a small XeLaTeX runtime tree, enough to run the current test documents.

The source build lives under `ptx`.  The small runtime tree lives under
`runtime`.

## Native XeTeX Build

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

## Small Runtime Tree

The small runtime tree is:

```text
runtime\
  run-xelatex.bat
  bin\windows\
  texmf-dist\
  texmf-var\
```

`bin\windows` contains the engine/launcher/runtime DLL layer:

```text
xetex.exe
xelatex.exe
xetex.dll
xdvipdfmx.exe
dvipdfmx.dll
kpsewhich.exe
kpathsealibw64.dll
icudt*.dll or icu-data\icudt76l.dat
VC runtime DLLs
```

`texmf-dist` contains the reduced TeX tree:

```text
web2c\texmf.cnf
web2c\fmtutil.cnf
tex\latex\...
tex\xelatex\xecjk\...
fonts\opentype\public\fandol\...
fonts\opentype\public\lm\...
fonts\opentype\public\lm-math\...
dvipdfmx\dvipdfmx.cfg
```

`texmf-var` contains generated/runtime state:

```text
web2c\xetex\xelatex.fmt
fonts\conf\fonts.conf
fonts\cache\...
```

The existing `runtime` tree is assembled as a runtime, not as source.
It can run XeLaTeX documents, but its original `xetex.dll` came from TeX Live.
To test the self-built DLL, either replace the DLL in the tree or put
`ptx\texk\web2c` first in `PATH`.

## Rebuilding The Small Runtime Tree

A script was added for rebuilding a small runtime tree:

```powershell
.\scripts\build-mini-texlive-xetex.ps1 -Destination .\runtime -UseSelfBuiltXeTeX -Clean
```

Parameters:

```text
-TeXLiveRoot        full TeX Live root, for example C:\texlive\2026
-Destination        output runtime tree
-SourceRoot         this workspace root
-UseSelfBuiltXeTeX  copy ptx-built xetex.dll and matching launchers
-Clean              delete destination before rebuilding
```

Without `-TeXLiveRoot`, the script tries `$env:TEXLIVE_ROOT`, then
`kpsewhich -var-value=TEXMFROOT`.

With `-UseSelfBuiltXeTeX`, the script copies:

```text
ptx\texk\web2c\xetex.dll
ktx\texk\calldll\xetex.exe
ktx\texk\calldll\xelatex.exe
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

The fix is to dump a new format using the same `xetex.dll` that will run it.
The small-tree build script does this automatically.

The manual equivalent is:

```powershell
xelatex.exe -ini -etex -jobname=xelatex xelatex.ini
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

The small runtime script is intentionally conservative: it copies a selected
set of packages and fonts known to cover the current tests.  If a new document
uses more packages or fonts, add the corresponding directories/files to
`scripts\build-mini-texlive-xetex.ps1`.

The mini-tree build script now prepends a runtime override block to
`texmf-dist\web2c\texmf.cnf`.  This removes the full TeX Live `!!` database-only
assumptions and makes package lookup relative to the small tree itself.  The
script was verified by rebuilding the runtime tree and running `test_4.tex`
through that tree's own `run-xelatex.bat`.

The script also warms the fontconfig cache during the build.  By default it
uses:

```text
test\test_5.tex
```

as the warmup document, because that document exercises the intended fixed
preamble, including `mhchem`, `physics`, and `xcolor`.  The warmup run does not
use `--no-font-cache-refresh`, so cache files are written under:

```text
texmf-var\fonts\cache
```

The generated `run-xelatex.bat` still uses `--no-font-cache-refresh` for normal
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
`run-xelatex.bat`, which keeps cache refresh disabled.

Do not confuse the three layers:

```text
xetex.dll       real engine
xetex.exe       launcher
xelatex.fmt     preloaded LaTeX format tied to the engine build
```

When changing XeTeX source, rebuild `xetex.dll` and redump `xelatex.fmt`.
