# StemTeX

StemTeX is a Windows-native XeLaTeX daemon runtime for low-latency rendering of
short STEM snippets.  The current version is recorded in `VERSION`.

The supported path is:

- a trimmed StemTeX runtime tree;
- patched `xetexdaemon` and `xdvipdfmxdaemon` binaries;
- a tiny `stemtex-worker-host` process supervisor for XeTeX daemon invocations;
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

The renderer does not launch `xetexdaemon` directly.  Warmup compilation,
profile-cache refresh, and live XeTeX workers all go through
`stemtex-worker-host`, which then starts `xetexdaemon` with the renderer's
stdio pipes.  A private lifetime pipe stays open for as long as the renderer
owns that invocation.  Closing that pipe during renderer destroy, restart,
cancel, timeout, or host-process death makes the helper terminate the child
`xetexdaemon`.  This is not an idle timeout; a hot worker may wait indefinitely
for the next snippet.

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
  stemtex_worker_host.cpp         Tiny worker-process supervisor.
  stemtex_renderer_smoke.cpp      Smoke/timing executable.

gui/
  main.cpp                        StemTeX Renderer GUI.
  assets/                         GUI icon source and generated ICO/PNG.

installer/
  stemtex.iss                     Inno Setup definition.

scripts/
  generate-profile-warmup.py      Generate profile warmup.tex from preamble capabilities.
  generate-gui-icon.py            Regenerate GUI PNG/ICO from SVG.

texlive-xetex/
  src/                            Generated-C XeTeX and xdvipdfmx sources.
  prebuilt-msvc/                  Static MSVC dependency libs and headers.
  third_party-msvc-src/           Source snapshots for rebuilding those libs.
  build-standalone-msvc.sh        Build xetexdaemon/xdvipdfmxdaemon.
  install-msvc-standalone-to-side-tree.sh

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

The supported application build is CMake-driven.  Use a Visual Studio x64
developer environment, or initialize `vcvars64.bat` before using the Ninja
preset.

Configure and build the renderer and GUI:

```bat
cmake --preset ninja-msvc
cmake --build --preset ninja-release
```

Install a staged tree:

```bat
cmake --install build/stemtex-ninja --prefix staging
```

The Visual Studio generator preset is also available:

```bat
cmake --preset vs2022
cmake --build --preset release
cmake --install build/stemtex --config Release --prefix staging
```

The CMake install step expects the daemon binaries and runtime side tree to
exist.  The default inputs are:

```text
texlive-xetex/out/standalone-msvc
dist/stemtex-texlive-daemon-static
```

Override them with `STEMTEX_STANDALONE_DIR` and `STEMTEX_RUNTIME_SOURCE` CMake
cache variables when using a different local layout.  Rebuilding the daemon
engine itself is a maintainer workflow documented in
[docs/WINDOWS_XETEX_BUILD_NOTES.md](docs/WINDOWS_XETEX_BUILD_NOTES.md).

Run a native renderer smoke test against the staged tree:

```bat
build\stemtex-ninja\cpp-daemon\stemtex-renderer-smoke.exe ^
  --repo "%CD%" ^
  --runtime "%CD%\staging\runtime" ^
  --profile "%CD%\staging\gui\profiles\unicodemath_cjk" ^
  --case validate
```

Run a native GUI smoke test:

```bat
staging\gui\stemtex-renderer-gui.exe --smoke
```

Build the installer from a CMake-installed staging tree:

```powershell
$version = (Get-Content .\VERSION).Trim()
$stage = "$PWD\dist\stemtex-installer\StemTeX"
$output = "$PWD\dist\installer"
cmake --install build/stemtex-ninja --prefix $stage
New-Item -ItemType Directory -Force $output | Out-Null
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" `
  "/DSourceDir=$stage" `
  "/DOutputDir=$output" `
  "/DAppVersion=$version" `
  .\installer\stemtex.iss
```

The installer is written under:

```text
dist/installer/StemTeX-<version>-Setup.exe
```

The installer intentionally does not ship generated font cache files.  It
refreshes the selected profile cache during installation when the GUI and
bundled TeX tree are selected.  The installer does not pick a default profile.
A host application or the GUI chooses a profile directory and passes it to the
renderer.

## Runtime Layout

The staged or installed runtime has this shape:

```text
StemTeX/
  gui/
    stemtex-renderer-gui.exe
    Qt runtime files
  runtime/
    bin/windows/
      stemtex-worker-host.exe
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
    gui/profiles/
      <name>/preamble.tex
      <name>/warmup.tex
    texmf-dist/
    texmf-var/
```

## External TeX Trees

The installed runtime is the supported default. Advanced hosts may set the
renderer's `texmf_root_utf8` field, or use the GUI's TeX tree selector, to read
packages and fonts from a full external TeX tree. That external tree must use
the TeX Live layout: the selected root is expected to contain `texmf-dist/` and
`texmf-dist/web2c/`, for example `C:\texlive\2026`.

This does not switch the engine to the user's TeX binaries. StemTeX still runs
its patched `xetexdaemon` and `xdvipdfmxdaemon` from the StemTeX runtime; the
external TeX Live tree supplies the kpathsea configuration, packages, fonts,
maps, CMaps, and related data.

MiKTeX roots are not supported by this option. MiKTeX uses a different root
model, FNDB/package-management layer, and configuration layout, and StemTeX does
not query MiKTeX Core or run MiKTeX's `xelatex.exe`.

## Native Renderer API

Host applications should use `stemtex-renderer.dll` through the C ABI in
`cpp-daemon/stemtex_renderer.h`.  The same DLL powers the GUI.  The renderer
serializes concurrent render calls for one renderer instance and uses spare
workers only for failover, not parallel throughput.

See [docs/CPP_RENDERER_API.md](docs/CPP_RENDERER_API.md).
The current API completion/status notes are in
[docs/CPP_RENDERER_API_STATUS.md](docs/CPP_RENDERER_API_STATUS.md).

## Notes

- The current source-of-truth engine patch for the generated-C tree is
  `patches/texlive-generated-daemon-runtime-switches.patch`.
