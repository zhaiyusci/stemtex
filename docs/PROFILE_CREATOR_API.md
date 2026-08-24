# Profile Creator API

StemTeX Profile Creator turns independent text, math, and CJK font choices into
a normal renderer profile. The implementation is split into a Qt-free native
library and a reference Qt application:

```text
stemtex-profile.dll        Font catalog, availability, recipes, generation
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

`profile.json` records stable recipe IDs and a generated-content fingerprint.
It does not record the local TeX Live path. `preamble.tex` and `warmup.tex` are
ordinary renderer profile sources, so every existing renderer host can consume
the result without a new rendering API.

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
- math: Latin Modern Math, XITS Math, Lete Sans Math, TeX Gyre Termes Math,
  TeX Gyre Pagella Math, Libertinus Math, and STIX Two Math;
- CJK: none, Windows SimSun/SimHei, SimHei, Microsoft YaHei, Fandol Song, and
  Fandol Hei.

Catalog entries remain visible when unavailable. The JSON reports missing
files, allowing a GUI to disable an entry before profile creation. Selecting a
full TeX Live root therefore exposes recipes that the bundled small tree does
not carry.

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

char *stemtex_profile_font_catalog_json(...);
char *stemtex_profile_preamble_utf8(...);
int stemtex_profile_materialize(...);
void stemtex_profile_free_string(char *value);
```

`stemtex_profile_font_catalog_json` validates that `texmf_root_utf8` has the
TeX Live `texmf-dist` and `texmf-dist\web2c` shape, then returns all recipes and
their availability in the selected tree.

`stemtex_profile_preamble_utf8` validates one combination and returns the exact
generated preamble without writing files.

`stemtex_profile_materialize` creates `<profiles_root>/<name>` through a
temporary sibling directory and rename. It never overwrites an existing
profile. On success, result JSON contains `profilePath` and `fingerprint`.

Every returned `char *`, including errors, must be released with
`stemtex_profile_free_string`. Static version strings must not be released.

## Host Example

```cpp
StemTeXProfileContext context{"C:\\texlive\\2026"};
StemTeXProfileSpec spec{
    "paper-fonts",
    "tex-gyre-termes",
    "tex-gyre-termes-math",
    "fandol-song",
};

StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
char *result = nullptr;
char *error = nullptr;
int ok = stemtex_profile_materialize(
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

The lower editor updates immediately through `stemtex_profile_preamble_utf8`
and shows the exact `preamble.tex` produced by the current text, math, and CJK
font choices. The Renderer GUI launches the creator with its current runtime
and TeX Live choices, then rescans user profiles when the creator exits.
