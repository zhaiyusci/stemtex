#pragma once

#include <stddef.h>

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
  int request_timeout_ms;
  int xdvipdfmx_timeout_ms;
  int min_width_pt;
  int max_width_pt;
  int default_width_pt;
  /* 0 means the default hot spare count. Positive values are clamped internally. */
  int spare_worker_count;
  int auto_restart;
  int delete_intermediates;
  const char *warmup_tex_utf8;
  const char *worker_template_utf8;
  const char *preamble_tex_utf8;
} StemTeXConfig;

typedef enum StemTeXErrorCode {
  STEMTEX_OK = 0,
  STEMTEX_ERROR_INVALID_ARGUMENT,
  STEMTEX_ERROR_BAD_CONFIG,
  STEMTEX_ERROR_WORKER_STARTUP,
  STEMTEX_ERROR_WORKER_TIMEOUT,
  STEMTEX_ERROR_WORKER_RESTARTING,
  STEMTEX_ERROR_WORKER_BUSY,
  STEMTEX_ERROR_TEX_SNIPPET,
  STEMTEX_ERROR_XDVIPDFMX,
  STEMTEX_ERROR_CANCELLED,
  STEMTEX_ERROR_FILESYSTEM,
  STEMTEX_ERROR_INTERNAL
} StemTeXErrorCode;

typedef enum StemTeXRendererStatus {
  STEMTEX_STATUS_STARTING = 0,
  STEMTEX_STATUS_READY,
  STEMTEX_STATUS_RENDERING,
  STEMTEX_STATUS_RESTARTING,
  STEMTEX_STATUS_DEAD
} StemTeXRendererStatus;

typedef struct StemTeXRenderResult {
  char *request_id_utf8;
  char *pdf_path_utf8;
  char *summary_json_utf8;
} StemTeXRenderResult;

typedef struct StemTeXPdfBytes {
  unsigned char *data;
  size_t size;
} StemTeXPdfBytes;

typedef struct StemTeXEngineSnapshot {
  StemTeXRendererStatus status;
  int primary_ready;
  int spare_ready;
  int spare_target;
  int spare_rebuilding;
  StemTeXErrorCode last_error;
} StemTeXEngineSnapshot;

typedef struct StemTeXRenderer StemTeXRenderer;

typedef void (*StemTeXRenderCallback)(int ok, const StemTeXRenderResult *result, StemTeXErrorCode error_code,
                                      const char *error_utf8, void *user_data);

STEMTEX_API StemTeXRenderer *stemtex_renderer_create(const StemTeXConfig *config, StemTeXErrorCode *error_code,
                                                     char **error_utf8);
STEMTEX_API int stemtex_renderer_render(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                        StemTeXRenderResult *result, StemTeXErrorCode *error_code,
                                        char **error_utf8);
STEMTEX_API int stemtex_renderer_render_pdf_bytes(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                                  StemTeXPdfBytes *pdf, StemTeXRenderResult *result,
                                                  StemTeXErrorCode *error_code, char **error_utf8);
STEMTEX_API int stemtex_renderer_render_async(StemTeXRenderer *renderer, const char *snippet_utf8, int width_pt,
                                              StemTeXRenderCallback callback, void *user_data,
                                              StemTeXErrorCode *error_code, char **error_utf8);
STEMTEX_API int stemtex_renderer_restart(StemTeXRenderer *renderer, StemTeXErrorCode *error_code, char **error_utf8);
STEMTEX_API int stemtex_renderer_cancel_current(StemTeXRenderer *renderer, StemTeXErrorCode *error_code,
                                                char **error_utf8);
STEMTEX_API StemTeXRendererStatus stemtex_renderer_status(StemTeXRenderer *renderer);
STEMTEX_API int stemtex_renderer_engine_snapshot(StemTeXRenderer *renderer, StemTeXEngineSnapshot *snapshot);
STEMTEX_API StemTeXErrorCode stemtex_renderer_last_error_code(StemTeXRenderer *renderer);
STEMTEX_API char *stemtex_renderer_get_log_tail(StemTeXRenderer *renderer, int max_bytes);
STEMTEX_API const char *stemtex_renderer_version(void);
STEMTEX_API const char *stemtex_renderer_abi_version(void);
STEMTEX_API char *stemtex_renderer_runtime_version(StemTeXRenderer *renderer);
STEMTEX_API int stemtex_renderer_validate_config(const StemTeXConfig *config, StemTeXErrorCode *error_code,
                                                 char **diagnostics_utf8);
STEMTEX_API int stemtex_refresh_font_cache(const char *runtime_root_utf8, const char *warmup_tex_utf8,
                                           StemTeXErrorCode *error_code, char **error_utf8);
STEMTEX_API void stemtex_renderer_free_result(StemTeXRenderResult *result);
STEMTEX_API void stemtex_renderer_free_pdf_bytes(StemTeXPdfBytes *pdf);
STEMTEX_API void stemtex_renderer_free_string(char *value);
STEMTEX_API void stemtex_renderer_destroy(StemTeXRenderer *renderer);

#ifdef __cplusplus
}
#endif
