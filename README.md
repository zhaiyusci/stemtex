# StemTeX

StemTeX is a Windows-native XeLaTeX daemon runtime for low-latency rendering of
short STEM snippets.  The current version is recorded in `VERSION`.

The project is no longer organized around the old Node worker/web preview
experiments.  The supported path is:

- a trimmed StemTeX runtime tree;
- patched `xetexdaemon` and `xdvipdfmxdaemon` binaries;
- a native C ABI renderer DLL in `cpp-daemon/`;
- a Qt-based **StemTeX Renderer GUI** in `gui/`;
- an Inno Setup installer that packages the GUI, runtime, and renderer SDK.

## Runtime Model

The renderer keeps one XeTeX worker hot with the selected profile preamble.
Render requests send a small snippet body and a text-block width to that worker. XeTeX runs with
`-no-pdf --flush-output-on-shipout --no-font-cache-refresh`, writes cumulative
XDV output, and flushes it after each `\shipout`.  Each snippet is wrapped in
LaTeX's `preview` environment, so the emitted page box is tightened around the
typeset content instead of staying at a full paper size.  The renderer then
synthesizes a valid final XDV postamble and asks the hot
`dvipdfmxdaemon.dll` converter to convert only the newest page.

This is deliberately not a general LaTeX sandbox.  The intended input is short
Chinese/English STEM text with math, chemistry, physics, color, and ordinary
inline/display formulas under a selected StemTeX profile.

## XeTeX Changes

StemTeX adds two explicit daemon switches:

```text
--flush-output-on-shipout
--no-font-cache-refresh
```

`--flush-output-on-shipout` makes a live `-no-pdf` XeTeX process write pending
XDV bytes after every `\shipout` without exiting.

`--no-font-cache-refresh` is a runtime policy switch for the daemon path.  The
installer or developer warmup step owns fontconfig cache generation; normal
interactive rendering avoids refreshing that cache.

This is not the old Web2C `-ipc`/`-ipc-start` preview protocol.  The patch keeps
the useful page-boundary buffer-flush idea and discards the TeXView-oriented
transport.

## Repository Layout

```text
cpp-daemon/
  stemtex_renderer.h              Public C ABI.
  stemtex_renderer.cpp            Renderer DLL implementation.
  stemtex_renderer_smoke.cpp      Smoke/timing executable.
  worker-template.tex             Live XeTeX worker template.

gui/
  main.cpp                        StemTeX Renderer GUI.
  assets/                         GUI icon source and generated ICO/PNG.

installer/
  stemtex.iss                     Inno Setup definition.

scripts/
  build-cpp-daemon.sh             Build stemtex-renderer.dll.
  build-gui.sh                    Build StemTeX Renderer GUI.
  smoke-cpp-renderer.sh           Canonical C++ renderer smoke entrypoint.
  stage-stemtex.sh                Stage bin/dll/include/share/texmf-dist.
  build-stemtex-installer.sh      Stage runtime/GUI/SDK and build installer.
  generate-profile-warmup.py      Generate profile warmup.tex from preamble capabilities.
  generate-gui-icon.py            Regenerate GUI PNG/ICO from SVG.
  refresh-static-runtime-cache.sh Rebuild profile warmup/cache data.
  sync-renderer-sdk-to-runtime.sh Copy renderer DLL/lib/header into runtime.

texlive-xetex/
  src/                            Generated-C XeTeX and xdvipdfmx sources.
  prebuilt-msvc/                  Static MSVC dependency libs and headers.
  third_party-msvc-src/           Source snapshots for rebuilding those libs.
  build-standalone-msvc.sh        Build xetexdaemon/xdvipdfmxdaemon.
  install-msvc-standalone-to-side-tree.sh

test/
  test_*.tex                      Historical/manual test documents.

gui/profiles/
  <name>/
    preamble.tex                  Profile preamble selected by the host/GUI.
    warmup.tex                    Matching warmup source.
```

