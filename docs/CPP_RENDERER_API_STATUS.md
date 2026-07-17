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
- PDF/SVG output, including in-memory bytes for either format;
- configuration validation;
- font-cache refresh without calling PowerShell or batch scripts;
- async render callback on a DLL-owned worker thread.

## Current Contract

- One renderer uses one primary XeTeX worker for actual rendering.
- Concurrent render calls on one renderer are serialized by the renderer.
- Spare workers are failover capacity only; they are not a throughput pool.
- Async render is a PDF convenience wrapper around the same serialized render
  path. SVG callers use the generic synchronous output API from their own worker
  thread.
- Body-level TeX errors are recovered in the live XeTeX worker through the
  source-level checkpoint path described in
  `docs/XETEX_CHECKPOINT_RECOVERY.md`. The renderer waits for the restored worker
  to emit the next `WORKER_WAIT` marker before returning
  `STEMTEX_ERROR_TEX_SNIPPET`.
- `stemtex_renderer_engine_snapshot` is a synchronous cached-state read. It does
  not take the render lock and should be the single source of truth for GUI
  status indicators.
- Render results now carry a normalized outcome code separate from the base
  `StemTeXErrorCode`. A successful render can return
  `STEMTEX_RENDER_OUTCOME_RECOVERABLE` with `issue_flags != 0`; hosts should show
  the PDF but surface the warning to users. Failed render calls update the same
  outcome channel through `stemtex_renderer_last_outcome_code`,
  `stemtex_renderer_last_issue_flags`, and
  `stemtex_renderer_last_outcome_message`.
- `spare_worker_count = 0` now means no hot spares. Ordinary snippet errors do
  not require spare workers because the live worker can restore to its request
  loop in place.
- Output conversion uses a renderer-maintained cumulative XDV page counter
  rather than the TeX loop request number. After checkpoint restore the loop
  marker can return to `WORKER_WAIT:2`, while the XDV stream may already contain
  later successful pages.
- Cancellation kills the active worker and makes that active render fail with
  `STEMTEX_ERROR_CANCELLED`; queued work continues after failover/rebuild.
- Default request and `xdvipdfmx` timeouts are both 90000 ms. SVG conversion is
  currently covered by the render request path and reports `dvisvgmMs` in the
  summary.

## Remaining Product Work

- Evaluate a persistent renderer helper process for fault isolation around
  startup/dependency-level failures. The source-level checkpoint path covers
  body-level TeX errors, but it cannot stop a TeX Live dependency from calling
  `exit()` during initialization.
- Add a stable host-language binding once the embedding language is known.
- Decide whether `auto_restart` needs a real disabled mode; zero-initialized
  configs currently keep automatic recovery enabled.
- Decide whether callers need fixed output filenames. The current API returns a
  generated output path and optional in-memory bytes.
- Add long-running stress tests around repeated cancel/restart cycles.
