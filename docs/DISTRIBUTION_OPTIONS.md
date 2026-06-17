# Distribution Options

StemTeX currently supports one concrete distribution shape: an embedded Windows
runtime with the patched daemon engine, native renderer DLL, SDK header/import
library, and the optional GUI.

Older package-manager and Node prototype routes were useful during research, but
they are not current delivery paths.

## Supported: Embedded StemTeX Runtime

The installer stages this layout:

```text
StemTeX\
  gui\
    stemtex-renderer-gui.exe
    Qt runtime files
  runtime\
    bin\windows\
      xetexdaemon.exe
      xetexdaemon.dll
      xdvipdfmxdaemon.exe
      dvipdfmxdaemon.dll
    bin\sdk\
      stemtex-renderer.dll
    sdk\include\
      stemtex_renderer.h
    sdk\lib\
      stemtex-renderer.lib
    cache-warmup\
      warmup.tex
    worker-template.tex
    preamble.tex
    texmf-dist\
    texmf-var\
    refresh-font-cache.ps1
```

Build sequence:

```sh
./texlive-xetex/build-standalone-msvc.sh
./texlive-xetex/install-msvc-standalone-to-side-tree.sh
./scripts/refresh-static-runtime-cache.sh
./scripts/build-cpp-daemon.sh
./scripts/sync-renderer-sdk-to-runtime.sh
./scripts/build-gui.sh
./scripts/build-stemtex-installer.sh
```

Strengths:

- The application does not depend on a user TeX installation.
- The runtime is controlled, reproducible, and known to match the renderer.
- The GUI and host applications use the same `stemtex-renderer.dll`.
- Font cache generation happens during installation, not during normal
  interactive rendering.
- The SDK files are installed next to the runtime for host integration.

Costs:

- There is no package manager inside this tree.
- The vendor rebuilds and ships a new runtime when the supported package/font
  set changes.
- The runtime is intentionally narrower than a full TeX Live installation.

Use this path for applications with a known preamble and short snippets using
ordinary CJK, English, math, chemistry, physics, and color content.

## Conceptual: User TeX Integration

An application could ship only `stemtex-renderer.dll` and a daemon-engine
overlay, then use a user-provided full TeX installation for packages.  This is
not implemented as a supported installer path.

Such an integration would need to:

1. Discover and validate the user's TeX installation.
2. Build or select a daemon-specific format.
3. Ensure `xetexdaemon` and `xdvipdfmxdaemon` are first on the runtime search
   path used by the renderer.
4. Generate cache/warmup data for that environment.
5. Store the validated paths in renderer configuration.

This remains a possible expert-mode strategy, but it trades installer size for
significantly more validation and support burden.

## Font Cache And Warmup

`--no-font-cache-refresh` is mainly a cold-start/runtime hygiene switch.  After
workers are hot, per-snippet latency is dominated by XeTeX page work and
`xdvipdfmxdaemon` conversion.

New glyphs in an already selected font are handled during PDF subsetting.  The
important warmup boundary is new font instances: CJK fallback fonts, bold or
italic variants, new math alphabets, or different OpenType fonts.  Those should
be covered by `runtime/cache-warmup/warmup.tex`.

When the default preamble or supported macro usage expands, update the warmup
document and rebuild the installer.
