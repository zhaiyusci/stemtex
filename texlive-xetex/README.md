# Generated TeX Live XeTeX Sources

This directory stores generated web2c outputs for the TeX Live upstream XeTeX
experiment, so the normal build does not need to repeat the WEB/CWEB literate
programming conversion step.

It also contains enough ordinary C/C++ source and prebuilt UCRT64 static
libraries to build TeX Live style DLL-backed `xetex.exe` and `xdvipdfmx.exe`
wrappers without the TeX Live WEB/CWEB sources.

Current source:

```text
../texlive-source
TeX Live 2027/dev, commit a1f0eea56a708007b32b8fecda2a79a6efe94084
```

Generated files:

```text
web2c/xetexini.c
web2c/xetex0.c
web2c/xetexcoerce.h
web2c/xetexd.h
web2c/xetex-pool.c
```

Standalone C/C++ build:

```sh
JOBS=16 ./build-standalone-ucrt64.sh
```

Output:

```text
out/standalone-ucrt64/xetex.dll
out/standalone-ucrt64/xetex.exe
out/standalone-ucrt64/dvipdfmx.dll
out/standalone-ucrt64/xdvipdfmx.exe
```

The default UCRT64 build keeps MSYS2 third-party libraries dynamic.  For a
smaller distributable tree, build the same DLL-backed layout with static
third-party linkage:

```sh
LINK_MODE=static OUT_DIR=out/standalone-ucrt64-static JOBS=16 ./build-standalone-ucrt64.sh
```

This still produces independent TeX Live style DLLs:

```text
xetexdaemon.exe -> xetexdaemon.dll:dllxetexmain
xdvipdfmx.exe   -> dvipdfmx.dll:dlldvipdfmxmain
```

but `xetexdaemon.dll` and `dvipdfmx.dll` absorb most MSYS2 runtime
dependencies such as fontconfig, freetype, harfbuzz, graphite2, ICU, libpng,
zlib, libstdc++, and libgcc.  The current static UCRT64 route still imports
the Windows UCRT API-set DLLs.  Set `STRIP_OUTPUT=0` when a debug-symbol build
is needed; release builds strip binaries by default.

The next size-reduction route is an MSVC build with `/MT` plus static
third-party libraries.  The intended shape is the same as above: keep
`xetexdaemon.dll` and `dvipdfmx.dll` as the public binary components, but avoid
shipping a loose pile of compiler/runtime DLLs.  MSVC alone only removes the
MinGW runtime layer; third-party libraries still need static MSVC builds to
remove their DLLs.

This standalone route does not read `../texlive-source`. It uses:

```text
src/web2c                    ordinary C/C++ and generated C/H files
src/dvipdfm-x                ordinary xdvipdfmx C/H files
src/libpaper                 small libpaper C/H files used by xdvipdfmx
src/windows_mingw_wrapper    TeX Live style DLL wrapper sources
prebuilt-ucrt64              static libraries and generated dependency headers
MSYS2 UCRT64 system packages zlib/libpng/freetype/ICU/graphite2/harfbuzz/fontconfig
```

The directory intentionally does not include `.web`, `.ch`, `.p`, or `.pool`
literate-programming inputs/intermediates. `xdvipdfmx` is built from ordinary
C sources and does not require a WEB conversion step here.

The executable wrappers import the same DLL entrypoints used by TeX Live on
Windows:

```text
xetex.exe     -> xetex.dll:dllxetexmain
xdvipdfmx.exe -> dvipdfmx.dll:dlldvipdfmxmain
```

The normal script uses these files by default:

```sh
scripts/build-texlive-xetex-ucrt64.sh
```

To force regeneration from the original WEB/CWEB sources, disable the seed:

```sh
USE_GENERATED=0 scripts/build-texlive-xetex-ucrt64.sh
```

After regenerating, copy the updated files back into `web2c/`.

`prebuilt-ucrt64/` stores the UCRT64 static libraries and generated headers
needed by the same build route. The build script seeds them by default:

```text
texk/kpathsea
texk/ptexenc
libs/teckit
libs/pplib
texk/web2c/lib
texk/web2c/libmd5.a
texk/web2c/libxetex.a
```

To force rebuilding those libraries from source:

```sh
USE_PREBUILT_LIBS=0 scripts/build-texlive-xetex-ucrt64.sh
```
