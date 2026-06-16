# C++ Renderer API TODO

The current native API is enough for the prototype:

- create one renderer;
- keep one XeTeX worker hot;
- render one snippet synchronously;
- return a PDF path plus timing JSON;
- report errors as UTF-8 strings;
- destroy the renderer.

For an embeddable StemTeX component, the API should grow in a few focused
directions. The target shape is still small:

```text
create / status / render / restart / diagnostics / destroy
```

## Must Have Before Product Integration

### Error Codes

Current state: errors are returned only as strings.

Problem: host software cannot reliably branch on error type.

Add a stable error enum:

```cpp
typedef enum StemTeXErrorCode {
  STEMTEX_OK = 0,
  STEMTEX_ERROR_BAD_CONFIG,
  STEMTEX_ERROR_WORKER_STARTUP,
  STEMTEX_ERROR_WORKER_TIMEOUT,
  STEMTEX_ERROR_WORKER_RESTARTING,
  STEMTEX_ERROR_TEX_SNIPPET,
  STEMTEX_ERROR_XDVIPDFMX,
  STEMTEX_ERROR_FILESYSTEM,
  STEMTEX_ERROR_INTERNAL
} StemTeXErrorCode;
```

Possible API shapes:

```cpp
StemTeXErrorCode stemtex_renderer_last_error_code(StemTeXRenderer *renderer);
```

or add the code to every failing call through an output parameter.

### Renderer Status

Current state: the host learns status only by calling `render`.

Problem: UI cannot distinguish ready, restarting, dead, or starting.

Add:

```cpp
typedef enum StemTeXRendererStatus {
  STEMTEX_STATUS_STARTING,
  STEMTEX_STATUS_READY,
  STEMTEX_STATUS_RENDERING,
  STEMTEX_STATUS_RESTARTING,
  STEMTEX_STATUS_DEAD
} StemTeXRendererStatus;

StemTeXRendererStatus stemtex_renderer_status(StemTeXRenderer *renderer);
```

### Explicit Restart

Current state: worker restart is scheduled automatically after a TeX failure or
timeout.

Problem: the host cannot deliberately restart after changing runtime files,
recovering from a known bad state, or preparing for a new interaction session.

Add:

```cpp
int stemtex_renderer_restart(StemTeXRenderer *renderer, char **error_utf8);
```

This should stop the current worker, rerun the live warmup/prime step, and return
only when the renderer is ready or has failed.

### Log Tail / Diagnostics

Current state: error strings include a TeX output tail only on some failures.

Problem: host software cannot show diagnostics after a warning, slow render, or
unexpected behavior that did not hard-fail.

Add:

```cpp
char *stemtex_renderer_get_log_tail(StemTeXRenderer *renderer, int max_bytes);
```

The returned string should be freed with `stemtex_renderer_free_string`.

### Threading Contract

Current state: implementation serializes render calls internally, but the public
contract is not documented as stable.

Document and enforce:

- one renderer owns one live worker;
- `stemtex_renderer_render` is synchronous;
- concurrent render calls on one renderer are serialized or rejected;
- multiple renderer instances are allowed only if the host accepts multiple
  XeTeX worker processes and separate state directories.

## Should Have

### Richer Configuration

Current state:

```cpp
typedef struct StemTeXConfig {
  const char *repo_root_utf8;
  const char *runtime_root_utf8;
  const char *state_root_utf8;
  const char *renders_root_utf8;
} StemTeXConfig;
```

Useful additions:

```cpp
int request_timeout_ms;
int xdvipdfmx_timeout_ms;
int min_width_pt;
int max_width_pt;
int default_width_pt;
int keep_intermediates;
int auto_restart;
const char *warmup_tex_utf8;
const char *worker_template_utf8;
const char *preamble_tex_utf8;
```

Need versioning for the config struct before extending it:

```cpp
uint32_t struct_size;
uint32_t api_version;
```

That lets future DLLs accept older host structs safely.

### Version Queries

Add:

```cpp
const char *stemtex_renderer_version(void);
const char *stemtex_renderer_abi_version(void);
char *stemtex_renderer_runtime_version(StemTeXRenderer *renderer);
```

The returned runtime version can read StemTeX `VERSION` and optionally include
the patched engine identity.

### PDF Bytes Output

Current state: render returns a PDF path.

Problem: some host applications prefer in-memory bytes and do not want to reopen
the file themselves.

Possible API:

```cpp
typedef struct StemTeXPdfBytes {
  unsigned char *data;
  size_t size;
} StemTeXPdfBytes;

int stemtex_renderer_render_pdf_bytes(
  StemTeXRenderer *renderer,
  const char *snippet_utf8,
  int width_pt,
  StemTeXPdfBytes *pdf,
  StemTeXRenderResult *result,
  char **error_utf8
);

void stemtex_renderer_free_pdf_bytes(StemTeXPdfBytes *pdf);
```

### Config Validation

Add a cheap validation call:

```cpp
int stemtex_renderer_validate_config(
  const StemTeXConfig *config,
  char **diagnostics_utf8
);
```

This should check:

- runtime root exists;
- `xetexdaemon.exe` exists;
- `xdvipdfmx.exe` exists;
- `xelatex.fmt` exists;
- `cache-warmup/warmup.tex` exists;
- required worker template and preamble files exist;
- fontconfig and ICU paths are plausible.

## Nice To Have

### Warmup / Cache Refresh API

Current installation path builds fontconfig cache by running the StemTeX runtime
warmup script.

Future API:

```cpp
int stemtex_refresh_font_cache(
  const char *runtime_root_utf8,
  const char *warmup_tex_utf8,
  char **error_utf8
);
```

This is optional because installers or host applications can still call the
existing runtime script.

### Async API

The synchronous API is simpler and appropriate for the first version. A host can
already run it on its own worker thread.

If needed later:

```cpp
typedef void (*StemTeXRenderCallback)(
  int ok,
  const StemTeXRenderResult *result,
  const char *error_utf8,
  void *user_data
);
```

Keep this optional until a real host needs it.

### Cancellation

Possible API:

```cpp
int stemtex_renderer_cancel_current(StemTeXRenderer *renderer);
```

This would probably kill and restart the live worker. It is useful for impatient
interactive UIs, but it complicates state management.

### Output Policy

Current render output is file-based under `renders_root`.

Later options:

- fixed output filename supplied by host;
- automatic cleanup of old render directories;
- in-memory PDF only;
- keep or delete intermediate XDV/log files.

## Suggested Implementation Order

1. Add API version/config struct sizing.
2. Add error codes.
3. Add status query.
4. Add explicit restart.
5. Add log tail.
6. Document and enforce threading behavior.
7. Add config validation.
8. Add optional PDF-bytes API.

The first five items are the most important because they turn the current
prototype API into something a host application can supervise reliably.
