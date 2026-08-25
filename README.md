# StemTeX

StemTeX is a low-latency XeLaTeX snippet renderer. The current implementation
targets Windows and ships a trimmed TeX Live runtime, a native C ABI, and an
optional Qt GUI. The current version is recorded in [`VERSION`](VERSION).

StemTeX is built for short Chinese/English STEM fragments with text, math,
chemistry, physics, and color. It is not a general document compiler or a TeX
sandbox.

## What It Provides

- one primary hot XeTeX worker per renderer instance;
- in-process checkpoint recovery after ordinary snippet errors;
- PDF and SVG output through hot `xdvipdfmx` and `dvisvgm` converters;
- a small `stemtex-worker-host` process supervisor that prevents normal host
  shutdown from leaving an orphan `xetexdaemon`;
- a native renderer DLL and SDK for embedding;
- a Qt GUI that uses the same native API;
- a Qt-free Profile Creator C ABI and a Qt reference application for composing
  independent text, math, and CJK font choices;
- a CMake staging flow and an Inno Setup installer.

The default configuration uses one primary worker and no hot spares. Spares are
optional failover capacity for process loss, cancellation, or explicit restart;
ordinary body-level TeX errors recover in the primary worker.

## Runtime Model

The renderer starts XeTeX with a selected profile preamble and keeps it waiting
for snippet requests. Each request supplies the snippet body, text width, and
font size. XeTeX appends one tightly cropped page to a cumulative XDV stream,
then the selected hot converter emits only that page as PDF or SVG.

After warmup, XeTeX records one process-local checkpoint. A malformed snippet
restores that baseline and returns a renderer error instead of making the host
application handle a C++ exception or routinely restart the worker. See
[XeTeX checkpoint recovery](docs/XETEX_CHECKPOINT_RECOVERY.md) for the state
model and recovery protocol.

`stemtex-worker-host` owns each XeTeX child through a lifetime pipe. A ready
worker may wait indefinitely for the next snippet; the pipe is a lifetime
signal, not an idle timeout.

## Build And Try

The application, GUI, staging tree, and installer are CMake-driven and do not
require bash. From an initialized Visual Studio x64 environment:

```bat
cmake --preset ninja-msvc
cmake --build --preset ninja-release
cmake --build build\stemtex-ninja --target stage
```

Run the staged GUI smoke test:

```bat
staging\gui\stemtex-renderer-gui.exe --smoke
```

The CMake install expects previously built daemon binaries and a runtime side
tree. Rebuilding those lower-level engine inputs is a separate maintainer task.
The complete prerequisites, staging validation, and installer procedure are in
[Building and packaging](docs/BUILDING_AND_PACKAGING.md).

## Bundled Profiles

A profile directory contains `preamble.tex` and `warmup.tex`. The host or GUI
selects a profile explicitly; the renderer does not guess one.

| Profile | Intended use |
| --- | --- |
| `unicodemath` | Broad Latin STEM profile with math, chemistry, physics, color, and cancel. |
| `unicodemath_cjk` | Broad CJK STEM profile and the installer warmup target. |
| `xits_cjk` | Times-compatible XITS text and math for Word-oriented academic documents, with Windows CJK fonts. |
| `arial_lete_simhei` | Arial text, Lete Sans Math, and SimHei CJK with deterministic synthetic bold and slant. |

`warmup.tex` is source. `warmup.xdv` and Fontconfig caches are generated
artifacts and are intentionally not committed or copied by CMake install. The
full installer may generate the maintained `unicodemath_cjk` cache after files
are installed. At runtime the renderer keeps derived XDV state in the per-user
`%LOCALAPPDATA%\StemTeX\profile-xdv` cache and rebuilds it when the preamble,
warmup source, or selected format is newer. The corresponding Fontconfig cache
is also persistent and keyed by runtime/tree/profile. The Renderer GUI exposes
`清空 XDV` to clear both caches and force XDV regeneration plus a fresh font
scan for the current combination.

## Profile Creator

`stemtex-profile-creator.exe` creates normal StemTeX profiles without exposing
font-loading TeX commands. Text, math, and CJK fonts are selected independently;
the creator library resolves a maintained recipe for each choice, checks it
against the selected TeX Live tree, and generates `preamble.tex` and
`warmup.tex`.

