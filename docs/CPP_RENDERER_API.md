# C++ Renderer API

This document describes the native DLL-style interface in `cpp-daemon/`.

The C++ layer is intentionally exposed as a small C ABI so host applications can
load it like a normal Windows DLL without using a network protocol.

## Build Outputs

The current CMake build emits:

```text
build/cpp-daemon/Release/stemtex-renderer.dll
build/cpp-daemon/Release/stemtex-renderer.lib
build/cpp-daemon/Release/stemtex-renderer-smoke.exe
```

The public header is:

```text
cpp-daemon/stemtex_renderer.h
```

In the installer/runtime layout, the renderer is treated as part of the StemTeX
runtime SDK:

```text
runtime\
  bin\sdk\stemtex-renderer.dll
  sdk\include\stemtex_renderer.h
  sdk\lib\stemtex-renderer.lib
```

## Runtime Model

`stemtex-renderer.dll` owns one primary XeTeX worker plus a configurable number
of hot spare workers:

- `primary`: handles normal render requests.
- `spare-N`: stays hot as an immediate failover target.

On create, it:

1. Reads the configured StemTeX runtime.
2. Loads XDV font definitions from the installation-time warmup output under
   `texmf-var/cache-warmup`.
3. Falls back to running a warmup worker with `cache-warmup/warmup.tex` only if
   that cached XDV is missing or unreadable.
4. Starts and primes a primary live worker.
5. Starts building spare live workers in the background.

`create` returns after the primary worker is ready. Spare workers are then built
asynchronously, so increasing the spare count does not lengthen the foreground
create path. This also avoids parallel XeTeX font/loading contention during the
critical user-visible path.

The fixed preamble currently uses native/OpenType fonts through `fontspec`,
`xeCJK`, and `unicode-math`. XeTeX refuses to dump a format after native fonts or
font mappings have been selected:

```text
! Can't \dump a format with native fonts or font-mappings.
```

So the product path does not try to bake the full preamble into a custom fmt.
Instead, installation-time warmup owns fontconfig cache generation and XDV
font-definition extraction.

On render, it:

1. Writes the snippet body to a request file.
2. Sends width and request path to the primary live worker.
3. Wraps the snippet in a fixed-width `preview` page inside XeTeX.
4. Waits for `WORKER_DONE:N`.
5. Reads the current cumulative live XDV.
6. Synthesizes a valid final XDV postamble.
7. Calls `xdvipdfmx -s N-N` to convert only the newest page.
8. Returns the PDF path and a JSON timing summary.

The current conversion path is deliberately conservative: cumulative XDV plus
latest-page conversion. The earlier standalone-delta XDV experiment is not part
of this API.

The `preview` package owns the tight page box.  The GUI may still crop the
rendered bitmap for display convenience, but the PDF itself is already cropped
to the snippet content plus the configured preview border.

## Public API

```cpp
typedef struct StemTeXConfig {
  const char *repo_root_utf8;
  const char *runtime_root_utf8;
  const char *state_root_utf8;
  const char *renders_root_utf8;
  int request_timeout_ms;
  int xdvipdfmx_timeout_ms;
  int min_width_pt;
  int max_width_pt;
  int default_width_pt;
  int spare_worker_count;
  int auto_restart;
  int delete_intermediates;
  const char *warmup_tex_utf8;
  const char *worker_template_utf8;
  const char *preamble_tex_utf8;
} StemTeXConfig;
```

Fields:

- `repo_root_utf8`: repository/resource root. The renderer expects to find
  `cpp-daemon/worker-template.tex` and `test/preamble.tex` under this tree in
  a source checkout. If they are absent, it falls back to
  `runtime_root\worker-template.tex` and `runtime_root\preamble.tex`.
- `runtime_root_utf8`: StemTeX runtime root, meaning the directory that directly
  contains `bin\windows\xetexdaemon.exe`, `worker-template.tex`,
  `preamble.tex`, and `cache-warmup\warmup.tex`. In the installer layout this
  is normally `C:\StemTeX\runtime`.
- `state_root_utf8`: optional worker state directory. If null, the renderer
  uses a unique directory under the system temporary directory.
- `renders_root_utf8`: optional render output directory. If null, the renderer
  uses a unique directory under the system temporary directory.
- `request_timeout_ms`: worker request timeout. `0` uses `90000`.
- `xdvipdfmx_timeout_ms`: PDF conversion timeout. `0` uses `90000`.
- `default_width_pt`: width used when a render call passes `width_pt <= 0`.
  `0` uses `360`.
