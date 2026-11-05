# Patches

## LuaTeX / LuaHBTeX DVI page flushing

`luatex-dvi-flush-on-shipout.patch` targets the TeX Live source layout and was
developed against LuaTeX 1.26.2, development revision 7748. It adds the opt-in
`--flush-output-on-shipout` switch. In DVI mode, each completed shipout drains
both the engine's DVI buffer and the C file stream. The stream stays open and
keeps its absolute offsets; the host can copy complete pages and add a DVI
postamble to the copy. The patch also fixes final-file padding for the now
possibly unaligned buffer base. It does not add checkpoints or change the
native PDF backend.

The native Windows experimental build is separate from the released XeTeX
runtime. Use `scripts/build-luahbtex-mingw.ps1` to prepare and build a patched
copy of a TeX Live source tree, then `scripts/test-luatex-dvi-flush.ps1` to
generate compatible local formats and run live-output regression checks.
The source tree and installed TeX Live are not modified.

## XeTeX generated-C runtime switches

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
