#include "stemtex_renderer.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static fs::path default_runtime_root(const fs::path &repo_root) {
  const char *env = std::getenv("STEMTEX_RUNTIME");
  if (env && *env) return fs::path(env);
  fs::path side_tree = repo_root / "dist" / "stemtex-texlive-daemon";
  if (fs::exists(side_tree / "bin" / "windows" / "xetexdaemon.exe") &&
      fs::exists(side_tree / "texmf-var" / "cache-warmup" / "warmup.xdv")) {
    return side_tree;
  }
  return fs::path("C:/StemTeX");
}

static bool summary_has_dll_mode(const char *summary) {
  return summary && std::string(summary).find("\"xdvipdfmxMode\":\"dll\"") != std::string::npos;
}

static void print_snapshot(StemTeXRenderer *renderer, const char *label) {
  StemTeXEngineSnapshot snapshot{};
  if (!stemtex_renderer_engine_snapshot(renderer, &snapshot)) {
    std::printf("%s snapshot=unavailable\n", label);
    return;
  }
  std::printf("%s status=%d primary=%d spare=%d/%d rebuilding=%d lastError=%d\n", label, (int)snapshot.status,
              snapshot.primary_ready, snapshot.spare_ready, snapshot.spare_target, snapshot.spare_rebuilding,
              (int)snapshot.last_error);
}

static StemTeXEngineSnapshot get_snapshot(StemTeXRenderer *renderer) {
  StemTeXEngineSnapshot snapshot{};
  stemtex_renderer_engine_snapshot(renderer, &snapshot);
  return snapshot;
}

