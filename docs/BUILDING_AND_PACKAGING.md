# Building And Packaging

This is the canonical procedure for the supported StemTeX application build,
staging tree, smoke tests, and Windows installer. It does not rebuild the
generated-C XeTeX daemon or its static third-party dependencies.

## Prerequisites

- Visual Studio 2022 with the x64 C++ toolchain;
- CMake 3.20 or newer;
- Qt 6.11 or newer with `Core`, `Gui`, `Widgets`, `Pdf`, `Svg`, and
  `LinguistTools`, including `qtbase_zh_CN.qm` for the Profile Creator;
- Python 3 for the checkpoint state audit;
- Inno Setup 6 when building the installer.

The checked-in presets currently point at the repository's maintained Qt and
Ninja locations. Override `CMAKE_PREFIX_PATH` or `CMAKE_MAKE_PROGRAM` during
configure when using a different local installation.

CMake install also consumes these prebuilt inputs by default:

```text
texlive-xetex/out/standalone-msvc
dist/stemtex-texlive-daemon-static
```

Use `STEMTEX_STANDALONE_DIR`, `STEMTEX_RUNTIME_SOURCE`, and
`STEMTEX_TEXLIVE_ROOT` to select different local inputs. See
[Windows engine rebuild notes](WINDOWS_XETEX_BUILD_NOTES.md) only when those
daemon/runtime inputs themselves must be regenerated.

## Configure And Build

Open an x64 Visual Studio developer environment, then use the Ninja preset:

```bat
cmake --preset ninja-msvc
cmake --build --preset ninja-release
```

The Visual Studio generator is also supported:

```bat
cmake --preset vs2022
cmake --build --preset release
```

For a renderer-only build without Qt:

```bat
cmake --preset renderer-ninja-msvc
cmake --build --preset renderer-ninja-release
```

These are CMake-driven application builds and do not require bash.

## Experimental LuaHBTeX GUI

The GUI also offers `LuaHBTeX (experimental)`. Build the patched engine with
`scripts/build-luahbtex-mingw.ps1`. At packaging time `STEMTEX_TEXLIVE_ROOT`
supplies the files for a private LuaLaTeX tree under `runtime/luatex`.
Installed users can render the bundled Latin Modern / `unicode-math` profile
with this tree alone. A profile with additional package or font dependencies
can use an explicitly selected external tree. PDF and SVG output use the same
resident `dvipdfmxdaemon.dll` and `dvisvgmdaemon.dll` as XeTeX.
The public renderer C ABI continues to use XeTeX.

After building the patched engine, configure and stage the GUI in an x64
Visual Studio environment:

```powershell
cmake --preset ninja-msvc -DSTEMTEX_INSTALL_EXPERIMENTAL_LUATEX=ON
cmake --build build/stemtex-ninja --target stage stemtex-luatex-gui-smoke
.\scripts\start-luatex-gui.ps1
```

The option defaults to OFF for ordinary developer stages. For release packaging,
the explicit `LuaHBTeX` install component below adds the engine to the stage
with either setting. It copies `luahbtex.exe` and its
DLLs and the matching `luahbtex.exe.stemtex-checkpoint` ownership map from
`STEMTEX_LUAHBTEX_DIR` to `runtime/bin/luatex`. The map is generated from the
linker's actual object layout and bound to the executable bytes; keep the pair
together. Use `-TexLiveRoot C:\texlive\2026` to select external packages for
a custom profile. The launch script can
also find the local build without staging that engine. Use `-EnginePath` or
`-StageRoot` to override those paths.

Alternatively, select the engine in the GUI toolbar, or launch with
`--engine luatex --luatex-engine C:/path/to/luahbtex.exe` before the existing
positional arguments (`repo runtime profile texmf`). The GUI initializes a
matching LuaLaTeX format and font cache outside the TeX Live installation;
first startup takes longer than subsequent starts.

Successful edits reuse a LuaHBTeX process for the selected output driver. Each shipout flushes DVI; the GUI
finalizes a copy and converts the newest page to PDF or SVG. Both converter DLLs
are initialized once and reused until the backend is closed; requests do not
start converter processes. The GUI displays typesetting, conversion, and total
time in the same format as XeTeX. After an internal warmup page, the engine saves
one joint TeX/Lua checkpoint. Ordinary malformed snippets restore that baseline
and return to the request loop in the same PID. Timeouts, cancellation, and
unsafe native-resource activity retain the worker-replacement fallback.
Profile Creator's engine selector
generates the appropriate CJK and DVI graphics configuration; the GUI lists
profiles compatible with the selected engine. Generated Lua profiles use a
separate lazy resident SVG worker because PGF's PDF and SVG drivers differ;
switching back to either format reuses its worker and the shared converters.