Generated build/package directories such as `build/`, `staging/`, `dist/`,
and `texlive-xetex/out/` are local artifacts and are not part of the source
tree.  `build/` is for compiler output, `staging/` is for temporary assembly
trees, and `dist/` is for distributable runtime/installer artifacts.

## Bundled Profiles

Each profile is a directory with `preamble.tex` and `warmup.tex`.  The GUI scans
these directories and passes the selected one to the renderer. Warmup files are
generated from the profile preamble with `scripts/generate-profile-warmup.py`.
They are minimal readiness probes; correctness comes from the renderer merging
font definitions found in live XDV output.

Current source profiles are intentionally kept small so warmup coverage can stay
meaningful:

| Profile | Intended use |
| --- | --- |
| `unicodemath` | Broad Latin STEM profile with math, chemistry, physics, color, and cancel. |
| `unicodemath_cjk` | Broad CJK STEM profile with the same package set; this is the default maintained warmup target. |

## Build

Run from MSYS2.  The scripts may call Visual Studio, CMake, Qt, and Inno Setup,
but the orchestration is shell-based.

Build the daemon engine bundle:

```sh
cd /path/to/stemtex
./scripts/stage-stemtex.sh
./texlive-xetex/build-standalone-msvc.sh
./texlive-xetex/install-msvc-standalone-to-side-tree.sh
./scripts/refresh-static-runtime-cache.sh ./dist/stemtex-texlive-daemon-static ./gui/profiles/unicodemath_cjk
```

Build the renderer and GUI:

```sh
./scripts/build-cpp-daemon.sh
./scripts/sync-renderer-sdk-to-runtime.sh
./scripts/build-gui.sh
```

Run the C++ renderer smoke suite:

```sh
./scripts/smoke-cpp-renderer.sh quick
./scripts/smoke-cpp-renderer.sh errors
```

Run a native GUI smoke test:

```sh
timeout 90s ./build/gui/Release/stemtex-renderer-gui.exe --smoke
```

Build the installer:

```sh
./scripts/build-stemtex-installer.sh
```

The installer is written under:

```text
dist/installer/StemTeX-<version>-Setup.exe
```

The installer intentionally does not ship generated font cache files.  During
The installer does not pick a default profile. A host application or the GUI
chooses a profile directory and passes it to the renderer.

## Runtime Layout

The staged or installed runtime has this shape:

```text
StemTeX/
  gui/
    stemtex-renderer-gui.exe
    Qt runtime files
  runtime/
    bin/windows/
      xetexdaemon.exe
      xetexdaemon.dll
      xdvipdfmxdaemon.exe
      dvipdfmxdaemon.dll
    bin/sdk/
      stemtex-renderer.dll
    sdk/include/
      stemtex_renderer.h
    sdk/lib/
      stemtex-renderer.lib
    worker-template.tex
    gui/profiles/
      <name>/preamble.tex
      <name>/warmup.tex
    texmf-dist/
    texmf-var/
```

## Native Renderer API

Host applications should use `stemtex-renderer.dll` through the C ABI in
`cpp-daemon/stemtex_renderer.h`.  The same DLL powers the GUI.  The renderer
serializes concurrent render calls for one renderer instance and uses spare
workers only for failover, not parallel throughput.

See [docs/CPP_RENDERER_API.md](docs/CPP_RENDERER_API.md).
The current API completion/status notes are in
[docs/CPP_RENDERER_API_STATUS.md](docs/CPP_RENDERER_API_STATUS.md).

## Notes

- `docs/XELATEX_PROFILING_NOTES.md` is historical.  It records earlier timing
  work, Tectonic checks, Node prototypes, and the abandoned independent-XDV
  experiment.
- `patches/w32tex-2025-runtime-switches.md` is historical source archaeology
  for the earlier W32TeX route.
- The current source-of-truth engine patch for the generated-C tree is
  `patches/texlive-generated-daemon-runtime-switches.patch`.