static void wait_for_spares(StemTeXRenderer *renderer, int seconds) {
  long long deadline = now_ms() + seconds * 1000LL;
  while (now_ms() < deadline) {
    StemTeXEngineSnapshot snapshot = get_snapshot(renderer);
    if (snapshot.primary_ready && snapshot.spare_ready >= snapshot.spare_target && !snapshot.spare_rebuilding) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

int main(int argc, char **argv) {
  fs::path repo_root = argc > 1 && argv[1] && *argv[1] ? fs::absolute(argv[1]) : fs::current_path();
  fs::path runtime_root = argc > 2 && argv[2] && *argv[2] ? fs::absolute(argv[2]) : fs::absolute(default_runtime_root(repo_root));
  std::string repo_root_utf8 = repo_root.generic_string();
  std::string runtime_root_utf8 = runtime_root.generic_string();

  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = repo_root_utf8.c_str();
  cfg.runtime_root_utf8 = runtime_root_utf8.c_str();
  int runs = argc > 3 ? std::atoi(argv[3]) : 1;
  std::string case_name = argc > 4 ? argv[4] : "";
  cfg.spare_worker_count = argc > 5 ? std::atoi(argv[5]) : 1;
  cfg.warmup_tex_utf8 = argc > 6 ? argv[6] : nullptr;
  cfg.worker_template_utf8 = argc > 7 ? argv[7] : nullptr;
  cfg.preamble_tex_utf8 = argc > 8 ? argv[8] : nullptr;
  cfg.request_timeout_ms = 90000;
  cfg.xdvipdfmx_timeout_ms = 90000;
  if (runs <= 0) runs = 1;
  bool expect_dll = case_name != "--allow-exe";
  if (case_name == "--allow-exe") case_name.clear();

  std::printf("repoRoot=%s\n", repo_root_utf8.c_str());
  std::printf("runtimeRoot=%s\n", runtime_root_utf8.c_str());
  std::printf("runtimeHasXetexdaemon=%d runtimeHasDvipdfmxDaemonDll=%d runtimeHasWarmup=%d\n",
              fs::exists(runtime_root / "bin" / "windows" / "xetexdaemon.exe") ? 1 : 0,
              fs::exists(runtime_root / "bin" / "windows" / "dvipdfmxdaemon.dll") ? 1 : 0,
              fs::exists(runtime_root / "texmf-var" / "cache-warmup" / "warmup.xdv") ? 1 : 0);

  char *error = nullptr;
  StemTeXErrorCode error_code = STEMTEX_OK;
  if (case_name == "--validate") {
    int ok = stemtex_renderer_validate_config(&cfg, &error_code, &error);
    std::printf("validate=%d code=%d diagnostics=%s\n", ok, (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return ok ? 0 : 1;
  }
  if (case_name == "--refresh") {
    int ok = stemtex_refresh_font_cache(cfg.runtime_root_utf8, nullptr, &error_code, &error);
    std::printf("refresh=%d code=%d error=%s\n", ok, (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return ok ? 0 : 1;
  }
  long long create_start = now_ms();
  StemTeXRenderer *renderer = stemtex_renderer_create(&cfg, &error_code, &error);
  long long create_end = now_ms();
  if (!renderer) {
    std::fprintf(stderr, "create failed code=%d: %s\n", (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return 1;
  }
  std::printf("createMs=%lld\n", create_end - create_start);
  std::printf("status=%d version=%s abi=%s\n", (int)stemtex_renderer_status(renderer), stemtex_renderer_version(),
              stemtex_renderer_abi_version());
  char *runtime_version = stemtex_renderer_runtime_version(renderer);
  std::printf("runtime=%s\n", runtime_version ? runtime_version : "");
  stemtex_renderer_free_string(runtime_version);

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
  } else if (case_name == "--bad-then-good-wait-long") {
    bad_then_good = true;
    std::this_thread::sleep_for(std::chrono::seconds(25));
    print_snapshot(renderer, "beforeFailure");
    snippet = u8"\u4e2d\u6587 error test: \\undefinedstemtexcommand";
  } else if (case_name == "--bad-stress") {
    wait_for_spares(renderer, 90);
    print_snapshot(renderer, "beforeBadStress");
    const char *bad = u8"\u4e2d\u6587 repeated error test: \\undefinedstemtexcommand";
    int failures = 0;
    for (int i = 0; i < runs; ++i) {
      print_snapshot(renderer, ("beforeBadStress" + std::to_string(i + 1)).c_str());
      StemTeXRenderResult result{};
      long long render_start = now_ms();
      int ok = stemtex_renderer_render(renderer, bad, 360, &result, &error_code, &error);
      long long render_end = now_ms();
      if (ok) {
        std::printf("badStress run=%d unexpected success renderMs=%lld pdf=%s\n", i + 1, render_end - render_start,
                    result.pdf_path_utf8 ? result.pdf_path_utf8 : "");
        stemtex_renderer_free_result(&result);
      } else {
        failures += 1;
        std::printf("badStress run=%d failed code=%d renderMs=%lld\n", i + 1, (int)error_code,
                    render_end - render_start);
        stemtex_renderer_free_string(error);
        error = nullptr;
      }
      print_snapshot(renderer, ("afterBadStress" + std::to_string(i + 1)).c_str());
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    stemtex_renderer_destroy(renderer);
    return failures == runs ? 0 : 1;
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
  } else if (case_name == "--latin-math") {
    snippet = u8"Latin math only: $E=mc^2$ "
              u8"\\[\\int_0^1 x^2\\,dx=\\frac13\\quad \\alpha+\\beta=\\gamma\\]";
  } else if (case_name == "--latin-text") {
    snippet = u8"Latin text only: quick brown fox, bold italic mono. "
              u8"\\textbf{bold} \\textit{italic} \\texttt{mono}.";
  } else if (case_name == "--restart") {
    if (!stemtex_renderer_restart(renderer, &error_code, &error)) {
      std::fprintf(stderr, "restart failed code=%d: %s\n", (int)error_code, error ? error : "");
      stemtex_renderer_free_string(error);
      stemtex_renderer_destroy(renderer);
      return 1;
    }
    std::printf("restart=ok status=%d\n", (int)stemtex_renderer_status(renderer));
  } else if (case_name == "--async") {
    struct AsyncState {
      std::mutex mu;
      std::condition_variable cv;
      int callbacks = 0;
      int failures = 0;
      uint64_t latest_job = 0;
      uint64_t latest_success = 0;
    } state;
    auto callback = [](uint64_t job_id, int ok, const StemTeXRenderResult *result, StemTeXErrorCode code, const char *err, void *data) {
      auto *state = static_cast<AsyncState *>(data);
      if (ok) {
        std::printf("async job=%llu pdf=%s\n", (unsigned long long)job_id,
                    result && result->pdf_path_utf8 ? result->pdf_path_utf8 : "");
      } else {
        std::printf("async job=%llu failed code=%d err=%s\n", (unsigned long long)job_id, (int)code, err ? err : "");
      }
      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->callbacks += 1;
        if (!ok) state->failures += 1;
        if (ok) state->latest_success = job_id;
      }
      state->cv.notify_all();
    };
    for (int i = 0; i < runs; ++i) {
      uint64_t job_id = 0;
      if (!stemtex_renderer_render_async(renderer, snippet, 360, &job_id, callback, &state, &error_code, &error)) {
        std::fprintf(stderr, "async submit failed code=%d: %s\n", (int)error_code, error ? error : "");
        stemtex_renderer_free_string(error);
        stemtex_renderer_destroy(renderer);
        return 1;
      }
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.latest_job = job_id;
      }
    }
    std::unique_lock<std::mutex> lock(state.mu);
    state.cv.wait_for(lock, std::chrono::seconds(90), [&]() { return state.callbacks >= runs; });
    std::printf("async callbacks=%d failures=%d latestJob=%llu latestSuccess=%llu\n", state.callbacks, state.failures,
                (unsigned long long)state.latest_job, (unsigned long long)state.latest_success);
    int ok = state.callbacks == runs && state.latest_success == state.latest_job;
    stemtex_renderer_destroy(renderer);
    return ok ? 0 : 1;
  } else if (case_name == "--cancel") {
    struct CancelState {
      std::mutex mu;
      std::condition_variable cv;
      int callbacks = 0;
      StemTeXErrorCode code = STEMTEX_OK;
    } state;
    auto callback = [](uint64_t job_id, int ok, const StemTeXRenderResult *, StemTeXErrorCode code, const char *err, void *data) {
      auto *state = static_cast<CancelState *>(data);
      std::printf("cancel callback job=%llu ok=%d code=%d err=%s\n", (unsigned long long)job_id, ok, (int)code,
                  err ? err : "");
      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->callbacks += 1;
        state->code = code;
      }
      state->cv.notify_all();
    };
    const char *hang = "\\loop\\iftrue\\repeat";
    uint64_t job_id = 0;
    if (!stemtex_renderer_render_async(renderer, hang, 360, &job_id, callback, &state, &error_code, &error)) {
      std::fprintf(stderr, "cancel submit failed code=%d: %s\n", (int)error_code, error ? error : "");
      stemtex_renderer_free_string(error);
      stemtex_renderer_destroy(renderer);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!stemtex_renderer_cancel_current(renderer, &error_code, &error)) {
      std::fprintf(stderr, "cancel failed code=%d: %s\n", (int)error_code, error ? error : "");
      stemtex_renderer_free_string(error);
      stemtex_renderer_destroy(renderer);
      return 1;
    }
    std::unique_lock<std::mutex> lock(state.mu);
    state.cv.wait_for(lock, std::chrono::seconds(90), [&]() { return state.callbacks > 0; });
    int ok = state.callbacks == 1 && state.code == STEMTEX_ERROR_CANCELLED;
    lock.unlock();
    stemtex_renderer_destroy(renderer);
    return ok ? 0 : 1;
  } else if (case_name == "--recover-no-worker") {
    stemtex_renderer_cancel_current(renderer, &error_code, &error);
    stemtex_renderer_free_string(error);
    error = nullptr;
    StemTeXRenderResult result{};
    long long render_start = now_ms();
    int ok = stemtex_renderer_render(renderer, snippet, 360, &result, &error_code, &error);
    long long render_end = now_ms();
    if (!ok) {
      std::fprintf(stderr, "recover render failed code=%d: %s\n", (int)error_code, error ? error : "");
      stemtex_renderer_free_string(error);
      stemtex_renderer_destroy(renderer);
      return 1;
    }
    std::printf("recover renderMs=%lld pdf=%s summary=%s\n", render_end - render_start,
                result.pdf_path_utf8 ? result.pdf_path_utf8 : "",
                result.summary_json_utf8 ? result.summary_json_utf8 : "");
    stemtex_renderer_free_result(&result);
    stemtex_renderer_destroy(renderer);
    return 0;
  }

  for (int i = 0; i < runs; ++i) {
    StemTeXRenderResult result{};
    StemTeXPdfBytes pdf{};
    long long render_start = now_ms();
    int ok = case_name == "--bytes"
                 ? stemtex_renderer_render_pdf_bytes(renderer, snippet, 360, &pdf, &result, &error_code, &error)
                 : stemtex_renderer_render(renderer, snippet, 360, &result, &error_code, &error);
    if (!ok) {
      long long render_end = now_ms();
      std::fprintf(stderr, "run=%d renderMs=%lld\n", i + 1, render_end - render_start);
      std::fprintf(stderr, "render failed code=%d: %s\n", (int)error_code, error ? error : "");
      print_snapshot(renderer, "afterFailure");
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
    std::printf("run=%d renderMs=%lld pdf=%s pdfBytes=%zu\nsummary=%s\n", i + 1, render_end - render_start,
                result.pdf_path_utf8, pdf.size, result.summary_json_utf8);
    print_snapshot(renderer, "afterSuccess");
    if (expect_dll && !summary_has_dll_mode(result.summary_json_utf8)) {
      std::fprintf(stderr, "expected xdvipdfmxMode=dll, got summary=%s\n",
                   result.summary_json_utf8 ? result.summary_json_utf8 : "");
      stemtex_renderer_free_pdf_bytes(&pdf);
      stemtex_renderer_free_result(&result);
      stemtex_renderer_destroy(renderer);
      return 1;
    }
    stemtex_renderer_free_pdf_bytes(&pdf);
    stemtex_renderer_free_result(&result);
  }
  char *tail = stemtex_renderer_get_log_tail(renderer, 512);
  std::printf("logTailBytes=%zu lastError=%d\n", tail ? std::string(tail).size() : 0,
              (int)stemtex_renderer_last_error_code(renderer));
  stemtex_renderer_free_string(tail);

  stemtex_renderer_destroy(renderer);
  return 0;
}