- `min_width_pt`, `max_width_pt`: retained in the ABI for host-side policy, but
  the renderer no longer clamps `width_pt`.
- `spare_worker_count`: number of hot spare workers to maintain. `0` means the
  default, currently `1`. Positive values are clamped internally; the current
  maximum is `4`.
- `auto_restart`: reserved policy flag; zero-initialized configs keep automatic
  recovery enabled.
- `delete_intermediates`: delete request/XDV intermediates after successful
  render while keeping PDF and summary.
- `warmup_tex_utf8`, `worker_template_utf8`, `preamble_tex_utf8`: optional
  resource overrides.

Create a renderer:

```cpp
StemTeXRenderer *stemtex_renderer_create(
  const StemTeXConfig *config,
  StemTeXErrorCode *error_code,
  char **error_utf8
);
```

Render one snippet:

```cpp
int stemtex_renderer_render(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  int width_pt,
  StemTeXRenderResult *result,
  StemTeXErrorCode *error_code,
  char **error_utf8
);
```

Related APIs:

```cpp
int stemtex_renderer_render_pdf_bytes(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  int width_pt,
  StemTeXPdfBytes *pdf,
  StemTeXRenderResult *result,
  StemTeXErrorCode *error_code,
  char **error_utf8
);

int stemtex_renderer_render_async(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  int width_pt,
  StemTeXRenderCallback callback,
  void *user_data,
  StemTeXErrorCode *error_code,
  char **error_utf8
);

int stemtex_renderer_restart(StemTeXRenderer *renderer, StemTeXErrorCode *error_code, char **error_utf8);
int stemtex_renderer_cancel_current(StemTeXRenderer *renderer, StemTeXErrorCode *error_code, char **error_utf8);
StemTeXRendererStatus stemtex_renderer_status(StemTeXRenderer *renderer);
int stemtex_renderer_engine_snapshot(StemTeXRenderer *renderer, StemTeXEngineSnapshot *snapshot);
StemTeXErrorCode stemtex_renderer_last_error_code(StemTeXRenderer *renderer);
char *stemtex_renderer_get_log_tail(StemTeXRenderer *renderer, int max_bytes);
const char *stemtex_renderer_version(void);
const char *stemtex_renderer_abi_version(void);
char *stemtex_renderer_runtime_version(StemTeXRenderer *renderer);
int stemtex_renderer_validate_config(const StemTeXConfig *config, StemTeXErrorCode *error_code, char **diagnostics_utf8);
int stemtex_refresh_font_cache(const char *runtime_root_utf8, const char *warmup_tex_utf8,
                               StemTeXErrorCode *error_code, char **error_utf8);
```

Render result:

```cpp
typedef struct StemTeXRenderResult {
  char *request_id_utf8;
  char *pdf_path_utf8;
  char *summary_json_utf8;
} StemTeXRenderResult;
```

Cleanup:

```cpp
void stemtex_renderer_free_result(StemTeXRenderResult *result);
void stemtex_renderer_free_pdf_bytes(StemTeXPdfBytes *pdf);
void stemtex_renderer_free_string(char *value);
void stemtex_renderer_destroy(StemTeXRenderer *renderer);
```

## Minimal Host Example

```cpp
#include "stemtex_renderer.h"

#include <cstdio>

int main() {
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = "C:\\Users\\jairy\\Documents\\xetex\\stemtex";
  cfg.runtime_root_utf8 = "C:\\StemTeX\\runtime";

  char *error = nullptr;
  StemTeXErrorCode error_code = STEMTEX_OK;
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &error_code, &error);
  if (!renderer) {
    std::fprintf(stderr, "create failed code=%d: %s\n", (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return 1;
  }

  StemTeXRenderResult result{};
  if (!stemtex_renderer_render(renderer, u8"中文 $E=mc^2$", 360, &result, &error_code, &error)) {
    std::fprintf(stderr, "render failed code=%d: %s\n", (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    stemtex_renderer_destroy(renderer);
    return 1;
  }

  std::printf("pdf: %s\n", result.pdf_path_utf8);
  std::printf("summary: %s\n", result.summary_json_utf8);

  stemtex_renderer_free_result(&result);
  stemtex_renderer_destroy(renderer);
  return 0;
}
```

## Width

`width_pt` is the snippet text block width in TeX points.

The renderer does not clamp positive width values:

