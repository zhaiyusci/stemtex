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
of hot spare workers. The default configuration keeps only the primary worker;
hosts may opt into hot spares by passing a positive `spare_worker_count`:

- `primary`: handles normal render requests.
- `spare-N`: stays hot as an immediate failover target.

On create, it:

1. Reads the configured StemTeX runtime.
2. Loads XDV font definitions for the selected profile. The renderer reuses a
   valid `profile_root\warmup.xdv` cache when present. If the cache is missing
   or unreadable, it compiles `warmup.tex` in the renderer instance state
   directory and does not update the profile cache.
3. Starts and primes a primary live worker.
4. If requested, starts building spare live workers in the background.

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
7. Converts only the newest page with the selected backend:
   `xdvipdfmxdaemon` for PDF or `dvisvgmdaemon` for SVG.
8. Returns the output path and a JSON timing summary.

The current conversion path is deliberately conservative: cumulative XDV plus
latest-page conversion. The earlier standalone-delta XDV experiment is not part
of this API.

The `preview` package owns the tight page box. PDF output gets that box through
xdvipdfmx's `pdf:pagesize` handling, and SVG output asks dvisvgm for
`--bbox=papersize` so it uses the same XDV page-size special. The GUI may still
crop the rendered bitmap for display convenience, but the emitted PDF and SVG
page boxes both include the configured preview border.

## Public API

```cpp
typedef struct StemTeXConfig {
  const char *repo_root_utf8;
  const char *runtime_root_utf8;
  const char *texmf_root_utf8;
  const char *profile_root_utf8;
  const char *state_root_utf8;
  const char *renders_root_utf8;
  int request_timeout_ms;
  int xdvipdfmx_timeout_ms;
  double min_width_pt;
  double max_width_pt;
  double default_width_pt;
  int spare_worker_count;
  int auto_restart;
  int delete_intermediates;
  const char *worker_template_utf8;
} StemTeXConfig;
```

Fields:

- `repo_root_utf8`: optional working directory for relative TeX inputs inside
  snippets. It is not used for renderer resource discovery.
- `runtime_root_utf8`: StemTeX runtime root, meaning the directory that directly
  contains `bin\windows\stemtex-worker-host.exe` and `bin\windows\xetexdaemon.exe`. In the installer layout this is
  normally `C:\StemTeX\runtime`.
- `texmf_root_utf8`: optional TeX Live tree used for packages and TeX fonts.
  If null, it defaults to `runtime_root_utf8`. When set, the path must be a
  TeX Live-style root that contains `texmf-dist` and `texmf-dist\web2c`, such as
  `C:\texlive\2026`. The renderer still runs patched binaries, formats,
  fontconfig configuration, and cache from `runtime_root_utf8`; this field
  redirects `TEXMFROOT`, `TEXMFDIST`, `TEXMFCNF`, and `WEB2C`. It does not switch
  to a user-provided TeX engine, and it does not support MiKTeX roots.
- `profile_root_utf8`: required profile directory. It must directly contain
  `preamble.tex` and `warmup.tex`. The renderer does not guess a default
  profile.
- `state_root_utf8`: optional worker state base directory. Each renderer
  instance creates and later removes its own unique child directory there. If
  null, the renderer uses a unique directory under the system temporary
  directory.
- `renders_root_utf8`: optional render output directory. If null, the renderer
  uses a unique directory under the system temporary directory.
- `request_timeout_ms`: startup, warmup, and active-render request timeout.
  `0` uses `90000`. This is not an idle timeout; a ready worker can wait
  indefinitely for the next render request.
- `xdvipdfmx_timeout_ms`: PDF conversion timeout. `0` uses `90000`.
- `default_width_pt`: width used when a render call passes `width_pt <= 0`.
  `0` uses `360`. Fractional point values are accepted.
- `min_width_pt`, `max_width_pt`: retained in the ABI for host-side policy, but
  the renderer no longer clamps `width_pt`.
- `spare_worker_count`: number of hot spare workers to maintain. `0` means no
  hot spare workers. Positive values are clamped internally; the current maximum
  is `4`.
- `auto_restart`: reserved policy flag; zero-initialized configs keep automatic
  recovery enabled.
- `delete_intermediates`: delete request/XDV intermediates after successful
  render while keeping PDF and summary.
- `worker_template_utf8`: optional worker template override. Leave this null for
  the built-in live XeTeX worker template.

### TeX Distribution Boundary

The external `texmf_root_utf8` option is deliberately narrow: it lets StemTeX's
patched TeX Live-derived daemon read package/font data from another TeX Live
installation. The selected tree supplies kpathsea configuration and data files;
the running binaries still come from the StemTeX runtime.

