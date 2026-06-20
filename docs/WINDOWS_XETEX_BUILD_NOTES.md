# Windows XeTeX Build Notes

StemTeX currently builds its daemon engine from the generated-C source bundle in
`texlive-xetex/`.  The preferred Windows route is MSVC with static third-party
dependencies.

## Current Build Route

Build from MSYS2, using the installed Visual Studio toolchain:

```sh
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

The `.exe` files are small `calldll` wrappers.  The actual engine/converter
code lives in the DLLs:

```text
xetexdaemon.exe     -> xetexdaemon.dll:dllxetexmain
xdvipdfmxdaemon.exe -> dvipdfmxdaemon.dll:dlldvipdfmxmain
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

Install the built binaries into the static StemTeX side tree:

```sh
./texlive-xetex/install-msvc-standalone-to-side-tree.sh
```

Then refresh the runtime warmup/cache data:

```sh
./scripts/refresh-static-runtime-cache.sh ./dist/stemtex-texlive-daemon-static ./profiles/unicodemath_cjk
```

## Source Inputs

The build does not read a full TeX Live checkout during normal operation.  It
uses:

```text
texlive-xetex/src/web2c
texlive-xetex/src/dvipdfm-x
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

```sh
./texlive-xetex/build-thirdparty-msvc.sh
./texlive-xetex/build-texlive-libs-msvc.sh
```

This regenerates the static dependency inputs used by
`build-standalone-msvc.sh`.  Build products inside `third_party-msvc-src/` are
not source and should not be committed.

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

The build/install scripts assemble:

```text
dist/stemtex-texlive-daemon-static/
  bin/windows/
    xetexdaemon.exe
    xetexdaemon.dll
    xdvipdfmxdaemon.exe
    dvipdfmxdaemon.dll
  profiles/
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
profiles/<name>/preamble.tex
profiles/<name>/warmup.tex
```

The renderer requires the host or GUI to pass a profile directory explicitly.
The preamble loads `preview` with `active,tightpage`; the live worker wraps each
request in a `preview` environment so the resulting PDF page is already cropped
to the snippet content.

## Historical Routes

The earlier W32TeX source route produced `ptx/texk/web2c/xetex.dll` and patched
bundled fontconfig sources.  That work is retained only as notes in:

```text
patches/w32tex-2025-runtime-switches.md
```

The UCRT64 route is still present for comparison:

```sh
./texlive-xetex/build-standalone-ucrt64.sh
```

It is not the preferred distribution route because it pulls in a larger MSYS2
runtime dependency set.
