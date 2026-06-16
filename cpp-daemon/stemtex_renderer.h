#pragma once

#ifdef STEMTEX_RENDERER_EXPORTS
#define STEMTEX_API __declspec(dllexport)
#else
#define STEMTEX_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StemTeXConfig {
  const char *repo_root_utf8;
  const char *runtime_root_utf8;
  const char *state_root_utf8;
  const char *renders_root_utf8;
} StemTeXConfig;

typedef struct StemTeXRenderResult {
  char *request_id_utf8;
  char *pdf_path_utf8;
  char *summary_json_utf8;
} StemTeXRenderResult;

typedef struct StemTeXRenderer StemTeXRenderer;

STEMTEX_API StemTeXRenderer *stemtex_renderer_create(const StemTeXConfig *config, char **error_utf8);
STEMTEX_API int stemtex_renderer_render(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                        StemTeXRenderResult *result, char **error_utf8);
STEMTEX_API void stemtex_renderer_free_result(StemTeXRenderResult *result);
STEMTEX_API void stemtex_renderer_free_string(char *value);
STEMTEX_API void stemtex_renderer_destroy(StemTeXRenderer *renderer);

#ifdef __cplusplus
}
#endif