MiKTeX is not accepted as a `texmf_root_utf8` value. It has a different
multi-root and FNDB model, and StemTeX does not query MiKTeX Core, invoke
MiKTeX package installation, or run MiKTeX's `xelatex.exe`.

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
  double width_pt,
  StemTeXRenderResult *result,
  StemTeXErrorCode *error_code,
  char **error_utf8
);
```

The original `stemtex_renderer_render` API is the PDF convenience path. New
hosts that need selectable output should use the generic output API:

```cpp
typedef enum StemTeXOutputFormat {
  STEMTEX_OUTPUT_PDF = 0,
  STEMTEX_OUTPUT_SVG = 1
} StemTeXOutputFormat;

typedef struct StemTeXRenderOutputResult {
  char *request_id_utf8;
  char *output_path_utf8;
  char *output_format_utf8;
  char *summary_json_utf8;
  StemTeXRenderOutcomeCode outcome_code;
  int issue_flags;
  char *outcome_message_utf8;
} StemTeXRenderOutputResult;

typedef struct StemTeXOutputBytes {
  unsigned char *data;
  size_t size;
} StemTeXOutputBytes;

int stemtex_renderer_render_output(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  double width_pt,
  StemTeXOutputFormat format,
  StemTeXRenderOutputResult *result,
  StemTeXErrorCode *error_code,
  char **error_utf8
);

int stemtex_renderer_render_output_bytes(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  double width_pt,
  StemTeXOutputFormat format,
  StemTeXOutputBytes *bytes,
  StemTeXRenderOutputResult *result,
  StemTeXErrorCode *error_code,
  char **error_utf8
);
```

Related APIs:

```cpp
int stemtex_renderer_render_pdf_bytes(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  double width_pt,
  StemTeXPdfBytes *pdf,
  StemTeXRenderResult *result,
  StemTeXErrorCode *error_code,
  char **error_utf8
);

