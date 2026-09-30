# StemTeX 0.14.0

## Changes

- Internationalize Profile Creator with English and Simplified Chinese UI text,
  including font/package descriptions, compatibility notices, and Qt buttons.
- Select the language from the system locale, or override it with
  `--language en`, `--language zh_CN`, or `--language system`.
- Embed the application and Qt Chinese translations in the executable; no
  external translation files are required.
- Use the following configuration in the bundled default `unicodemath` profile
  and every Profile Creator math recipe, including Lete Sans Math:

  ```tex
  \usepackage[
    mathrm=sym,
    mathit=sym,
    mathbf=sym,
    mathsf=sym,
    mathtt=sym
  ]{unicode-math}
  ```

- Refresh the Profile Creator's development SDK copy even when only its DLL
  changes, without requiring a GUI relink.

## Compatibility

Existing user profiles are not rewritten. Generate a new profile to adopt the
new math alphabet configuration. The C API/ABI and font/package IDs are unchanged;
SDK diagnostic details remain in English. The Renderer GUI's language is unchanged.

## Build requirements

GUI builds now require Qt LinguistTools and Qt's `qtbase_zh_CN.qm`. See
[Building and packaging](BUILDING_AND_PACKAGING.md) and
[Profile Creator API](PROFILE_CREATOR_API.md) for build and translation checks.
