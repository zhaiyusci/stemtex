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

- `runtime`: required daemon binaries, renderer, SDK, helpers, and optionally a
  prebuilt format for faster first startup;
- `gui`: optional GUI and profile source;
- `texmf`: optional bundled package/font tree.

The full installation selects all three. The compact installation contains the
runtime only and therefore needs a host-provided profile and a compatible TeX
Live package/font tree.

## Runtime Ownership

StemTeX always runs the patched binaries from its own runtime. The GUI and
third-party hosts both call the same `stemtex-renderer.dll`. The runtime does
not search for or switch to a user's `xelatex.exe` or stock `xelatex.fmt`.

A prebuilt `xelatexdaemon.fmt` in the runtime is an optional startup
optimization. The renderer uses an existing format directly. If it is absent,
or if real renderer startup reports an explicit format/LaTeX-kernel mismatch,
the patched StemTeX engine generates a replacement in:

```text
%LOCALAPPDATA%/StemTeX/formats/<tree-and-runtime-key>/
```

The cache key identifies the selected tree and StemTeX installation; it does
not attempt to model TeX Live package versions. There is no separate
compatibility probe: an explicit mismatch from real profile/worker startup
causes one regeneration and retry. Ordinary profile, package, and font errors
do not. Regeneration uses a private temporary directory and is published under
an inter-process lock, so neither the runtime tree nor the selected TeX Live
tree has to be writable.

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
`unicodemath/warmup.xdv` after the files are installed.

At runtime, `warmup.tex` remains the nominal profile input. The renderer stores
its derived cache under:

```text
%LOCALAPPDATA%/StemTeX/profile-xdv/<runtime-tree-profile-key>/warmup.xdv
%LOCALAPPDATA%/StemTeX/fontconfig/cache/<runtime-tree-profile-key>/
```

It reuses that file only when it parses correctly and is not older than
`preamble.tex`, `warmup.tex`, or the selected `xelatexdaemon.fmt`. Otherwise it
compiles `warmup.tex` and publishes a replacement under an inter-process lock.
A current profile-local `warmup.xdv` may still serve as an optional prebuilt
cache for the bundled tree; it is not reused for an external tree. The Qt GUI's
`清空 XDV` button calls the renderer C API to discard both keyed caches and
force XDV generation plus a fresh font scan without modifying the profile
directory.

The C API also makes writable locations explicit:

- `state_root_utf8` is an optional base for per-instance worker state. When it
  is explicitly set, it also owns the host's shared Fontconfig cache. Each
  instance creates and removes its own unique child;
- `renders_root_utf8` is an optional output directory for generated results;
- when either is omitted, the renderer uses a unique directory under the system
  temporary directory.

The profile and runtime trees can therefore remain read-only during normal
rendering. Hosts that need persistent or application-owned output locations
should set both writable paths explicitly.

## Bundled TeX Tree

The bundled `texmf-dist` is intentionally narrower than a full TeX Live
installation. It contains a file-level dependency closure for the maintained
`unicode-math` profile: selected Latin Modern OpenType faces, the Computer
Modern metric/Type 1 files needed by dynamic point-size selection, and the
LaTeX/Unicode data needed to rebuild the daemon format. It does not copy whole
font packages, all-language hyphenation patterns, or optional whitelist
packages. It has no package manager; adding supported packages or fonts
requires updating the CMake install inventory and rebuilding the installer.

This is the most reproducible deployment path because the renderer, package
versions, font maps, and converter resources are tested together. A bundled
format avoids first-start generation, but it is a reproducible cache rather
than a compatibility boundary.

## External TeX Live Tree

Advanced hosts may set `texmf_root_utf8`, and the GUI exposes the same option,
to a TeX Live-style root containing:

```text
<root>/texmf-dist/
<root>/texmf-dist/web2c/
```

For example, `C:\texlive\2026` is a valid shape. The selected tree supplies
kpathsea configuration, packages, fonts, maps, CMaps, and related backend data.
StemTeX still uses its own patched XeTeX/converter binaries and Fontconfig
configuration. It first tries a compatible cached or bundled daemon format. If
the selected tree has moved ahead of that format, as in an `expl3`/LaTeX kernel
update, the patched engine regenerates the format in the per-user cache. It
does not use the external tree's stock format and does not write into the
external tree.

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