The bundled small tree enables its maintained Latin Modern, XITS, and Lete Sans
Math recipes. Pointing the creator at a full TeX Live root additionally exposes
families such as TeX Gyre, Libertinus, STIX Two, and Fandol. Windows font recipes
cover Arial, SimSun/SimHei, and Microsoft YaHei.

Generated profiles live under `%LOCALAPPDATA%\StemTeX\profiles`. The Renderer
GUI scans that location and exposes one `字体 Profile...` button that launches
the separate creator. Other GUI technologies can call `stemtex-profile.dll`
directly through its C ABI. See [Profile Creator API](docs/PROFILE_CREATOR_API.md).

## External TeX Trees

The bundled tree is the supported default. Advanced hosts can set
`texmf_root_utf8`, and the GUI exposes the same choice, to read packages and
fonts from an external TeX Live root containing `texmf-dist` and
`texmf-dist/web2c`.

StemTeX still uses its own patched engine, converters, and Fontconfig setup.
At startup it directly tries a cached or bundled `xelatexdaemon.fmt`. If real
renderer startup reports an explicit format/LaTeX-kernel incompatibility, the
patched engine regenerates the format once under
`%LOCALAPPDATA%\StemTeX\formats` and retries. A missing format is generated
immediately. There is no separate compatibility probe. This option never runs the external
installation's XeTeX binary or writes into its tree. MiKTeX roots are not
supported because their root, FNDB, and package-management model does not match
this TeX Live contract.

## Documentation

The [documentation index](docs/README.md) assigns one responsibility to each
document:

- [Building and packaging](docs/BUILDING_AND_PACKAGING.md): normal CMake build,
  clean staging, smoke tests, and installer creation.
- [C renderer API](docs/CPP_RENDERER_API.md): public ABI, ownership, errors,
  output formats, and host examples.
- [Profile Creator API](docs/PROFILE_CREATOR_API.md): font catalog, recipes,
  managed profiles, generation ABI, and reference Qt application.
- [Renderer status](docs/CPP_RENDERER_API_STATUS.md): current contract and
  remaining product work.
- [Distribution options](docs/DISTRIBUTION_OPTIONS.md): installed layout,
  writable data, profiles, and external TeX Live integration.
- [XeTeX checkpoint recovery](docs/XETEX_CHECKPOINT_RECOVERY.md): live-worker
  error recovery design and regression coverage.
- [Windows engine rebuild notes](docs/WINDOWS_XETEX_BUILD_NOTES.md): lower-level
  generated-C daemon and static dependency maintenance.

## Repository Layout

```text
cpp-daemon/       Native renderer, worker supervisor, public header, and smoke tests.
gui/              Qt GUI, assets, and rendering profiles.
profile-creator/   Qt-free profile catalog/generator C ABI and smoke tests.
profile-creator-qt/ Qt reference front end for font profile creation.
installer/        Inno Setup definition.
cmake/            Helpers installed into the runtime.
scripts/          Profile generation and checkpoint audit tools.
texlive-xetex/    Generated-C engines, converter sources, and MSVC dependencies.
patches/          Patch records for the generated TeX Live source bundle.
docs/             User, integration, build, and design documentation.
```

Generated `build/`, `staging/`, `dist/`, and `texlive-xetex/out/` directories
are local artifacts, not source. A staged or installed tree has two top-level
parts:

```text
StemTeX/
  gui/
    stemtex-renderer-gui.exe
    profiles/<name>/{preamble.tex,warmup.tex}
  runtime/
    bin/windows/                  Patched engines and worker host.
    bin/sdk/stemtex-renderer.dll
    sdk/{include,lib}/            C header and import library.
    texmf-dist/                   Optional trimmed package/font tree.
    texmf-var/                    Format and generated runtime data.
```

The source-of-truth public API is
[`cpp-daemon/stemtex_renderer.h`](cpp-daemon/stemtex_renderer.h). The
source-of-truth generated-C runtime switch record is
[`patches/texlive-generated-daemon-runtime-switches.patch`](patches/texlive-generated-daemon-runtime-switches.patch).
