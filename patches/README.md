# Patches

For the runtime behavior implemented around this generated-C patch, see
[`docs/WINDOWS_XETEX_BUILD_NOTES.md`](../docs/WINDOWS_XETEX_BUILD_NOTES.md) and
[`docs/XETEX_CHECKPOINT_RECOVERY.md`](../docs/XETEX_CHECKPOINT_RECOVERY.md).

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

This is not the same as enabling Web2C IPC. The patch reuses the page-boundary
buffer-flush idea but skips the `ipcpage()` transport.

## Relation To Web2C IPC

Web2C IPC was for incremental DVI previewing with TeXView. It can launch or
connect to a previewer and notify it as the DVI file grows. The live worker here
needs a narrower primitive: make partial XDV bytes visible on disk after each
page, so an external controller can add a temporary postamble and run
`xdvipdfmx`.

Therefore the project adds a new explicit switch instead of relying on `-ipc` or
`-ipc-start`.

For the current generated-C route, `--no-font-cache-refresh` is implemented in
XeTeX's Fontconfig initialization by setting Fontconfig's rescan interval to
zero.
