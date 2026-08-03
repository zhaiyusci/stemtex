# Building And Packaging

This is the canonical procedure for the supported StemTeX application build,
staging tree, smoke tests, and Windows installer. It does not rebuild the
generated-C XeTeX daemon or its static third-party dependencies.

## Prerequisites

- Visual Studio 2022 with the x64 C++ toolchain;
- CMake 3.20 or newer;
- Qt 6 with `Core`, `Gui`, `Widgets`, `Pdf`, and `Svg`;
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

## Validate The Stage

Run the native renderer against the staged runtime and profile:

```powershell
$repo = (Get-Location).Path
$smoke = ".\build\stemtex-ninja\cpp-daemon\stemtex-renderer-smoke.exe"
$runtime = ".\staging\runtime"
$profile = ".\staging\gui\profiles\unicodemath_cjk"

& $smoke --repo $repo --runtime $runtime --profile $profile --case validate --spares 0
& .\staging\gui\stemtex-renderer-gui.exe --smoke
cmake --build --preset ninja-release --target stemtex-checkpoint-audit
```

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

For a manually installed tree, generate the maintained default profile cache by
running the installed helper from the runtime directory:

```bat
cd /d C:\path\to\StemTeX\runtime
refresh-profile-cache.bat
```

With no argument, the helper uses the adjacent
`gui\profiles\unicodemath_cjk` directory. Pass an absolute profile path to
refresh a different profile. Do not pass a bare relative Windows path with
backslashes: XeTeX can interpret the backslashes in that path as control
sequences.

The full installer runs this helper for `unicodemath_cjk` after installation
when both the GUI and bundled `texmf` components are selected. The installer
does not ship the generated cache in its payload.

## Build The Installer

Use a fresh version-specific package stage so old files cannot leak into the
installer:

```powershell
$version = (Get-Content .\VERSION).Trim()
$packageStage = Join-Path $PWD "dist\package-stage\StemTeX-$version"
$output = Join-Path $PWD "dist\installer"

if (Test-Path $packageStage) {
  throw "Package stage already exists: $packageStage"
}

cmake --install build\stemtex-ninja --prefix $packageStage

$stagedVersion = (Get-Content "$packageStage\runtime\VERSION").Trim()
if ($stagedVersion -ne $version) {
  throw "Staged VERSION is $stagedVersion, expected $version"
}

New-Item -ItemType Directory -Force $output | Out-Null
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" `
  "/DSourceDir=$packageStage" `
  "/DOutputDir=$output" `
  "/DAppVersion=$version" `
  .\installer\stemtex.iss
```

The result is:

```text
dist/installer/StemTeX-<version>-Setup.exe
```

Run the renderer smoke test against `$packageStage` before invoking Inno Setup,
using `$packageStage\runtime` and
`$packageStage\gui\profiles\unicodemath_cjk`. After building the installer,
install it into a test directory and verify both PDF and SVG output through the
GUI or the generic output API.
