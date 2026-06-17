# Generated TeX Live XeTeX Sources

This directory stores generated web2c outputs for the TeX Live upstream XeTeX
experiment, so the normal build does not need to repeat the WEB/CWEB literate
programming conversion step.

It also contains enough ordinary C/C++ source and prebuilt UCRT64 static
libraries to build `xetex.exe` without the TeX Live WEB/CWEB sources.

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
out/standalone-ucrt64/xetex.exe
```

This standalone route does not read `../texlive-source`. It uses:

```text
src/web2c                    ordinary C/C++ and generated C/H files
prebuilt-ucrt64              static libraries and generated dependency headers
MSYS2 UCRT64 system packages zlib/libpng/freetype/ICU/graphite2/harfbuzz/fontconfig
```

The directory intentionally does not include `.web`, `.ch`, `.p`, or `.pool`
literate-programming inputs/intermediates.

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
