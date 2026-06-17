# Patches

`texlive-generated-daemon-runtime-switches.patch`

Current StemTeX engine patch record. It applies the daemon runtime switches to
the checked-in generated-C TeX Live bundle under `texlive-xetex/src/web2c`.
It patches generated `xetex0.c` directly, after the WEB/CWEB conversion has
already happened.

It adds:

```text
--flush-output-on-shipout
--no-font-cache-refresh
```

`--flush-output-on-shipout` makes a live `-no-pdf` XeTeX process flush pending
XDV bytes after each `\shipout`.

`--no-font-cache-refresh` disables fontconfig rescans during normal daemon
rendering. Installation/warmup owns cache generation.

This is not the same as enabling legacy Web2C IPC. The patch reuses the old
page-boundary buffer-flush idea but skips the old `ipcpage()` transport.

`texlive-2026-flush-output-on-shipout.patch`

Older source-tree patch for a TeX Live 2026 checkout. It is retained as
archaeology for the pre-generated-C route.

`w32tex-2025-runtime-switches.md`

Historical source-edit notes from the earlier W32TeX-style Windows source tree.
It is not the current build path and is not a guaranteed `git apply` patch.
It records the same two user-facing switches:

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

For the current generated-C route, `--no-font-cache-refresh` is implemented in
XeTeX's Fontconfig initialization by setting Fontconfig's rescan interval to
zero. The old W32TeX route patched bundled fontconfig sources instead.
