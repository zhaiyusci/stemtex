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
| `mathtools` | mathematics | on | pre-font math foundation |
| `mhchem` | chemistry | on | requires `mathtools`; uses `version=4` |
| `physics` | physics | on | requires `mathtools` |
| `xcolor` | text/color | on | normal post-font package |
| `cancel` | mathematics | on | ordered after `xcolor` when both are selected |
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
directory from its ID. The drawing group deliberately excludes recipes that
need shell escape or an external conversion/layout executable.

The resolver first expands hard requirements, then performs a stable
topological ordering. The current generated preamble is arranged as:

```text
document class
pre-font whitelist packages
managed math/text/CJK font recipes
post-font whitelist packages
managed preview package and settings
```

The planned user-authored command section will be appended after this managed
section. It is intentionally not exposed by the current ABI or Qt UI yet.
`preview`, `fontspec`, `unicode-math`, and `xeCJK` remain internal/managed rather
than selectable whitelist entries.

`warmup.tex` probes only commands belonging to the resolved package set. For
example, disabling `mhchem` removes both its `\usepackage` line and the `\ce`
readiness probe; selecting `pgfplots` adds the `xcolor`/`tikz` dependency chain,
its managed compatibility setting, and lightweight TikZ/axis probes.

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

char *stemtex_profile_font_catalog_json(...);
char *stemtex_profile_package_catalog_json(...);
char *stemtex_profile_package_plan_json(...);
char *stemtex_profile_preamble_utf8(...);
char *stemtex_profile_preamble_v2_utf8(...);
int stemtex_profile_materialize(...);
int stemtex_profile_materialize_v2(...);
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
dependency has `explicit: false` and reports its `requiredBy` parents.

The original `StemTeXProfileSpec`, `stemtex_profile_preamble_utf8`, and
`stemtex_profile_materialize` entry points remain available and select all five
legacy/default packages. New hosts should use `StemTeXProfileSpecV2` and the
`_v2` entry points; an empty package array intentionally generates a profile
with no optional whitelist packages.

Both preamble entry points validate one combination and return the exact
generated preamble without writing files.

`stemtex_profile_materialize` creates `<profiles_root>/<name>` through a
temporary sibling directory and rename. It never overwrites an existing
profile. On success, result JSON contains `profilePath` and `fingerprint`.

Every returned `char *`, including errors, must be released with
`stemtex_profile_free_string`. Static version strings must not be released.

## Host Example

```cpp
StemTeXProfileContext context{"C:\\texlive\\2026"};
const char *packages[] = {"mathtools", "mhchem", "xcolor", "cancel"};
StemTeXProfileSpecV2 spec{
    "paper-fonts",
    "tex-gyre-termes",
    "tex-gyre-termes-math",
    "fandol-song",
    packages,
    4,
};

StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
char *result = nullptr;
char *error = nullptr;
int ok = stemtex_profile_materialize_v2(
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

The Qt application presents available whitelist packages as checkable rows.
Unavailable rows remain visible but disabled. A partially checked row denotes
a package brought in only by another selection. The UI displays the actual
resolved sequence, including the managed font and preview boundaries; it does
not maintain a second package-order table.

The lower editor updates immediately through
`stemtex_profile_preamble_v2_utf8` and shows the exact `preamble.tex` produced
by the current font/package choices. The Renderer GUI launches the creator with
its current runtime and TeX Live choices, then rescans user profiles when the
creator exits.
