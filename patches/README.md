# Patches

`texlive-2026-flush-output-on-shipout.patch`

Applies the live-output experiment to a TeX Live 2026 source tree. It adds the
explicit XeTeX command-line option:

```text
--flush-output-on-shipout
```

With `-no-pdf`, XeTeX flushes the XDV file after each `\shipout`, while keeping
the process alive.

`w32tex-2025-runtime-switches.md`

Records the changes used in the W32TeX-style Windows source tree used by the
current prototype. It is a source-edit note rather than a guaranteed
`git apply` patch. It adds:

```text
--flush-output-on-shipout
--no-font-cache-refresh
```

The font-cache option maps to:

```text
FONTCONFIG_NO_CACHE_REFRESH=1
```

and makes fontconfig use existing cache files without writing or rescanning
cache data during normal snippet rendering.
