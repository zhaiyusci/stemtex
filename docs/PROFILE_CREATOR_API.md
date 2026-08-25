# Profile Creator API

StemTeX Profile Creator turns independent font choices and curated package
selections into a normal renderer profile. The implementation is split into a
Qt-free native library and a reference Qt application:

```text
stemtex-profile.dll          Font/package catalogs, dependency planning, generation
stemtex-profile-creator.exe  Reference Qt front end with live preamble preview
```

The renderer remains the consumer of generated profiles. It does not depend on
the creator library, and hand-written profiles remain supported.

## Managed Profile Shape

The creator atomically writes a new directory under the host-selected profiles
root:

```text
<name>/
  profile.json
  preamble.tex
  warmup.tex
```

`profile.json` schema 2 records stable font recipe IDs, explicitly selected
whitelist package IDs, dependency-resolved package order, and a generated-content
fingerprint. It does not record the local TeX Live path. `preamble.tex` and
`warmup.tex` are ordinary renderer profile sources, so every existing renderer
host can consume the result without a new rendering API.

The reference Qt application writes to
`%LOCALAPPDATA%\StemTeX\profiles`. The Renderer GUI scans this directory in
addition to its installed `gui\profiles` directory.

## Font Model

Text, math, and CJK choices are independent. Recipes own the TeX implementation
details: package order, OpenType filenames, face mapping, `unicode-math`,
`fontspec`, `xeCJK`, and package-specific options such as Lete Sans Math's text
font suppression.

The initial catalog contains:

- text: Latin Modern, XITS, TeX Gyre Termes, Libertinus Serif, STIX Two Text,
  and Windows Arial;
- math: the maintained TeX Live `unicode-math` OpenType families, including
  Latin Modern, XITS, Lete Sans Math, TeX Gyre Math, Libertinus, STIX, New
  Computer Modern, KpMath, and the other recipes reported by the catalog;
- CJK: none, Windows SimSun/SimHei, SimHei, Microsoft YaHei, Fandol Song, and
  Fandol Hei.

Catalog entries remain visible when unavailable. The JSON reports missing
files, allowing a GUI to disable an entry before profile creation. Selecting a
full TeX Live root therefore exposes recipes that the bundled small tree does
not carry.

## Whitelist Package Model

The curated package whitelist begins with the set exercised by the original
StemTeX profiles and an opt-in XeTeX-native drawing group:

| ID | Category | Default | Managed relation |
| --- | --- | --- | --- |
| `mathtools` | mathematics | off | pre-font math foundation |
| `mhchem` | chemistry | off | requires `mathtools`; uses `version=4` |
| `physics` | physics | off | requires `mathtools` |
| `siunitx` | numbers/units | off | ordered after `physics` when both are selected; emits a compatibility notice |
| `xcolor` | text/color | off | normal post-font package |
| `cancel` | mathematics | off | ordered after `xcolor` when both are selected |
| `graphicx` | images | off | graphics inclusion and box transforms; required by `adjustbox` |
| `array` | tables | off | extended table and mathematical-array columns; required by `tabularx` |
| `booktabs` | tables | off | publication-quality table rules |
| `tabularx` | tables | off | requires `array`; flexible-width columns |
| `multirow` | tables | off | cells spanning multiple rows |
| `adjustbox` | layout | off | requires `graphicx`; constrains and transforms boxed content |
| `enumitem` | lists | off | configurable labels, spacing, and indentation |
| `tikz` | graphics | off | requires `xcolor`; drawing foundation |
| `pgfplots` | plots | off | requires `tikz`; emits `compat=newest` after loading |
| `tikz-cd` | diagrams | off | requires `tikz` |
| `circuitikz` | diagrams | off | requires `tikz` |
| `forest` | diagrams | off | requires `tikz` |
| `chemfig` | chemistry/drawing | off | requires `tikz` |
| `quantikz` | diagrams | off | requires `tikz` |

Each recipe owns its TeX Live `texmf-dist` relative path, default options,
post-load setup, warmup probe, phase, stable order, hard requirements, and
conditional `after` relationships. Package availability uses the same
deterministic regular-file check as font recipes. It does not guess a package
directory from its ID. The current recipes deliberately exclude packages that
need shell escape or an external conversion/layout executable. `graphicx`
readiness is tested with an in-memory box transform, so creating a profile does
not require an external image file.

Selecting both `physics` and `siunitx` follows `siunitx`'s own compatibility
policy: `physics` is loaded first and retains `\qty`; users can use `\SI`,
`\num`, and `\unit` for `siunitx` input. The resolver reports this as a
non-blocking notice in the package plan. StemTeX neither rejects the combination
nor silently redefines either package's commands.

The resolver first expands hard requirements, then performs a stable
topological ordering. The current generated preamble is arranged as:

```text
document class
pre-font whitelist packages
managed math/text/CJK font recipes
post-font whitelist packages
managed preview package and settings
verbatim user preamble
```

The user preamble is deliberately not parsed or dependency-managed. It may
contain additional `\usepackage` declarations, command definitions, and package
configuration. The creator normalizes line endings, appends the text verbatim,
and includes it in the profile fingerprint. TeX reports any error when the
profile is initialized.
`preview`, `fontspec`, `unicode-math`, and `xeCJK` remain internal/managed rather
than selectable whitelist entries.

`warmup.tex` probes only commands belonging to the resolved package set. For
example, disabling `mhchem` removes both its `\usepackage` line and the `\ce`
readiness probe; selecting `pgfplots` adds the `xcolor`/`tikz` dependency chain,
its managed compatibility setting, and lightweight TikZ/axis probes. Selecting
`tabularx` adds `array`, and selecting `adjustbox` adds `graphicx`; their probes
use only short generated text and tables.

