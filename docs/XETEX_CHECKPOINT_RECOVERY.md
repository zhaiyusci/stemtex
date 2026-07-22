# XeTeX Checkpoint Recovery

This note records the current StemTeX live-worker error recovery path.

## Goal

The renderer should keep XeTeX hot for normal snippets, but a bad snippet must
not poison the worker or force the GUI to rely on process restarts. In
particular, ordinary body-level TeX errors should return a normal renderer error
code and leave the same live worker ready for the next request.

This is not a process-isolation mechanism. It handles errors after the fixed
preamble and worker loop are already running. Startup failures, kpathsea
initialization failures, and dependency-level `exit()` calls still belong to the
startup/restart/future-helper-process class of problems.

## Runtime Protocol

The built-in worker template takes a single checkpoint after the worker's first
warmup request has completed:

```tex
\typeout{WORKER_DONE:\the\snippetcount}%
\ifstemtexcheckpointed\else
  \stemtexcheckpointedtrue
  \special{stemtex:checkpoint}%
  \typeout{WORKER_BASELINE_READY}%
\fi
\errorstopmode
\workerloop
```

After that, the request loop continues normally:

```tex
\def\workerloop{%
  \advance\snippetcount by 1
  \typeout{WORKER_WAIT:\the\snippetcount}%
  \read16 to\snippetHsize
  \read16 to\stemtexfontsizeline
  \read16 to\requestfile
  ...
}
```

`stemtex:checkpoint` is intercepted inside XeTeX's generated C source. It is not
written into the XDV stream. Instead, it snapshots the TeX state that matters for
returning to the clean, post-preamble/post-warmup worker loop. The checkpoint is
not refreshed for every later request, so a successful snippet cannot become the
new baseline by accident.

When a snippet later triggers a TeX error or a prompt path, XeTeX prints:

```text
STEMTEX_RESTORED
```

Then it restores the checkpoint and long-jumps back to the long-lived
`maincontrol()` call. The renderer waits until it sees the following
`WORKER_WAIT:N` before reporting the snippet failure to the caller. This is the
important synchronization point: `STEMTEX_RESTORED` alone is only a restore
signal; the next `WORKER_WAIT` proves that XeTeX is back at the request loop and
ready for the next stdin request triple. The numeric suffix is diagnostic only; the C++
renderer does not require it to be monotonic after a checkpoint restore.

## Captured State Model

The checkpoint has two layers.

The first layer is the TeX user-state layer. It follows the inventory used by
XeTeX's `storefmtfile()`/`loadfmtfile()` path, but keeps the snapshot in memory
instead of writing a real `.fmt` file. This layer saves and restores:

- main memory, eqtb, and hash table;
- string pool and string-start table;
- allocator/hash/string scalar state such as `lomemmax`, `himemmin`,
  `hashused`, `poolptr`, and `strptr`;
- e-TeX sparse register roots used by format dumps, with the page/mark
  scratch root kept in the worker continuation layer;
- font state from the format-dump model: `fontinfo`, `fontptr`, `fmemptr`, font
  metric/base arrays, font names/areas, font flags, font mappings/layout-engine
  pointers, `fontused`, and `fontglue`; restored checkpoints also release
  native font engines and TECkit mappings from font slots that were created
  after the checkpoint and are being rolled back;
- hyphenation and trie state, including hyphen exceptions, trie arrays,
  trie-op arrays, `hyphcount`, `hyphnext`, `triemax`, and `trieopptr`.

This is intentionally not the same as calling `\dump`. Normal XeTeX format
dumping refuses native fonts and font mappings because a `.fmt` file must be
portable across process starts. StemTeX's checkpoint is process-local, so native
font/layout-engine pointers loaded during warmup can remain live and are shallow
copied as part of the in-process font table.

The second layer is the worker continuation layer. It saves and restores:

- save stack, nest stack, input stack, parameter stack, if/group stacks;
- current input/scanner state, grouping state, conditionals, paragraph tokens,
  interaction mode, and error counters;
- paragraph building, math-list conversion, line breaking, hyphenation, page
  builder, and e-TeX direction/last-line-fit scratch state.

The largest checkpoint regions (`mem`, `eqtb`, `hash`, and `fontinfo`) are
stored as block-based fill/raw snapshots. Each region is split into fixed-size
element blocks. A block whose elements are all byte-identical is stored as one
fill element; other blocks fall back to raw bytes. This keeps exact full-state
restore semantics without storing every large region byte-for-byte. Smaller
tables, stacks, and scalar state are copied directly.

Fill-block restore is optimized for the hot error-recovery path: all-zero fill
blocks use `memset`, and non-zero fill blocks are expanded by repeated doubling
copies instead of one element at a time.

