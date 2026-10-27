# StemTeX 0.15.2 hotfix

## Changes

- Fix automatic-height block rendering for leading tall displays, including
  `equation*` with `split` when all four display skips are zero. Typeset the
  content between two actual, separate strut support lines.
- Remove exactly the height plus depth of each support line after layout.
  No visible-content scanning, additional safety margin, or formula/color
  special cases are applied.
- Keep native TeX display spacing and input whitespace semantics. Generated
  setting lines can end in `%` to prevent their newlines becoming spaces.

## Compatibility

Public interfaces and profiles are unchanged. The GUI continues to use
`stemtex_renderer_render_block`; legacy rendering APIs are unchanged.
The four display spacing parameters retain their profile defaults.

## Validation

- Block regression checks cover text, wrapping, font sizes, tall formulas,
  zero-skip split displays, PDF/SVG dimensions, and legacy API behavior.
- Color-profile checks preserve native color-group spacing.
- The fresh package stage passed block regression checks and GUI PDF/SVG
  smoke tests. The Windows installer was built; an elevated installation
  was not retested in this session.
- The reported zero-skip split formula was compared with an uncropped
  support-line reference: after removing the two support lines, the rendered
  pixels matched exactly at 722.7 DPI.