int stemtex_renderer_render_async(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  double width_pt,
  uint64_t *job_id,
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
StemTeXRenderOutcomeCode stemtex_renderer_last_outcome_code(StemTeXRenderer *renderer);
int stemtex_renderer_last_issue_flags(StemTeXRenderer *renderer);
char *stemtex_renderer_last_outcome_message(StemTeXRenderer *renderer);
char *stemtex_renderer_get_log_tail(StemTeXRenderer *renderer, int max_bytes);
const char *stemtex_renderer_version(void);
const char *stemtex_renderer_abi_version(void);
char *stemtex_renderer_runtime_version(StemTeXRenderer *renderer);
char *stemtex_renderer_profile_info_json(const char *profile_root_utf8,
                                         StemTeXErrorCode *error_code, char **error_utf8);
int stemtex_renderer_validate_config(const StemTeXConfig *config, StemTeXErrorCode *error_code, char **diagnostics_utf8);
int stemtex_refresh_font_cache(const char *runtime_root_utf8, const char *profile_root_utf8,
                               StemTeXErrorCode *error_code, char **error_utf8);
```

`stemtex_renderer_profile_info_json` parses one profile directory. Host
applications may decide where to look for profile candidates, such as
`repo_root\gui\profiles` or `gui_dir\profiles`; the renderer library owns the
rules for what is inside a valid profile. The returned string is JSON:

```json
{
  "name": "unicodemath_cjk",
  "path": "C:\\StemTeX\\gui\\profiles\\unicodemath_cjk",
  "valid": true,
  "hasPreamble": true,
  "hasWarmup": true,
  "hasWarmupXdv": false
}
```

The GUI scans profile candidate folders, then calls this library API for each
candidate instead of reimplementing profile validation.

`stemtex_renderer_render_async` returns a monotonically increasing `job_id`.
The callback receives the same id, so hosts can associate a completion with the
input version that created it.

The async API currently uses `StemTeXRenderResult` and is therefore PDF-only.
Hosts that need SVG should call `stemtex_renderer_render_output` from their own
worker thread and display only the newest UI request.

Async rendering is latest-only for work that has not started yet. If a pending
async job is superseded by a newer submission, its callback is still invoked
with `STEMTEX_ERROR_CANCELLED` and the message `Async render superseded by a
newer request`. A job that has already started is allowed to finish; hosts that
only care about live preview should display only the callback whose `job_id`
matches the latest submitted id.

`stemtex_renderer_engine_snapshot` is the status API intended for UI indicators.
It reads a cached snapshot maintained by the renderer and does not take the
render lock. The snapshot includes the renderer status, current stage
(`idle`, `queued`, `typesetting`, `converting`, `rebuilding`, or `stopping`),
primary/spare readiness, spare rebuild state, and the running/pending async job
ids. Hosts should use this snapshot as the single source of truth for status
lights instead of inferring readiness from render callbacks or summary JSON.

Render result:

```cpp
typedef enum StemTeXRenderOutcomeCode {
  STEMTEX_RENDER_OUTCOME_OK = 0,
  STEMTEX_RENDER_OUTCOME_RECOVERABLE = 1,
  STEMTEX_RENDER_OUTCOME_INVALID_ARGUMENT = 100,
  STEMTEX_RENDER_OUTCOME_BAD_CONFIG = 101,
  STEMTEX_RENDER_OUTCOME_WORKER_STARTUP = 102,
  STEMTEX_RENDER_OUTCOME_WORKER_TIMEOUT = 103,
  STEMTEX_RENDER_OUTCOME_WORKER_RESTARTING = 104,
  STEMTEX_RENDER_OUTCOME_WORKER_BUSY = 105,
  STEMTEX_RENDER_OUTCOME_TEX_SNIPPET = 106,
  STEMTEX_RENDER_OUTCOME_XDVIPDFMX = 107,
  STEMTEX_RENDER_OUTCOME_CANCELLED = 108,
  STEMTEX_RENDER_OUTCOME_FILESYSTEM = 109,
  STEMTEX_RENDER_OUTCOME_INTERNAL = 110,
  STEMTEX_RENDER_OUTCOME_DVISVGM = 111
} StemTeXRenderOutcomeCode;

typedef struct StemTeXRenderResult {
  char *request_id_utf8;
  char *pdf_path_utf8;
  char *summary_json_utf8;
  StemTeXRenderOutcomeCode outcome_code;
  int issue_flags;
  char *outcome_message_utf8;
} StemTeXRenderResult;
```

`StemTeXErrorCode` reports whether the API call itself succeeded. Successful
calls may still produce a PDF with recoverable rendering issues. Hosts should
therefore check `StemTeXRenderResult::outcome_code` after a successful render:

- `STEMTEX_RENDER_OUTCOME_OK`: PDF was generated without known recoverable
  renderer issues.
- `STEMTEX_RENDER_OUTCOME_RECOVERABLE`: PDF was generated, but the renderer saw
  a recoverable issue. The current `issue_flags` value `1` means xdvipdfmx saw a
  missing/undefined font reference and omitted affected output.
- Values `100` and above mirror fatal API/render failures. If a synchronous
  render call returns `0`, no `StemTeXRenderResult` is available; call
  `stemtex_renderer_last_outcome_code`, `stemtex_renderer_last_issue_flags`, and
  `stemtex_renderer_last_outcome_message` on the renderer to read the same
  normalized outcome channel.

The timing summary JSON repeats the same high-level fields as
`outcomeCode`, `issueFlags`, and `outcomeMessage`. For xdvipdfmx specifically it
also includes `xdvipdfmxReturnCode`, `xdvipdfmxIssueFlags`,
`xdvipdfmxIssueMessage`, and `xdvipdfmxWarning`.

The bundled Qt GUI is a reference consumer of this contract: it lets users
choose PDF or SVG output, previews both, and when it receives a recoverable
outcome it still displays the generated preview while showing a warning marker
and the outcome fields in the details text.

Cleanup:

```cpp
void stemtex_renderer_free_result(StemTeXRenderResult *result);
void stemtex_renderer_free_pdf_bytes(StemTeXPdfBytes *pdf);
void stemtex_renderer_free_output_result(StemTeXRenderOutputResult *result);
void stemtex_renderer_free_output_bytes(StemTeXOutputBytes *bytes);
void stemtex_renderer_free_string(char *value);
void stemtex_renderer_destroy(StemTeXRenderer *renderer);
```

## Minimal Host Example

```cpp
#include "stemtex_renderer.h"

#include <cstdio>

int main() {
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = "C:\\path\\to\\stemtex";
  cfg.runtime_root_utf8 = "C:\\StemTeX\\runtime";
  cfg.texmf_root_utf8 = "C:\\texlive\\2026";
  cfg.profile_root_utf8 = "C:\\StemTeX\\gui\\profiles\\unicodemath_cjk";

  char *error = nullptr;
  StemTeXErrorCode error_code = STEMTEX_OK;
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &error_code, &error);
  if (!renderer) {
    std::fprintf(stderr, "create failed code=%d: %s\n", (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return 1;
  }

  StemTeXRenderResult result{};
  if (!stemtex_renderer_render(renderer, u8"中文 $E=mc^2$", 360.0, &result, &error_code, &error)) {
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
The C API sends positive `width_pt` values to TeX unchanged, including
fractional point values such as `360.5pt`. GUI frontends may still impose their
own control ranges.
```

If `width_pt <= 0`, the renderer uses `360pt`.

## Summary JSON

`summary_json_utf8` currently contains fields like:

```json
{
  "outputFormat": "pdf",
  "outputPath": "C:\\...\\snippet-1.pdf",
  "renderMode": "cpp-dll-live-worker-latest-page",
  "backend": "xdvipdfmxdaemon",
  "converterMode": "daemon-dll",
  "widthPt": 360.5,
  "requestToOutputMs": 203,
  "requestToPdfMs": 203,
  "requestToSvgMs": 0,
  "finalizeXdvMs": 2,
  "convertMs": 124,
  "xdvipdfmxMs": 124,
  "dvisvgmMs": 0,
  "cumulativeXdvBytes": 11519,
  "newXdvBytes": 1153,
  "finalXdvBytes": 12352,
  "outputBytes": 26111,
  "pdfBytes": 26111,
  "svgBytes": 0,
  "workerRequest": 2,
  "workerSlot": "primary",
  "spareReady": 1,
  "spareTarget": 1,
  "spareRebuilding": false
}
```

The stable high-level timings are:

- `requestToPdfMs`: total hot render time for this request.
- `requestToSvgMs`: total hot render time for SVG requests.
- `requestToOutputMs`: total hot render time independent of output format.
- `convertMs`: selected backend conversion time.
- `xdvipdfmxMs`: PDF conversion time.
- `dvisvgmMs`: SVG conversion time.
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

If primary and all spares are unavailable, the renderer creates a new primary
synchronously. That degraded path can pay cold-start latency.

If `xdvipdfmx` fails after TeX has already produced XDV, the renderer returns
failure and includes the converter stdout/stderr tail in `error_utf8`. This path
does not automatically restart the live XeTeX worker because the worker itself
may still be healthy.

Memory returned through `error_utf8` belongs to the DLL and must be freed with
`stemtex_renderer_free_string`.

## Environment Isolation

The renderer launches `xetexdaemon.exe` through `stemtex-worker-host.exe` for
warmup compilation, profile-cache refresh, and live XeTeX workers. The helper
starts the daemon with the renderer's stdio pipes and a private lifetime pipe.
For live workers, the helper blocks indefinitely while the worker is idle. It
cleans up `xetexdaemon` when the renderer closes the lifetime pipe during
destroy, restart, cancellation, timeout, or host-process death.

The renderer and helper use a StemTeX-local environment. It sets paths such as:

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

Build the smoke executable through CMake, then pass repo/runtime/profile paths
explicitly.  The smoke executable accepts named options only; test cases are
selected with `--case NAME`.

```powershell
cmake --build --preset renderer-release

$repo = (Get-Location).Path
$smoke = ".\build\stemtex-renderer\cpp-daemon\Release\stemtex-renderer-smoke.exe"
$runtime = ".\dist\stemtex-texlive-daemon-static"
$profile = ".\gui\profiles\unicodemath_cjk"

& $smoke --repo $repo --runtime $runtime --profile $profile --case validate
& $smoke --repo $repo --runtime $runtime --profile $profile --runs 2
& $smoke --repo $repo --runtime $runtime --profile $profile --case physics
& $smoke --repo $repo --runtime $runtime --profile $profile --case bad-corpus --spares 0
```

Useful options:

```text
--repo PATH              Source tree root.
--runtime PATH           Runtime tree containing bin/windows and texmf-var.
--texmf PATH             Optional external TeX Live tree for packages/fonts.
--profile PATH           Profile directory containing preamble.tex and warmup.tex.
--runs N                 Number of repeated renders for applicable cases.
--case NAME              Test case such as validate, physics, bad-corpus, async.
--spares N               Hot spare target for the renderer.
--worker-template PATH   Optional worker-state template.
--allow-exe              Allow xdvipdfmx process fallback during diagnostics.
```

Examples:

```text
stemtex-renderer-smoke.exe --repo PATH --runtime PATH --profile PROFILE --case physics --spares 2
stemtex-renderer-smoke.exe --repo PATH --runtime PATH --profile PROFILE --case async --runs 5 --spares 2
```

For timing checks, run representative cases with `--runs` and capture the
console output from the smoke executable. Useful cases are:

- cold create/warmup/live-worker startup;
- physics, font, and chemistry representative snippets;
- the expected bad-snippet error path;
- the hot-spare failover path.