## Public C ABI

The source of truth is
[`profile-creator/stemtex_profile.h`](../profile-creator/stemtex_profile.h).
The ABI uses UTF-8 strings and caller-owned returned strings:

```cpp
typedef struct StemTeXProfileContext {
  const char *texmf_root_utf8;
} StemTeXProfileContext;

typedef struct StemTeXProfileSpec {
  const char *name_utf8;
  const char *text_font_id_utf8;
  const char *math_font_id_utf8;
  const char *cjk_font_id_utf8;
} StemTeXProfileSpec;

typedef struct StemTeXProfileSpecV2 {
  const char *name_utf8;
  const char *text_font_id_utf8;
  const char *math_font_id_utf8;
  const char *cjk_font_id_utf8;
  const char *const *package_ids_utf8;
  size_t package_count;
} StemTeXProfileSpecV2;

typedef struct StemTeXProfileSpecV3 {
  const char *name_utf8;
  const char *text_font_id_utf8;
  const char *math_font_id_utf8;
  const char *cjk_font_id_utf8;
  const char *const *package_ids_utf8;
  size_t package_count;
  const char *user_preamble_utf8;
} StemTeXProfileSpecV3;

char *stemtex_profile_font_catalog_json(...);
char *stemtex_profile_package_catalog_json(...);
char *stemtex_profile_package_plan_json(...);
char *stemtex_profile_preamble_utf8(...);
char *stemtex_profile_preamble_v2_utf8(...);
char *stemtex_profile_preamble_v3_utf8(...);
int stemtex_profile_materialize(...);
int stemtex_profile_materialize_v2(...);
int stemtex_profile_materialize_v3(...);
void stemtex_profile_free_string(char *value);
```

`stemtex_profile_font_catalog_json` validates that `texmf_root_utf8` has the
TeX Live `texmf-dist` and `texmf-dist\web2c` shape, then returns all recipes and
their availability in the selected tree.

`stemtex_profile_package_catalog_json` reports every curated package recipe,
including `relativePath`, `available`, `defaultEnabled`, `phase`, `order`,
`options`, `afterLoadTex`, `requires`, `after`, and `missing`.

`stemtex_profile_package_plan_json` accepts the package IDs explicitly selected
by a host and returns their dependency closure plus an ordered `loadOrder`
containing the managed font and preview boundaries. A package added only as a
dependency has `explicit: false` and reports its `requiredBy` parents. Its
`notices` array contains non-blocking, selection-dependent compatibility advice
with stable `id`, `severity`, `packageIds`, and fallback English `message`
fields; hosts may localize known notice IDs.

The original and V2 entry points remain available. New hosts should use
`StemTeXProfileSpecV3` and the `_v3` entry points. An empty package array selects
no optional whitelist packages, and a null `user_preamble_utf8` appends nothing.
V3 manifests use schema version 3 and retain the user text in `userPreamble`.

All preamble entry points validate one combination and return the exact generated
preamble without writing files.

`stemtex_profile_materialize` creates `<profiles_root>/<name>` through a
temporary sibling directory and rename. It never overwrites an existing
profile. On success, result JSON contains `profilePath` and `fingerprint`.

Every returned `char *`, including errors, must be released with
`stemtex_profile_free_string`. Static version strings must not be released.

## Host Example

```cpp
StemTeXProfileContext context{"C:\\texlive\\2026"};
const char *packages[] = {"mathtools", "mhchem", "xcolor", "cancel"};
StemTeXProfileSpecV3 spec{
    "paper-fonts",
    "tex-gyre-termes",
    "tex-gyre-termes-math",
    "fandol-song",
    packages,
    4,
    "\\usepackage{hyperref}\n"
    "\\newcommand{\\R}{\\mathbb{R}}\n",
};

StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
char *result = nullptr;
char *error = nullptr;
int ok = stemtex_profile_materialize_v3(
    &context, &spec,
    "C:\\Users\\me\\AppData\\Local\\StemTeX\\profiles",
    &result, &code, &error);

stemtex_profile_free_string(result);
stemtex_profile_free_string(error);
```

After materialization, pass the reported profile path to the existing renderer
as `StemTeXConfig.profile_root_utf8`, while passing the same TeX Live root as
`StemTeXConfig.texmf_root_utf8`.

## Reference Qt Application

`stemtex-profile-creator.exe` accepts optional arguments:

```text
--runtime PATH   StemTeX runtime used to locate the profile SDK
--texmf PATH     TeX Live tree used for discovery and generation
--profiles PATH  Destination profile collection
--smoke          Construct and validate the UI/catalog, then exit
```

The Qt application keeps the TeX Live root, destination, and profile name above
four tabs: fonts, whitelist packages, a large free-form user preamble editor,
and the generated preamble. Package rows therefore have a full page for
selection, dependency/load-order details, and compatibility notices, while both
editors remain readable at full size.
Unavailable rows remain visible but disabled. A partially checked row denotes
a package brought in only by another selection. The UI displays the actual
resolved sequence, including the managed font and preview boundaries; it does
not maintain a second package-order table.

The generated editor updates immediately through
`stemtex_profile_preamble_v3_utf8` and shows the exact `preamble.tex` produced
by the current font, whitelist, and verbatim user text. The Renderer GUI launches
the creator with its current runtime and TeX Live choices, then rescans user
profiles when the creator exits.
