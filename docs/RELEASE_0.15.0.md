# StemTeX 0.15.0

## Changes

- Add `stemtex_renderer_render_block`, a synchronous PDF/SVG API for fixed-width
  blocks with automatic content height. Ordinary paragraphs retain the full
  first-line ascent and last-line depth; tall formulas and graphics grow
  naturally. Literal leading displays keep their natural spacing.
- Use block rendering in the Renderer GUI. Preserve the entire logical page in
  the preview instead of cropping to visible pixels, while retaining the
  separate preview padding setting.
- Prefer the bundled `unicodemath` profile on initial startup, so older custom
  profiles do not become the default just because they sort first.
- Add opt-in `qrcode` support to Profile Creator, including TeX Live availability
  detection, automatic `xcolor` dependency selection, Chinese UI text, and a
  QR-code warmup probe.

## Compatibility

Existing renderer entry points and public result structures are unchanged.
The new function is opt-in for SDK callers; the updated GUI requires the
matching 0.15.0 renderer DLL. Existing user profiles are not rewritten.

Block sizing uses TeX's logical boxes and retains the profile's preview border.
It does not repair painted content deliberately placed outside those boxes.
Leading author glue or specials can hide the first measurable line; those
boundaries retain their authored geometry. Leading-display detection recognizes
literal syntax rather than expanding arbitrary author macros.

`qrcode` remains off by default and requires a TeX Live tree containing the
package. It is not added to the bundled minimal TeX tree.

See [Native Renderer C API](CPP_RENDERER_API.md#automatic-height-blocks) and
[Profile Creator API](PROFILE_CREATOR_API.md) for usage details.
