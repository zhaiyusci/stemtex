# StemTeX Documentation

The root [`README`](../README.md) is the project overview and shortest path to a
working staged GUI. The documents here separate build operations, public API
contracts, distribution policy, and engine internals.

## Start Here

| Task | Document | Scope |
| --- | --- | --- |
| Build, stage, test, or package StemTeX | [Building and packaging](BUILDING_AND_PACKAGING.md) | Supported CMake application flow and Inno Setup package flow. |
| Embed the native renderer | [C renderer API](CPP_RENDERER_API.md) | Public ABI, configuration, ownership, rendering, and errors. |
| Check implemented and pending behavior | [Renderer status](CPP_RENDERER_API_STATUS.md) | Current contract and remaining product work. |
| Understand the installed tree or external TeX Live support | [Distribution options](DISTRIBUTION_OPTIONS.md) | Components, paths, writable data, cache policy, and compatibility boundary. |
| Understand bad-snippet recovery | [XeTeX checkpoint recovery](XETEX_CHECKPOINT_RECOVERY.md) | In-process snapshot model, protocol, limits, and regression tests. |
| Rebuild patched engine binaries | [Windows engine rebuild notes](WINDOWS_XETEX_BUILD_NOTES.md) | Generated-C XeTeX, converters, and static dependency maintenance. |

## Sources Of Truth

- Public ABI declarations: [`cpp-daemon/stemtex_renderer.h`](../cpp-daemon/stemtex_renderer.h)
- Version: [`VERSION`](../VERSION)
- Build and install rules: [`CMakeLists.txt`](../CMakeLists.txt) and
  [`CMakePresets.json`](../CMakePresets.json)
- Installer behavior: [`installer/stemtex.iss`](../installer/stemtex.iss)
- Bundled profile source: [`gui/profiles`](../gui/profiles)
- Generated-C patch record:
  [`patches/texlive-generated-daemon-runtime-switches.patch`](../patches/texlive-generated-daemon-runtime-switches.patch)

Generated directories such as `build/`, `staging/`, `dist/`, and
`texlive-xetex/out/` are evidence from a local build, not documentation or
release source. Do not use their contents to define the API or package layout.

## Documentation Rules

- Keep normal build and package commands in `BUILDING_AND_PACKAGING.md`.
- Keep public host behavior in `CPP_RENDERER_API.md`; implementation details
  belong in the checkpoint or engine notes.
- Keep future work in `CPP_RENDERER_API_STATUS.md`, not mixed into the API
  contract.
- Describe generated caches as artifacts. Profile source consists of
  `preamble.tex` and `warmup.tex`; `warmup.xdv` is never a source file.
- Update command examples when CMake presets, smoke cases, or installer inputs
  change.
