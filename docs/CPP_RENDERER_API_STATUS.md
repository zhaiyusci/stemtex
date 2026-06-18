# C++ Renderer API Status

The C++ renderer API now exposes the embeddable surface that was previously
tracked here as TODO:

- error codes on create/render/restart/cancel/validate/refresh;
- status query and last-error query;
- explicit restart;
- current-render cancellation;
- log-tail diagnostics;
- richer configuration, including timeouts, width policy, resource overrides,
  spare count, and intermediate-file cleanup;
- version and runtime-version queries;
- PDF-bytes output;
- configuration validation;
- font-cache refresh without calling PowerShell or batch scripts;
- async render callback on a DLL-owned worker thread.

## Current Contract

- One renderer uses one primary XeTeX worker for actual rendering.
- Concurrent render calls on one renderer are serialized by the renderer.
- Spare workers are failover capacity only; they are not a throughput pool.
- Async render is a convenience wrapper around the same serialized render path.
- `stemtex_renderer_engine_snapshot` is a synchronous cached-state read. It does
  not take the render lock and should be the single source of truth for GUI
  status indicators.
- Cancellation kills the active worker and makes that active render fail with
  `STEMTEX_ERROR_CANCELLED`; queued work continues after failover/rebuild.
- Default request and `xdvipdfmx` timeouts are both 90000 ms.

## Remaining Product Work

- Add a stable host-language binding once the embedding language is known.
- Decide whether `auto_restart` needs a real disabled mode; zero-initialized
  configs currently keep automatic recovery enabled.
- Decide whether callers need fixed output filenames. The current API returns a
  generated PDF path and optional in-memory bytes.
- Add long-running stress tests around repeated cancel/restart cycles.