```text
The C API sends positive `width_pt` values to TeX unchanged. GUI frontends may
still impose their own control ranges.
```

If `width_pt <= 0`, the renderer uses `360pt`.

## Summary JSON

`summary_json_utf8` currently contains fields like:

```json
{
  "pdfMode": "cpp-dll-live-worker-latest-page",
  "widthPt": 360,
  "requestToPdfMs": 203,
  "finalizeXdvMs": 2,
  "xdvipdfmxMs": 124,
  "cumulativeXdvBytes": 11519,
  "newXdvBytes": 1153,
  "finalXdvBytes": 12352,
  "pdfBytes": 26111,
  "workerRequest": 2,
  "workerSlot": "primary",
  "spareReady": 1,
  "spareTarget": 1,
  "spareRebuilding": false
}
```

The stable high-level timings are:

- `requestToPdfMs`: total hot render time for this request.
- `xdvipdfmxMs`: PDF conversion time.
- `finalizeXdvMs`: time spent writing the synthesized final XDV.

## Error Handling

The live worker is started with:

```text
-interaction=errorstopmode -halt-on-error -no-pdf -flush-output-on-shipout
```

If a snippet contains a TeX error, XeTeX exits. The renderer detects that it did
not receive `WORKER_DONE:N`, returns failure from `stemtex_renderer_render`, and
includes the recent TeX output tail in `error_utf8`.

After a primary worker failure or request timeout, the renderer immediately
promotes a hot spare to primary and schedules replacement spares in the
background. The failing request still fails, but the next request can use the
promoted worker without paying cold-start cost.

Concurrent render calls on one renderer are serialized. The spare pool is only
for failover/recovery; it is not used as a parallel rendering pool.

`stemtex_renderer_cancel_current` kills the currently active worker. The active
render returns `STEMTEX_ERROR_CANCELLED`, and the renderer promotes/rebuilds a
worker for subsequent queued requests.

If primary and all spares are unavailable, the renderer falls back to creating a
new primary synchronously. That is the degraded path and can pay cold-start
latency.

If `xdvipdfmx` fails after TeX has already produced XDV, the renderer returns
failure and includes the converter stdout/stderr tail in `error_utf8`. This path
does not automatically restart the live XeTeX worker because the worker itself
may still be healthy.

Memory returned through `error_utf8` belongs to the DLL and must be freed with
`stemtex_renderer_free_string`.

## Environment Isolation

The renderer launches `xetexdaemon.exe` and `xdvipdfmxdaemon.exe` with a StemTeX-local
environment. It sets paths such as:

```text
PATH
TEXMFROOT
TEXMFCNF
TEXFORMATS
FONTCONFIG_PATH
XE_FONTCONFIG_PATH
FC_CACHEDIR
XE_FC_CACHEDIR
ICU_DATA
```

It also clears or overrides common inherited TeX variables so a host application's
global TeX Live or old StemTeX install does not leak into the render.

## Smoke Tests

The smoke executable supports these cases:

```text
stemtex-renderer-smoke.exe <repo-root> <runtime-root> <runs>
stemtex-renderer-smoke.exe <repo-root> <runtime-root> 1 --physics
stemtex-renderer-smoke.exe <repo-root> <runtime-root> 1 --fonts
stemtex-renderer-smoke.exe <repo-root> <runtime-root> 1 --chem-text
stemtex-renderer-smoke.exe <repo-root> <runtime-root> 1 --bad
stemtex-renderer-smoke.exe <repo-root> <runtime-root> 2 --bad-then-good
```

`--bad` is expected to fail quickly. It verifies the TeX-error path instead of
PDF output.

`--bad-then-good` first sends a bad snippet and then immediately sends a good
snippet. The second render should succeed through the promoted spare worker
without cold-start latency.

## Timing Report

Run the timing report from MSYS2:

```bash
cd /c/Users/jairy/Documents/xetex/stemtex
./scripts/run-cpp-timing-report.sh 'C:\StemTeX'
```

The script writes:

```text
out/timing/cpp-renderer-YYYYMMDD-HHMMSS/summary.json
out/timing/cpp-renderer-YYYYMMDD-HHMMSS/report.md
out/timing/cpp-renderer-YYYYMMDD-HHMMSS/raw/*.log
```

The report covers:

- cold create/warmup/live-worker startup;
- five hot default renders;
- physics, font, and chemistry representative snippets;
- the expected bad-snippet error path;
- the hot-spare failover path.
