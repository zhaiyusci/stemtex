#pragma once

#include <stddef.h>

#ifdef _WIN32
#ifdef STEMTEX_PROFILE_EXPORTS
#define STEMTEX_PROFILE_API __declspec(dllexport)
#else
#define STEMTEX_PROFILE_API __declspec(dllimport)
#endif
#else
#define STEMTEX_PROFILE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum StemTeXProfileErrorCode {
  STEMTEX_PROFILE_OK = 0,
  STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT = 1,
  STEMTEX_PROFILE_ERROR_BAD_TEXMF_ROOT = 2,
  STEMTEX_PROFILE_ERROR_UNKNOWN_FONT = 3,
  STEMTEX_PROFILE_ERROR_FONT_UNAVAILABLE = 4,
  STEMTEX_PROFILE_ERROR_PROFILE_EXISTS = 5,
  STEMTEX_PROFILE_ERROR_FILESYSTEM = 6,
  STEMTEX_PROFILE_ERROR_INTERNAL = 7,
  STEMTEX_PROFILE_ERROR_UNKNOWN_PACKAGE = 8,
  STEMTEX_PROFILE_ERROR_PACKAGE_UNAVAILABLE = 9
} StemTeXProfileErrorCode;

typedef struct StemTeXProfileContext {
  /* TeX Live-style root that directly contains texmf-dist. */
  const char *texmf_root_utf8;
} StemTeXProfileContext;

typedef struct StemTeXProfileSpec {
  /* Directory/profile name. It must be a single valid Windows filename. */
  const char *name_utf8;
  const char *text_font_id_utf8;
  const char *math_font_id_utf8;
  const char *cjk_font_id_utf8;
} StemTeXProfileSpec;

/*
 * Package-aware profile specification. package_ids_utf8 contains the package
 * recipes explicitly selected by the user. Dependencies and load order are
 * resolved by stemtex-profile.dll; an empty array intentionally selects no
 * optional whitelist packages.
 */
typedef struct StemTeXProfileSpecV2 {
  const char *name_utf8;
  const char *text_font_id_utf8;
  const char *math_font_id_utf8;
  const char *cjk_font_id_utf8;
  const char *const *package_ids_utf8;
  size_t package_count;
} StemTeXProfileSpecV2;

/*
 * User-extensible profile specification. Curated package IDs are still
 * dependency-resolved first. user_preamble_utf8 is then appended verbatim as
 * the final preamble section and may contain package loads, definitions, and
 * configuration commands; null means no user preamble.
 */
typedef struct StemTeXProfileSpecV3 {
  const char *name_utf8;
  const char *text_font_id_utf8;
  const char *math_font_id_utf8;
  const char *cjk_font_id_utf8;
  const char *const *package_ids_utf8;
  size_t package_count;
  const char *user_preamble_utf8;
} StemTeXProfileSpecV3;

/* Catalog JSON contains text, math, and CJK recipes plus current availability. */
STEMTEX_PROFILE_API char *stemtex_profile_font_catalog_json(
    const StemTeXProfileContext *context,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

/* Curated package recipes, availability, dependencies, and ordering metadata. */
STEMTEX_PROFILE_API char *stemtex_profile_package_catalog_json(
    const StemTeXProfileContext *context,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

/* Resolves explicit package IDs to their dependency closure and load order. */
STEMTEX_PROFILE_API char *stemtex_profile_package_plan_json(
    const StemTeXProfileContext *context,
    const char *const *package_ids_utf8,
    size_t package_count,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

/* Generates the exact managed preamble without writing a profile directory. */
STEMTEX_PROFILE_API char *stemtex_profile_preamble_utf8(
    const StemTeXProfileContext *context,
    const StemTeXProfileSpec *spec,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

/* Package-aware preamble generation. */
STEMTEX_PROFILE_API char *stemtex_profile_preamble_v2_utf8(
    const StemTeXProfileContext *context,
    const StemTeXProfileSpecV2 *spec,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

/* Curated packages plus a final verbatim user preamble section. */
STEMTEX_PROFILE_API char *stemtex_profile_preamble_v3_utf8(
    const StemTeXProfileContext *context,
    const StemTeXProfileSpecV3 *spec,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

/*
 * Atomically creates <profiles_root>/<spec.name>. Existing directories are
 * never overwritten. The result JSON reports the new profile path and recipe
 * fingerprint.
 */
STEMTEX_PROFILE_API int stemtex_profile_materialize(
    const StemTeXProfileContext *context,
    const StemTeXProfileSpec *spec,
    const char *profiles_root_utf8,
    char **result_json_utf8,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

STEMTEX_PROFILE_API int stemtex_profile_materialize_v2(
    const StemTeXProfileContext *context,
    const StemTeXProfileSpecV2 *spec,
    const char *profiles_root_utf8,
    char **result_json_utf8,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

STEMTEX_PROFILE_API int stemtex_profile_materialize_v3(
    const StemTeXProfileContext *context,
    const StemTeXProfileSpecV3 *spec,
    const char *profiles_root_utf8,
    char **result_json_utf8,
    StemTeXProfileErrorCode *error_code,
    char **error_utf8);

STEMTEX_PROFILE_API const char *stemtex_profile_version(void);
STEMTEX_PROFILE_API const char *stemtex_profile_abi_version(void);
STEMTEX_PROFILE_API void stemtex_profile_free_string(char *value);

#ifdef __cplusplus
}
#endif