The optional backend smoke target checks actual PDF/SVG output, continued
same-process rendering, recovery after malformed input, cancellation, and
XeTeX/LuaHBTeX transitions in one host process, including environment restoration.
An optional sixth path argument selects a runtime other than `staging/runtime`.
Run it with the Qt `bin` directory on `PATH`:

```powershell
$repo = (Get-Location).Path
$env:PATH = "C:\Qt\6.11.1\msvc2022_64\bin;$env:PATH"
& .\build\stemtex-ninja\tests\stemtex-luatex-gui-smoke.exe `
  $repo C:\texlive\2026 `
  "$repo\build\luahbtex-mingw\bin\luahbtex.exe" `
  "$repo\gui\profiles\unicodemath" "$repo\build\luatex-gui-smoke"
```

The generated-profile smoke additionally checks common math packages, color,
graphics transforms, TikZ, and Fandol/available Windows CJK fonts in real PDF
and SVG outputs. Its combined 21-package profile also repeats traditional TFM
and virtual-font output, guarding the resident converter's font-cache cleanup.
It checks per-output worker reuse, SVG error isolation, and worker shutdown:

```powershell
cmake --build build/stemtex-ninja --target stemtex-luatex-profile-smoke
& .\build\stemtex-ninja\tests\stemtex-luatex-profile-smoke.exe `
  $repo "$repo\staging\runtime" C:\texlive\2026 `
  "$repo\build\luahbtex-mingw\bin\luahbtex.exe" "$repo\build\luatex-profile-smoke"
```

`stemtex-luatex-recovery-smoke` checks deliberately malformed snippets against
the builtin, generated Latin, and generated Chinese/math/graphics profiles.
It uses the GUI's 90-second timeout and records error detection, checkpoint
restore, fallback restart, and the next valid output separately. Ordinary cases
require same-PID recovery in PDF and SVG, including state-leak, immutable
published-output, and unaffected-worker checks:

```powershell
cmake --build build/stemtex-ninja --target stemtex-luatex-recovery-smoke
& .\build\stemtex-ninja\tests\stemtex-luatex-recovery-smoke.exe `
  $repo "$repo\staging\runtime" C:\texlive\2026 `
  "$repo\build\luahbtex-mingw\bin\luahbtex.exe" "$repo\build\luatex-recovery-tests"
```

An optional last argument selects `builtin`, `latin`, `rich-cjk`, or
`PROFILE/CASE`. `conditional` checks unfinished true conditionals;
`pollution` mutates pre-checkpoint Lua tables, shared upvalues, function registry,
callbacks, node properties, font metrics and TeX state before causing an error;
`nested` checks Lua-to-TeX error propagation through protected calls;
`default-timeout` deliberately waits the full 90 seconds for an infinite loop.
For stress runs, set `STEMTEX_RECOVERY_REPEATS` to 1–1000, for example 20 with
`builtin/directlua-error`; unset it afterwards to restore the normal matrix.
Results are saved incrementally as `recovery-smoke-results.json`, with an
additional copy under each unique run directory. Recovery timings start after
profile initialization and output warmup; first-time font database creation is
not included. JSON distinguishes `recoveryMode=checkpoint` from `restart`, and
records both the failed and resulting worker PID.

The native allocator/file/resource checks can run without MSYS or a full engine
build. They compile with MinGW warnings treated as errors and exercise repeated
memory, Lua-style object, file-position and DVI rollback, plus refusal to restore
after guarded native resource changes:

```powershell
& .\scripts\test-luatex-checkpoint-memory.ps1
```

The 13 scenarios use separate processes; the memory scenario restores ten times.
The exact-binary ownership sidecar is required when deploying the engine; do not
reuse one from a different build, even if the engine version string is identical.

## Create A Staging Tree

For the repository's normal developer stage:

```bat
cmake --build build\stemtex-ninja --target stage
```

The target installs into `staging/`. To install elsewhere:

```bat
cmake --install build\stemtex-ninja --prefix C:\path\to\StemTeX
```

With a multi-configuration Visual Studio build, include the configuration:

```bat
cmake --install build\stemtex --config Release --prefix C:\path\to\StemTeX
```

CMake install copies files but does not remove stale files from an existing
destination. Release packaging must therefore use a new or verified-empty stage
directory. A stage assembled over an older version is not valid release input.

If `STEMTEX_RUNTIME_SOURCE` contains a prebuilt `xelatexdaemon.fmt`, CMake
installs it to reduce first-start latency. The format is optional: when it is
absent, or when it is incompatible with the selected TeX tree, the renderer
generates a compatible copy in `%LOCALAPPDATA%\StemTeX\formats` with the
patched StemTeX engine.

## Validate The Stage

Run the native renderer against the staged runtime and profile:

```powershell
$repo = (Get-Location).Path
$smoke = ".\build\stemtex-ninja\cpp-daemon\stemtex-renderer-smoke.exe"
$runtime = ".\staging\runtime"
$profile = ".\staging\gui\profiles\unicodemath"

