# Distribution Options

This project now focuses on a single embeddable runtime: **StemTeX**, a trimmed
Windows XeLaTeX tree with the patched daemon engine and a fixed STEM-oriented
package set.

## StemTeX Embedded Runtime

StemTeX is the product/runtime shape. It is small enough to ship inside another
application and controlled enough for low-latency snippet rendering.

```text
stemtex\
  bin\windows\
    xetexdaemon.exe
    xetexdaemon.dll
    xelatexdaemon.bat
    xdvipdfmx.exe
    required DLLs
  cache-warmup\
    warmup.tex
  texmf-dist\
  texmf-var\
  run-xelatexdaemon.bat
  refresh-font-cache.ps1
```

Build script:

```powershell
.\scripts\build-windows-native.ps1 -Target All -Arch x64

.\scripts\build-stemtex-runtime.ps1 `
  -TeXLiveRoot C:\texlive\2026 `
  -Destination .\stemtex `
  -Clean
```

Strengths:

- Small enough to embed in an application.
- The runtime is controlled and reproducible.
- No user TeX Live installation is required.
- Fast and predictable after fontconfig cache warmup.
- Uses daemon names in the shipped runtime.

Costs:

- No package manager inside the runtime.
- Users cannot freely install arbitrary packages inside this runtime.
- If the fixed preamble changes enough to need new packages/fonts, the vendor
  rebuilds and ships a new runtime.

Use this when the product has a known preamble and snippets are ordinary CJK,
English, math, chemistry, physics, and color content. The default preamble
currently loads `mathtools`, `mhchem`, `physics`, `xcolor`, and `cancel` on top
of `unicode-math` and `xeCJK`.

The embedded tree also includes `siunitx` for alternate preamble variants, but
it is not loaded by the default low-latency preamble because it conflicts with
`physics` over `\qty`.

## Existing TeX Integration

For expert users, an application may ship only the patched engine overlay and
renderer code, then use a user-provided full TeX installation for packages.
This is not the default distribution path.

```text
MyApp\
  renderer\
    stemtex-renderer.dll
    worker-webapp.tex
  xetexdaemon\
    xetexdaemon.exe
    xetexdaemon.dll
    xelatexdaemon.fmt
    setup-user-texlive.ps1
```

At setup time:

```text
1. Find the user's TeX bin directory.
2. Verify kpsewhich, xelatex.ini, xdvipdfmx, and required packages.
3. Put xetexdaemon before the user's TeX bin directory in PATH.
4. Build a daemon-specific format with xetexdaemon.
5. Run a warmup document to check fonts and cache behavior.
6. Save the discovered paths in renderer config.
```

Strengths:

- Smallest application download.
- Advanced users can keep using their own TeX installation.
- No need to distribute a TeX package tree.

Costs:

- Requires an existing user TeX installation.
- The app must discover and validate the user's TeX tree.
- Version compatibility matters. The safest setup builds a daemon-specific
  format with the daemon executable.
- Font cache policy is partly delegated to the user's TeX environment.

## Font Cache And Warmup

`--no-font-cache-refresh` mostly affects cold startup and first font discovery.
It is not a per-snippet typesetting accelerator after the live worker is hot.

New glyphs in an already selected font are normally handled by `xdvipdfmx` when
it subsets the selected page. The important boundary is new font instances:
CJK fallback fonts, bold/italic variants, new math alphabets, or different
OpenType fonts. Those should be covered by `cache-warmup\warmup.tex`.

For StemTeX, treat fontconfig cache and warmup as runtime/installation state.
If the default preamble or supported macro usage expands, update the warmup file
and rebuild the runtime/installer.

## Current Preference

For an embeddable software component, StemTeX embedded runtime is the default
and supported distribution. It is the smallest controlled package and matches
the fixed-preamble snippet-rendering product goal.
