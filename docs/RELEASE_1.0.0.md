# StemTeX 1.0.0

The Windows installer now offers XeTeX, LuaHBTeX, or both. The default preset
continues to install XeTeX with the GUI and bundled macros and fonts.

## Installation

- Choose an engine and keep the bundled macros and fonts selected to use the
  default Latin Modern and `unicode-math` profile without installing TeX Live.
  Profiles that need additional packages can use an external TeX Live tree.
- The installer includes the macro trees for the selected engines. The component
  sizes update when the selection changes. Both engines use the same installed
  PDF and SVG converters.

## Rendering and profiles

- The Renderer GUI supports an experimental LuaHBTeX backend. It streams DVI
  pages and reuses the PDF and SVG converters across preview requests.
- The LuaHBTeX worker restores its checkpoint in place after recoverable snippet
  errors. The GUI can rebuild the worker when in-place recovery fails.
- The GUI and Profile Creator detect the installed engines. Profile Creator
  supports engine-specific preambles and LuaLaTeX profiles. The public C++
  renderer SDK continues to use XeTeX.