& $smoke --repo $repo --runtime $runtime --profile $profile --case validate --spares 0
& .\staging\gui\stemtex-renderer-gui.exe --smoke
& .\staging\gui\stemtex-profile-creator.exe --smoke --runtime $runtime --texmf $runtime
& .\build\stemtex-ninja\profile-creator\stemtex-profile-smoke.exe --runtime $runtime --texmf $runtime
cmake --build --preset ninja-release --target stemtex-checkpoint-audit
```

The Profile Creator smoke materializes representative font combinations. With
`--runtime`, it also starts a real worker and renders each combination. Against
the bundled tree it tests the available minimal combinations and skips
full-tree-only recipes. Pass a compatible full TeX Live root to `--texmf` to
exercise TeX Gyre/Fandol, Libertinus, and STIX Two as well.

Before a release that changes worker recovery, also run the no-spare corpus:

```powershell
& $smoke --repo $repo --runtime $runtime --profile $profile --case bad-corpus --spares 0 --runs 1
& $smoke --repo $repo --runtime $runtime --profile $profile --case checkpoint-critical --spares 0 --runs 1
& $smoke --repo $repo --runtime $runtime --profile $profile --case bad-output-corpus --spares 0 --runs 1
& $smoke --repo $repo --runtime $runtime --profile $profile --case list-state --spares 0 --runs 1
```

The full recovery rationale and expected corpus behavior are documented in
[XeTeX checkpoint recovery](XETEX_CHECKPOINT_RECOVERY.md).

## Warmup Cache Policy

CMake install copies profile `preamble.tex` and `warmup.tex`, but deliberately
excludes `warmup.xdv`, logs, and other generated TeX output. A source or fresh
package stage should therefore have no profile `warmup.xdv`.

During normal use, the renderer derives XDV state from `warmup.tex` and caches
it under `%LOCALAPPDATA%\StemTeX\profile-xdv`. Its persistent Fontconfig cache
uses a matching runtime/tree/profile key under `%LOCALAPPDATA%\StemTeX`.
An XDV cache older than the profile sources or selected daemon format is rebuilt
automatically. The Renderer GUI also exposes `清空 XDV`, which clears both
keyed caches and forces the next initialization to regenerate XDV and rescan
fonts.

For a manually installed tree, generate the maintained default profile cache by
running the installed helper from the runtime directory:

```bat
cd /d C:\path\to\StemTeX\runtime
refresh-profile-cache.bat
```

With no argument, the helper uses the adjacent
`gui\profiles\unicodemath` directory. Pass an absolute profile path to
refresh a different profile. Do not pass a bare relative Windows path with
backslashes: XeTeX can interpret the backslashes in that path as control
sequences.

The installer runs this helper for `unicodemath` when XeTeX, the GUI and the
bundled `texmf` component are selected. That result is an
optional bundled-tree startup cache; external TeX trees always use their own
per-user keyed cache. The installer does not ship the generated cache in its
payload.

## Build The Installer

Build the patched LuaHBTeX with `scripts/build-luahbtex-mingw.ps1` and the
application with the GUI enabled, then run the installer build from the
repository root.

```powershell
.\scripts\build-installer.ps1
```

The result is:

```text
dist/installer/StemTeX-<version>-Setup.exe
```

The script installs the application and the CMake `LuaHBTeX` component into
a fresh stage, verifies the Lua engine and checkpoint file against the build
manifest, and generates the XeTeX format with that stage's own macro tree.
Use `-StageDir` and `-OutputDir` for a second build of the same version.

The installer offers XeTeX with the GUI and bundled fonts by default. The
LuaTeX preset includes the macro packages, Lua modules and fonts for its
default profile. The two-engine preset
installs both and shares the converters. For unattended installs use
`/TYPE=full` (XeTeX), `/TYPE=lua`, or `/TYPE=both`. The existing
`/TYPE=compact` selects the XeTeX runtime. Custom selections with
`/COMPONENTS` must include at least one engine. Add `texmf` to install
the matching macro and font trees for the selected engines. For example,
`/COMPONENTS=luatex,gui,texmf` installs the standalone Lua GUI and
`/COMPONENTS=luatex,gui` uses a user-supplied TeX tree.

On a test installation, run each installed GUI's `--smoke` check. With both
engines installed, run `stemtex-renderer-gui.exe --smoke --engine luatex` as
well to verify PDF and SVG rendering through the LuaHBTeX worker.

For a Lua-only or dual-engine installation, run the cold-start regression
with `python scripts/test-bundled-luatex.py <install-root>`. It starts with
an empty cache and no system TeX tools on PATH, runs both GUIs, and checks
the format and worker's recorded inputs together with the PDF/SVG outputs.
