# Distribution Options

StemTeX currently supports one release shape: an embedded Windows runtime with
the patched daemon engines, native renderer DLL, SDK, optional Qt GUI, and an
optional trimmed TeX package/font tree. The installed runtime is the default and
fully controlled environment.

Build and installer commands live in
[Building and packaging](BUILDING_AND_PACKAGING.md). This document defines what
the resulting tree contains and which data may come from outside it.

## Installed Layout

```text
StemTeX/
  gui/
    stemtex-renderer-gui.exe
    stemtex-profile-creator.exe
    Qt runtime files
    profiles/
      <name>/
        preamble.tex
        warmup.tex
  runtime/
    bin/windows/
      stemtex-worker-host.exe
      xetexdaemon.exe
      xetexdaemon.dll
      xdvipdfmxdaemon.exe
      dvipdfmxdaemon.dll
      dvisvgmdaemon.exe
      dvisvgmdaemon.dll
      icu-data/
    bin/sdk/
      stemtex-renderer.dll
      stemtex-profile.dll
    sdk/include/
      stemtex_renderer.h
      stemtex_profile.h
    sdk/lib/
      stemtex-renderer.lib
    texmf-dist/
    texmf-var/
    VERSION
    run-xelatexdaemon.bat
    refresh-profile-cache.bat
```

The Inno Setup package exposes three components:

- `runtime`: required daemon binaries, renderer, SDK, format, and helpers;
- `gui`: optional GUI and profile source;
- `texmf`: optional bundled package/font tree.

The full installation selects all three. The compact installation contains the
runtime only and therefore needs a host-provided profile and a compatible TeX
Live package/font tree.

## Runtime Ownership

StemTeX always runs the patched binaries and daemon format from its own runtime.
The GUI and third-party hosts both call the same `stemtex-renderer.dll`. The
runtime does not search for or switch to a user's `xelatex.exe`.

Each renderer has one primary XeTeX worker. `spare_worker_count = 0` is the
normal low-memory configuration; positive values add failover workers, not
parallel render throughput. See [C renderer API](CPP_RENDERER_API.md) for the
host contract.

## Profiles And Generated Data

Profile source consists of:

```text
gui/profiles/<name>/preamble.tex
gui/profiles/<name>/warmup.tex
```

The separate Profile Creator writes user-managed profiles to:

```text
%LOCALAPPDATA%/StemTeX/profiles/<name>/
  profile.json
  preamble.tex
  warmup.tex
```

The Renderer GUI scans both collections. `profile.json` is creator metadata;
the renderer continues to consume only `preamble.tex` and `warmup.tex`.

`warmup.xdv`, `.aux`, `.log`, PDF output, and Fontconfig cache files are
generated artifacts. CMake install and the installer payload exclude them. On a
full installation, the installer refreshes Fontconfig and generates
`unicodemath_cjk/warmup.xdv` after the files are installed.

At runtime, a valid profile `warmup.xdv` can accelerate worker startup. If it is
missing or unreadable, the renderer compiles `warmup.tex` inside that renderer
instance's private state directory. It does not write a new cache into the
profile directory, so concurrent renderer instances do not publish over one
another.

The C API also makes writable locations explicit:

- `state_root_utf8` is an optional base for per-instance worker state and shared
  Fontconfig cache. Each instance creates and removes its own unique child;
- `renders_root_utf8` is an optional output directory for generated results;
- when either is omitted, the renderer uses a unique directory under the system
  temporary directory.

The profile and runtime trees can therefore remain read-only during normal
rendering. Hosts that need persistent or application-owned output locations
should set both writable paths explicitly.

## Bundled TeX Tree

The bundled `texmf-dist` is intentionally narrower than a full TeX Live
installation. It contains the packages, fonts, maps, and configuration needed
by the maintained profiles. It has no package manager; adding supported packages
or fonts requires updating the CMake install inventory and rebuilding the
installer.

This is the most reproducible deployment path because the renderer, format,
package versions, font maps, and converter resources are tested together.

## External TeX Live Tree

Advanced hosts may set `texmf_root_utf8`, and the GUI exposes the same option,
to a TeX Live-style root containing:

```text
<root>/texmf-dist/
<root>/texmf-dist/web2c/
```

For example, `C:\texlive\2026` is a valid shape. The selected tree supplies
kpathsea configuration, packages, fonts, maps, CMaps, and related backend data.
StemTeX still uses its own patched XeTeX/converter binaries, daemon format,
Fontconfig configuration, and writable cache locations.

This mode is useful when a snippet needs packages or fonts outside the bundled
small tree, but it has a larger compatibility surface. Validate the chosen tree
with `stemtex_renderer_validate_config` and run representative PDF and SVG
renders before enabling it for users.

## Unsupported MiKTeX Roots

MiKTeX is not accepted as `texmf_root_utf8`. MiKTeX uses a multi-root FNDB and
package-management/configuration model rather than the TeX Live
`texmf-dist/web2c` contract. StemTeX does not query MiKTeX Core, invoke its
package manager, or run its XeTeX binaries.

Supporting MiKTeX would require a separate integration layer; it is not treated
as a variation of the current external TeX Live option.

## Current Portability Boundary

The renderer architecture keeps Qt outside the native library, but the current
ABI, process management, daemon binaries, packaging, and tested runtime are
Windows-specific. A future non-Windows port can retain the hot-worker and
checkpoint model, but it needs platform implementations for the native ABI
exports, worker supervision, engine build, and distribution layout.
