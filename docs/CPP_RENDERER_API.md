# C++ Renderer API

This document describes the native DLL-style interface in `cpp-daemon/`.

The C++ layer is intentionally exposed as a small C ABI so host applications can
load it like a normal Windows DLL without using a network protocol.

## Build Outputs

The current CMake build emits:

```text
dist/cpp-daemon/build/stemtex-renderer.dll
dist/cpp-daemon/build/stemtex-renderer.lib
dist/cpp-daemon/build/stemtex-renderer-smoke.exe
```

The public header is:

```text
cpp-daemon/stemtex_renderer.h
```

## Runtime Model

`stemtex-renderer.dll` owns one live XeTeX worker process.

On create, it:

1. Reads the configured StemTeX runtime.
2. Runs a warmup worker using `cache-warmup/warmup.tex`.
3. Extracts XDV font definitions from the warmup output.
4. Starts a live `xetexdaemon.exe` worker with the fixed web worker template.
5. Primes the live worker once so later requests are hot.

On render, it:

1. Writes the snippet body to a request file.
2. Sends width and request path to the live worker.
3. Waits for `WORKER_DONE:N`.
4. Reads the current cumulative live XDV.
5. Synthesizes a valid final XDV postamble.
6. Calls `xdvipdfmx -s N-N` to convert only the newest page.
7. Returns the PDF path and a JSON timing summary.

The current conversion path is deliberately conservative: cumulative XDV plus
latest-page conversion. The earlier standalone-delta XDV experiment is not part
of this API.

## Public API

```cpp
typedef struct StemTeXConfig {
  const char *repo_root_utf8;
  const char *runtime_root_utf8;
  const char *state_root_utf8;
  const char *renders_root_utf8;
} StemTeXConfig;
```

Fields:

- `repo_root_utf8`: repository/resource root. The renderer expects to find
  `webapp/worker-webapp.tex` and `test/preamble.tex` under this tree.
- `runtime_root_utf8`: StemTeX runtime root, for example `C:\StemTeX`.
- `state_root_utf8`: optional worker state directory. If null, the renderer
  uses `out/cpp-renderer-state` under the repo root.
- `renders_root_utf8`: optional render output directory. If null, the renderer
  uses `out/cpp-renderer-renders` under the repo root.

Create a renderer:

```cpp
StemTeXRenderer *stemtex_renderer_create(
  const StemTeXConfig *config,
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
  char **error_utf8
);
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
void stemtex_renderer_free_string(char *value);
void stemtex_renderer_destroy(StemTeXRenderer *renderer);
```

## Minimal Host Example

```cpp
#include "stemtex_renderer.h"

#include <cstdio>

int main() {
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = "C:\\Users\\jairy\\Documents\\xetex\\xetex-live-worker";
  cfg.runtime_root_utf8 = "C:\\StemTeX";

  char *error = nullptr;
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &error);
  if (!renderer) {
    std::fprintf(stderr, "create failed: %s\n", error ? error : "");
    stemtex_renderer_free_string(error);
    return 1;
  }

  StemTeXRenderResult result{};
  if (!stemtex_renderer_render(renderer, u8"中文 $E=mc^2$", 360, &result, &error)) {
    std::fprintf(stderr, "render failed: %s\n", error ? error : "");
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

The renderer clamps invalid or extreme values internally:

```text
180pt <= width <= 430pt
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
  "workerRequest": 2
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

After a worker failure or request timeout, the renderer schedules a background
restart. A request that arrives while restart is in progress may fail with:

```text
Renderer is restarting after a previous TeX error
```

If `xdvipdfmx` fails after TeX has already produced XDV, the renderer returns
failure and includes the converter stdout/stderr tail in `error_utf8`. This path
does not automatically restart the live XeTeX worker because the worker itself
may still be healthy.

Memory returned through `error_utf8` belongs to the DLL and must be freed with
`stemtex_renderer_free_string`.

## Environment Isolation

The renderer launches `xetexdaemon.exe` and `xdvipdfmx.exe` with a StemTeX-local
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
```

`--bad` is expected to fail quickly. It verifies the TeX-error path instead of
PDF output.

## Timing Report

Run the timing report from MSYS2:

```bash
cd /c/Users/jairy/Documents/xetex/xetex-live-worker
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
- the expected bad-snippet error path.
