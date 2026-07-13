# Distribution Options

StemTeX currently supports one concrete distribution shape: an embedded Windows
runtime with the patched daemon engine, native renderer DLL, SDK header/import
library, and the optional GUI.

## Supported: Embedded StemTeX Runtime

The installer consumes a CMake-installed staging tree with this layout:

```text
StemTeX\
  gui\
    stemtex-renderer-gui.exe
    Qt runtime files
  runtime\
    bin\windows\
      stemtex-worker-host.exe
      xetexdaemon.exe
      xetexdaemon.dll
      xdvipdfmxdaemon.exe
      dvipdfmxdaemon.dll
      dvisvgmdaemon.exe
      dvisvgmdaemon.dll
    bin\sdk\
      stemtex-renderer.dll
    sdk\include\
      stemtex_renderer.h
    sdk\lib\
      stemtex-renderer.lib
    texmf-dist\
    texmf-var\
    run-xelatexdaemon.bat
    refresh-profile-cache.bat
  gui\
    profiles\
      <name>\
        preamble.tex
        warmup.tex
```

Build sequence:

```bat
cmake --preset ninja-msvc
cmake --build --preset ninja-release
cmake --install build/stemtex-ninja --prefix dist\stemtex-installer\StemTeX
```

The Inno Setup package is then built from that CMake-installed tree by passing
`SourceDir`, `OutputDir`, and `AppVersion` to `installer/stemtex.iss`.
Maintainers can point CMake at non-default daemon/runtime inputs with
`STEMTEX_STANDALONE_DIR`, `STEMTEX_RUNTIME_SOURCE`, and `STEMTEX_TEXLIVE_ROOT`.

Strengths:

- The application does not depend on a user TeX installation.
- The runtime is controlled, reproducible, and known to match the renderer.
- The GUI and host applications use the same `stemtex-renderer.dll`.
- Profile warmup can be generated ahead of normal interactive rendering.
- The SDK files are installed next to the runtime for host integration.

Costs:

- There is no package manager inside this tree.
- The vendor rebuilds and ships a new runtime when the supported package/font
  set changes.
- The runtime is intentionally narrower than a full TeX Live installation.

Use this path for applications with a selected profile and short snippets using
ordinary CJK, English, math, chemistry, physics, and color content.  The profile
preamble includes LaTeX's `preview` package so snippet PDFs are emitted as tight
content pages instead of full paper pages.

## Conceptual: User TeX Integration

An application could ship only `stemtex-renderer.dll` and a daemon-engine
overlay, then use a user-provided full TeX installation for packages.  This is
not implemented as a supported installer path.

The implemented expert-mode hook is narrower than generic "user TeX" support:
`texmf_root_utf8` may point at a TeX Live-style root that contains
`texmf-dist/` and `texmf-dist/web2c/`. StemTeX still runs its own patched
daemon binaries and uses that TeX Live tree for kpathsea configuration,
packages, fonts, maps, CMaps, and related backend resources.

MiKTeX is not supported as that root. Its multi-root FNDB/package-manager model
and configuration layout are different from the TeX Live `texmf-dist/web2c`
contract used by the current renderer.

Such an integration would need to:

1. Discover and validate the user's TeX installation.
2. Build or select a daemon-specific format.
3. Ensure `stemtex-worker-host`, `xetexdaemon`, `xdvipdfmxdaemon`, and
   `dvisvgmdaemon` are first on the runtime search path used by the renderer.
4. Generate cache/warmup data for that environment.
5. Store the validated paths in renderer configuration.

This remains a possible expert-mode strategy, but it trades installer size for
significantly more validation and support burden.

## Font Cache And Warmup

`--no-font-cache-refresh` is mainly a cold-start/runtime hygiene switch.  After
workers are hot, per-snippet latency is dominated by XeTeX page work and the
selected backend conversion (`xdvipdfmxdaemon` for PDF, `dvisvgmdaemon` for
SVG).

New glyphs in an already selected font are handled during PDF subsetting.  The
important warmup boundary is new font instances: CJK fallback fonts, bold or
italic variants, new math alphabets, or different OpenType fonts.  Those should
be covered by the selected profile's `warmup.tex`.

When a profile preamble or supported macro usage expands, update that profile's
warmup document and rebuild the installer.
