# Generated-C XeTeX Daemon Bundle

This directory contains the generated-C XeTeX and ordinary C `xdvipdfmx`
sources used by StemTeX.  It avoids repeating TeX Live's WEB/CWEB conversion
step during normal builds.

The supported build path is the MSVC static-dependency route:

```sh
./build-standalone-msvc.sh
```

It produces TeX Live style Windows wrappers backed by DLL entrypoints:

```text
out/standalone-msvc/
  xetexdaemon.dll
  xetexdaemon.exe
  dvipdfmxdaemon.dll
  xdvipdfmxdaemon.exe
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
