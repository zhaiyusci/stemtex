#include "stemtex_renderer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv) {
  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = argc > 1 ? argv[1] : nullptr;
  cfg.runtime_root_utf8 = argc > 2 ? argv[2] : nullptr;
  int runs = argc > 3 ? std::atoi(argv[3]) : 1;
  std::string case_name = argc > 4 ? argv[4] : "";
  cfg.spare_worker_count = argc > 5 ? std::atoi(argv[5]) : 1;
  if (runs <= 0) runs = 1;

  char *error = nullptr;
  long long create_start = now_ms();
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &error);
  long long create_end = now_ms();
  if (!renderer) {
    std::fprintf(stderr, "create failed: %s\n", error ? error : "");
    stemtex_renderer_free_string(error);
    return 1;
  }
  std::printf("createMs=%lld\n", create_end - create_start);

  const char *snippet =
      u8"\u4e2d\u6587 C++ DLL smoke test: $E=mc^2$ "
      u8"\\[\\int_0^1 x^2\\,dx=\\frac13\\] "
      u8"{\\color{blue}$\\ce{2H2 + O2 -> 2H2O}$}";
  bool bad_then_good = false;
  if (case_name == "--bad") {
    snippet = u8"\u4e2d\u6587 error test: \\undefinedstemtexcommand";
  } else if (case_name == "--bad-then-good") {
    bad_then_good = true;
    snippet = u8"\u4e2d\u6587 error test: \\undefinedstemtexcommand";
  } else if (case_name == "--bad-then-good-wait") {
    bad_then_good = true;
    std::this_thread::sleep_for(std::chrono::seconds(8));
    snippet = u8"\u4e2d\u6587 error test: \\undefinedstemtexcommand";
  } else if (case_name == "--chem-text") {
    snippet = u8"\u6b63\u6587\u6a21\u5f0f\u5316\u5b66: "
              u8"\\ce{H2O}, \\ce{CO2}, \\ce{2H2 + O2 -> 2H2O}.";
  } else if (case_name == "--physics") {
    snippet = u8"\u7269\u7406\u516c\u5f0f: "
              u8"\\[\\ip{\\psi}{\\phi}\\quad \\dv{x}\\sin x=\\cos x\\quad \\vb{v}\\cdot\\vu{n}\\]";
  } else if (case_name == "--fonts") {
    snippet = u8"\u5b57\u4f53\u6d4b\u8bd5: \u4e2d\u6587\u5b8b\u4f53, "
              u8"{\\sffamily \u4e2d\u6587\u9ed1\u4f53}, "
              u8"\\textbf{bold}, \\textit{italic}, \\texttt{mono}.";
  }

  for (int i = 0; i < runs; ++i) {
    StemTeXRenderResult result{};
    long long render_start = now_ms();
    if (!stemtex_renderer_render(renderer, snippet, 360, &result, &error)) {
      long long render_end = now_ms();
      std::fprintf(stderr, "run=%d renderMs=%lld\n", i + 1, render_end - render_start);
      std::fprintf(stderr, "render failed: %s\n", error ? error : "");
      stemtex_renderer_free_string(error);
      if (!bad_then_good) {
        stemtex_renderer_destroy(renderer);
        return 1;
      }
      snippet = u8"\u4e2d\u6587 recovery test: $E=mc^2$ "
                u8"\\[\\ip{\\psi}{\\phi}\\quad \\ce{H2O}\\]";
      continue;
    }
    long long render_end = now_ms();
    std::printf("run=%d renderMs=%lld pdf=%s\nsummary=%s\n", i + 1, render_end - render_start,
                result.pdf_path_utf8, result.summary_json_utf8);
    stemtex_renderer_free_result(&result);
  }

  stemtex_renderer_destroy(renderer);
  return 0;
}
