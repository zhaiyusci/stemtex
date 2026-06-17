# W32TeX 2025 Runtime Switches

These historical notes record the local W32TeX-style source changes used by an
earlier prototype. They are intentionally notes rather than a blindly applicable
patch: the current build path is the generated-C bundle under `texlive-xetex/`,
and the checked-in repository does not vendor the old `ptx/` source tree.

## XeTeX Options

In `ptx\texk\web2c\lib\texmfmp.c`, add XeTeX options near `no-pdf`:

```c
{ "no-font-cache-refresh",     0, 0, 0 },
{ "flush-output-on-shipout",   0, &ipcon, 3 },
```

In `parse_options`, map the font-cache flag to an environment variable:

```c
} else if (ARGUMENT_IS ("no-font-cache-refresh")) {
  xputenv ("FONTCONFIG_NO_CACHE_REFRESH", "1");
```

In `ptx\texk\web2c\texmfmp-help.h`, add help lines:

```c
"-no-font-cache-refresh  use existing fontconfig caches only",
"-flush-output-on-shipout flush XDV/PDF output after each \\shipout",
```

## Fontconfig

In `ptx\libs\fontconfig\src\fccache.c`, add:

```c
FcBool
FcCacheNoRefresh (void)
{
    const char *env = getenv ("FONTCONFIG_NO_CACHE_REFRESH");
    FcBool value;

    return env && FcNameBool ((const FcChar8 *) env, &value) && value;
}
```

Declare it in `ptx\libs\fontconfig\src\fcint.h`:

```c
FcPrivate FcBool
FcCacheNoRefresh (void);
```

In `ptx\libs\fontconfig\src\fcdir.c`, make cache scanning/rescanning return
early when `FcCacheNoRefresh()` is true, and only fall back to
`FcDirCacheScan()` when refresh is allowed:

```c
if (FcCacheNoRefresh ())
    return NULL;

if (!cache && !FcCacheNoRefresh ())
    cache = FcDirCacheScan (dir, config);
```

## Generated XeTeX C

In `ptx\texk\web2c\xetex0.c`, after a page is shipped out and after:

```c
if ( ! nopdfoutput )
fflush ( dvifile ) ;
```

add the `ipcon == 3` branch that writes the pending DVI/XDV buffer, calls
`flushdvi()`, updates `dvioffset`/`dvigone`, and resets `dviptr`/`dvilimit`.

This mirrors the existing `IPC` flush block but deliberately skips `ipcpage()`;
it is a file-flush switch, not TeXView socket IPC.
