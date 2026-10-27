# StemTeX 0.15.1

## Changes

- Simplify automatic-height block rendering to one TeX box wrapper. Use the
  selected font's strut ascent and depth to support ordinary first and last
  lines, while allowing taller content to grow naturally.
- Remove leading-display syntax detection. Literal formulas and formulas
  produced by user macros now follow the same layout path.
- Preserve native LaTeX display spacing and color-group behavior. No color
  restoration hooks or special empty-paragraph removal are applied.
- Add regression coverage for strut-supported text, macro-defined displays,
  block width, colored text, and native color-group spacing.
- Include the additional Computer Modern font metrics and Type 1 fonts used
  by small bold math in the bundled runtime, fixing missing-font errors in
  clean installations when using `\boldsymbol`.

## Compatibility

Existing public interfaces and user profiles are unchanged. The GUI continues
to use the automatic-height block API introduced in 0.15.0.

Leading displays now use the same box as other content, so native terminal
display spacing is retained and some formula-only outputs can be taller than
in 0.15.0. Color declarations that end with a display may retain LaTeX's native
extra paragraph spacing; this release deliberately does not compensate for it.

Block sizing remains based on logical TeX boxes, not visible-pixel cropping.
See [Native Renderer C API](CPP_RENDERER_API.md#automatic-height-blocks).

## Validation

- Block geometry and error-recovery checks passed with the bundled profile,
  an xcolor profile, and the Latin Modern / SimSun profile.
- Coverage includes PDF/SVG dimensions, wrapping, font sizes, tall formulas,
  first/last line metrics, and unchanged legacy rendering behavior.