The restore is intentionally scoped to body-level recovery. It does not attempt
to re-run or partially undo preamble initialization.

## Renderer Behavior

On a restored snippet error, the C++ renderer:

1. detects a TeX error in the request output;
2. waits for `STEMTEX_RESTORED` and then the next `WORKER_WAIT`;
3. updates the live XDV offset so bad-snippet output is not converted as a
   successful page;
4. returns `STEMTEX_ERROR_TEX_SNIPPET`;
5. keeps the primary worker ready for the next render.

The TeX loop request number is not used as the cumulative XDV page number after
an error restore, because the loop returns to the checkpoint and can emit the
same `WORKER_WAIT:N` again. The renderer keeps its own cumulative XDV page
counter and uses that counter for final XDV postambles and PDF/SVG page
selection.

Because the worker can recover in place, `spare_worker_count = 0` is now a valid
configuration and means no hot spare workers. Spare workers remain supported as
failover capacity for process loss, cancellation, or explicit restart, but they
are no longer required for ordinary snippet errors.

During startup, the renderer also waits for `WORKER_BASELINE_READY` after the
warmup request. This guarantees the reusable checkpoint exists before the worker
is reported as ready for user snippets.

Cancellation is separate from checkpoint recovery. A cancellation intentionally
kills the active worker and returns `STEMTEX_ERROR_CANCELLED`; the next render
starts or promotes a worker through the normal recovery path.

## What This Avoids

This path deliberately avoids:

- sending interactive `s`/scrollmode input from the renderer after an error;
- treating a quiet output period as proof of recovery;
- requiring a standby XeTeX process just to survive malformed snippet input;
- hiding snippet errors by padding profiles or warmup files.

Timeouts remain as a last-resort dead-worker guard, not as the normal error
recovery mechanism.

## Current Test Coverage

The current smoke coverage was run against the staged runtime after switching to
the single post-warmup checkpoint with fill/raw large snapshots. The most
relevant cases are:

- `bad-corpus` with `SPARES=0`: malformed snippets, each immediately followed
  by a good recovery probe;
- `checkpoint-critical` with `SPARES=0`: the smaller set of errors most likely
  to poison checkpoint restore, including math alphabet commands used outside
  math mode, font-dimension mutation, native font loading, e-TeX sparse
  register mutation, and hyphenation mutation;
- `bad-output-corpus` with `SPARES=0`: PDF and SVG output recovery after bad
  snippets;
- `list-state` with `SPARES=0`: repeated `itemize`/`enumerate` snippets and a
  malformed open list, verifying fragment-local list state returns to zero;
- `lifecycle-stress` with `SPARES=1`: repeated create/render/destroy cycles;
- `profile-switch-stress` with `SPARES=1`: alternating `unicodemath_cjk` and
  `unicodemath`;
- `quick` with `SPARES=1`: validate, default rendering, async, recover-no-worker.

The most important regression test is:

```powershell
cmake --build --preset ninja-release --target stemtex-renderer-smoke

$repo = (Get-Location).Path
$smoke = ".\build\stemtex-ninja\cpp-daemon\stemtex-renderer-smoke.exe"
$runtime = ".\dist\stemtex-installer\StemTeX\runtime"
$profile = ".\gui\profiles\unicodemath_cjk"

& $smoke --repo $repo --runtime $runtime --profile $profile --case bad-corpus --spares 0 --runs 1
& $smoke --repo $repo --runtime $runtime --profile $profile --case checkpoint-critical --spares 0 --runs 1
& $smoke --repo $repo --runtime $runtime --profile $profile --case bad-output-corpus --spares 0 --runs 1
& $smoke --repo $repo --runtime $runtime --profile $profile --case list-state --spares 0 --runs 1
```

Expected result:

```text
badCorpus passed=36 failed=0 total=36
badCorpus passed=13 failed=0 total=13
badOutput passed=28 failed=0 total=28
listState failed=0
```

## Known Boundaries

- A bad or missing preamble is still a worker startup problem.
- A dependency that calls `exit()` during initialization can still terminate its
  hosting process. That is outside this checkpoint mechanism.
- File-handle cleanup during restore has not been generalized. An experimental
  attempt to close newly opened TeX input files during restore disturbed the
  missing-file path, so it was not kept. The current validated behavior is that
  prompt-style missing-file and missing-image errors recover and can render the
  next good snippet.
- The checkpoint is implemented in generated C (`xetex0.c`/`xetexini.c`), so
  future TeX Live source refreshes must either preserve or intentionally re-port
  this patch.
