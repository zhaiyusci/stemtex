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

The built-in worker template now places a checkpoint at the top of each request
loop:

```tex
\def\workerloop{%
  \special{stemtex:checkpoint}%
  \advance\snippetcount by 1
  \typeout{WORKER_WAIT:\the\snippetcount}%
  \read16 to\snippetHsize
  ...
}
```

`stemtex:checkpoint` is intercepted inside XeTeX's generated C source. It is not
written into the XDV stream. Instead, it snapshots the TeX state that matters for
returning to the worker loop.

When a snippet later triggers a TeX error or a prompt path, XeTeX prints:

```text
STEMTEX_RESTORED
```

Then it restores the checkpoint and long-jumps back to the long-lived
`maincontrol()` call. The renderer waits until it sees the following
`WORKER_WAIT:N` before reporting the snippet failure to the caller. This is the
important synchronization point: `STEMTEX_RESTORED` alone is only a restore
signal; the next `WORKER_WAIT` proves that XeTeX is back at the request loop and
ready for the next stdin pair.

## Captured State

The checkpoint currently saves and restores:

- main memory, eqtb, hash table, save stack, nest stack, input stack, parameter
  stack, if/group stacks;
- string pool and string-start table;
- current input/scanner state, grouping state, conditionals, paragraph tokens,
  interaction mode, and error counters;
- allocator/hash/string scalar state such as `lomemmax`, `himemmin`,
  `hashused`, `poolptr`, and `strptr`.

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

Because the worker can recover in place, `spare_worker_count = 0` is now a valid
configuration and means no hot spare workers. Spare workers remain supported as
failover capacity for process loss, cancellation, or explicit restart, but they
are no longer required for ordinary snippet errors.

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

The current smoke coverage was run against the staged runtime with
`SPARES=0`, except where noted:

- `quick`: validate, default rendering, async, recover-no-worker;
- content cases: `physics`, `fonts`, `chem-text`, `bytes`, `latin-math`,
  `latin-text`;
- `bad-corpus`: 20 malformed snippets, each immediately followed by a good
  recovery probe;
- `errors`: repeated bad snippets plus cancellation;
- lifecycle cases: `restart`, `bad-then-good`, `bad-then-good-wait`, and
  `bad-then-good-wait-long`;
- `quick` with `SPARES=2`, to confirm the older spare-worker path still works.

The most important regression test is:

```sh
BUILD=0 SPARES=0 RUNS=1 TIMEOUT=240 \
  ./scripts/smoke-cpp-renderer.sh case bad-corpus
```

Expected result:

```text
badCorpus passed=20 failed=0 total=20
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
