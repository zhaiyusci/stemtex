# Patches

`texlive-2026-flush-output-on-shipout.patch`

Applies the live-output experiment to a TeX Live 2026 source tree. It adds the
explicit XeTeX command-line option:

```text
--flush-output-on-shipout
```

With `-no-pdf`, XeTeX flushes the XDV file after each `\shipout`, while keeping
the process alive.

This is not the same as enabling legacy Web2C IPC. The patch uses `ipcon == 3`
as a new file-flush mode: it writes and flushes the DVI/XDV buffer, then skips
the old `ipcpage()` socket notification.

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

## Relation To Legacy IPC

The old Web2C IPC feature was for incremental DVI previewing with TeXView. It
can launch/connect to a previewer and notify it as the DVI file grows. The live
worker here needs a narrower primitive: make partial XDV bytes visible on disk
after each page, so an external controller can add a temporary postamble and run
`xdvipdfmx`.

Therefore the project adds a new explicit switch instead of relying on `-ipc` or
`-ipc-start`.

`texlive-generated-daemon-runtime-switches.patch`

Applies the same daemon runtime switches to the checked-in generated-C TeX Live
bundle under `texlive-xetex/src/web2c`. It is used after the WEB/CWEB conversion
has already happened, so it patches generated `xetex0.c` directly instead of
`xetex.web`.

It adds:

```text
--flush-output-on-shipout
--no-font-cache-refresh
```

For this generated-C/MinGW route, `--no-font-cache-refresh` is implemented in
XeTeX's Fontconfig initialization by setting Fontconfig's rescan interval to
zero. The old W32TeX route patched bundled fontconfig sources; this route links
against the MSYS2 UCRT64 fontconfig DLL instead.
