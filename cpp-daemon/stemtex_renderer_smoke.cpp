#include "stemtex_renderer.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static fs::path default_runtime_root(const fs::path &repo_root) {
  fs::path side_tree = repo_root / "dist" / "stemtex-texlive-daemon-static";
  if (fs::exists(side_tree / "bin" / "windows" / "stemtex-worker-host.exe") &&
      fs::exists(side_tree / "bin" / "windows" / "xetexdaemon.exe")) {
    return side_tree;
  }
  return fs::path();
}

static void print_startup_progress(StemTeXStartupStage stage, const char *message_utf8, void *) {
  std::printf("startupStage=%d message=%s\n", (int)stage, message_utf8 ? message_utf8 : "");
  std::fflush(stdout);
}

struct SmokeOptions {
  fs::path repo_root = fs::current_path();
  fs::path runtime_root;
  fs::path texmf_root;
  fs::path profile_root;
  int runs = 1;
  std::string case_name;
  int spare_workers = 0;
  double width_pt = 360.0;
  double font_size_pt = 10.0;
  std::string worker_template;
  bool allow_exe = false;
  bool default_state = false;
};

static void print_usage(const char *argv0) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s [--repo PATH] [--runtime PATH] [--texmf PATH] --profile PATH [--runs N] [--case NAME] [--spares N] [--width PT] [--font-size PT] [--default-state]\n"
               "  %s --profile PATH --case async --runs 5 --spares 2\n"
               "\n"
               "Cases: default, validate, refresh, clear-xdv-cache, physics, fonts, chem-text, bad,\n"
               "       bad-then-good, bad-then-good-wait, bad-then-good-wait-long,\n"
               "       bad-stress, latin-math, latin-text, restart, async,\n"
               "       async-callback-throw, cancel,\n"
               "       recover-no-worker, bad-corpus, bad-output-corpus, list-state, lifecycle-stress,\n"
               "       bytes, output-pdf, output-pdf-bytes,\n"
               "       svg, svg-bytes\n",
               argv0, argv0);
}

static std::string canonical_case(std::string value) {
  if (value.empty() || value == "default") return "";
  if (value.rfind("--", 0) != 0) value = "--" + value;
  return value;
}

static SmokeOptions parse_options(int argc, char **argv) {
  SmokeOptions opts;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i] ? argv[i] : "";
    auto need_value = [&](const char *name) -> const char * {
      if (i + 1 >= argc || !argv[i + 1]) {
        print_usage(argv[0]);
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "--repo") {
      opts.repo_root = fs::absolute(need_value("--repo"));
    } else if (arg == "--runtime") {
      opts.runtime_root = fs::absolute(need_value("--runtime"));
    } else if (arg == "--texmf") {
      opts.texmf_root = fs::absolute(need_value("--texmf"));
    } else if (arg == "--profile") {
      opts.profile_root = fs::absolute(need_value("--profile"));
    } else if (arg == "--runs") {
      opts.runs = std::atoi(need_value("--runs"));
    } else if (arg == "--case") {
      opts.case_name = canonical_case(need_value("--case"));
    } else if (arg == "--spares") {
      opts.spare_workers = std::atoi(need_value("--spares"));
    } else if (arg == "--width") {
      opts.width_pt = std::atof(need_value("--width"));
    } else if (arg == "--font-size") {
      opts.font_size_pt = std::atof(need_value("--font-size"));
    } else if (arg == "--worker-template") {
      opts.worker_template = need_value("--worker-template");
    } else if (arg == "--allow-exe") {
      opts.allow_exe = true;
    } else if (arg == "--default-state") {
      opts.default_state = true;
    } else {
      print_usage(argv[0]);
      throw std::runtime_error("unexpected argument: " + arg);
    }
  }

  opts.repo_root = fs::absolute(opts.repo_root);
  if (opts.runtime_root.empty()) opts.runtime_root = default_runtime_root(opts.repo_root);
  if (opts.runtime_root.empty()) throw std::runtime_error("runtime root is required; pass --runtime");
  opts.runtime_root = fs::absolute(opts.runtime_root);
  if (opts.texmf_root.empty()) opts.texmf_root = opts.runtime_root;
  return opts;
}

static bool summary_has_dll_mode(const char *summary) {
  if (!summary) return false;
  std::string text(summary);
  return text.find("\"xdvipdfmxMode\":\"daemon-dll\"") != std::string::npos ||
         text.find("\"xdvipdfmxMode\":\"dll\"") != std::string::npos ||
         text.find("\"xdvipdfmxMode\":\"process-isolated\"") != std::string::npos;
}

static bool summary_has_svg_dll_mode(const char *summary) {
  if (!summary) return false;
  std::string text(summary);
  return text.find("\"outputFormat\":\"svg\"") != std::string::npos &&
         text.find("\"backend\":\"dvisvgmdaemon\"") != std::string::npos &&
         text.find("\"dvisvgmMode\":\"daemon-dll\"") != std::string::npos;
}

static void print_snapshot(StemTeXRenderer *renderer, const char *label) {
  StemTeXEngineSnapshot snapshot{};
  if (!stemtex_renderer_engine_snapshot(renderer, &snapshot)) {
    std::printf("%s snapshot=unavailable\n", label);
    return;
  }
  std::printf("%s status=%d stage=%d primary=%d spare=%d/%d rebuilding=%d async=%d pending=%d runningJob=%llu pendingJob=%llu lastError=%d\n",
              label, (int)snapshot.status, (int)snapshot.stage, snapshot.primary_ready, snapshot.spare_ready,
              snapshot.spare_target, snapshot.spare_rebuilding, snapshot.async_running, snapshot.async_pending,
              (unsigned long long)snapshot.running_job_id, (unsigned long long)snapshot.pending_job_id,
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
  SmokeOptions opts;
  try {
    opts = parse_options(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 2;
  }
  fs::path repo_root = opts.repo_root;
  fs::path runtime_root = opts.runtime_root;
  fs::path texmf_root = opts.texmf_root.empty() ? runtime_root : opts.texmf_root;
  fs::path profile_root = opts.profile_root;
  std::string repo_root_utf8 = repo_root.generic_string();
  std::string runtime_root_utf8 = runtime_root.generic_string();
  std::string texmf_root_utf8 = texmf_root.generic_string();
  std::string profile_root_utf8 = profile_root.empty() ? "" : profile_root.generic_string();
  std::string state_root_utf8 = (repo_root / "build" / "smoke-state").generic_string();
  std::string renders_root_utf8 = (repo_root / "build" / "smoke-renders").generic_string();

  StemTeXConfig cfg{};
  cfg.repo_root_utf8 = repo_root_utf8.c_str();
  cfg.runtime_root_utf8 = runtime_root_utf8.c_str();
  cfg.texmf_root_utf8 = texmf_root_utf8.c_str();
  cfg.profile_root_utf8 = profile_root_utf8.empty() ? nullptr : profile_root_utf8.c_str();
  cfg.state_root_utf8 = opts.default_state ? nullptr : state_root_utf8.c_str();
  cfg.renders_root_utf8 = renders_root_utf8.c_str();
  int runs = opts.runs;
  std::string case_name = opts.case_name;
  double width_pt = opts.width_pt;
  double font_size_pt = opts.font_size_pt;
  cfg.spare_worker_count = opts.spare_workers;
  cfg.worker_template_utf8 = opts.worker_template.empty() ? nullptr : opts.worker_template.c_str();
  cfg.request_timeout_ms = 90000;
  cfg.xdvipdfmx_timeout_ms = 90000;
  if (runs <= 0) runs = 1;
  bool expect_dll = !opts.allow_exe;

  std::printf("repoRoot=%s\n", repo_root_utf8.c_str());
  std::printf("runtimeRoot=%s\n", runtime_root_utf8.c_str());
  std::printf("texmfRoot=%s\n", texmf_root_utf8.c_str());
  std::printf("profileRoot=%s\n", profile_root_utf8.c_str());
  std::printf("widthPt=%.6g\n", width_pt);
  std::printf("fontSizePt=%.6g\n", font_size_pt);
  std::printf("runtimeHasWorkerHost=%d runtimeHasXetexdaemon=%d runtimeHasDvipdfmxDaemonDll=%d profileHasWarmup=%d profileHasWarmupXdv=%d\n",
              fs::exists(runtime_root / "bin" / "windows" / "stemtex-worker-host.exe") ? 1 : 0,
              fs::exists(runtime_root / "bin" / "windows" / "xetexdaemon.exe") ? 1 : 0,
              fs::exists(runtime_root / "bin" / "windows" / "dvipdfmxdaemon.dll") ? 1 : 0,
              fs::exists(profile_root / "warmup.tex") ? 1 : 0,
              fs::exists(profile_root / "warmup.xdv") ? 1 : 0);

  char *error = nullptr;
  StemTeXErrorCode error_code = STEMTEX_OK;
  if (case_name == "--validate") {
    int ok = stemtex_renderer_validate_config(&cfg, &error_code, &error);
    std::printf("validate=%d code=%d diagnostics=%s\n", ok, (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return ok ? 0 : 1;
  }
  if (case_name == "--refresh") {
    int ok = stemtex_refresh_font_cache(cfg.runtime_root_utf8, cfg.profile_root_utf8, &error_code, &error);
    std::printf("refresh=%d code=%d error=%s\n", ok, (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return ok ? 0 : 1;
  }
  if (case_name == "--clear-xdv-cache") {
    int ok = stemtex_renderer_clear_profile_caches(&cfg, &error_code, &error);
    std::printf("clearXdvCache=%d code=%d error=%s\n", ok, (int)error_code, error ? error : "");
    stemtex_renderer_free_string(error);
    return ok ? 0 : 1;
  }
  long long create_start = now_ms();
  StemTeXRenderer *renderer = stemtex_renderer_create_with_progress(
      &cfg, print_startup_progress, nullptr, &error_code, &error);
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

  if (case_name == "--lifecycle-stress") {
    stemtex_renderer_destroy(renderer);
    renderer = nullptr;
    const char *probe = "Lifecycle probe: $E=mc^2$ \\[\\int_0^1 x^2\\,dx=\\frac13\\]";
    int failures = 0;
    for (int i = 0; i < runs; ++i) {
      error = nullptr;
      long long loop_create_start = now_ms();
      StemTeXRenderer *loop_renderer = stemtex_renderer_create(&cfg, &error_code, &error);
      long long loop_create_end = now_ms();
      if (!loop_renderer) {
        ++failures;
        std::printf("lifecycle run=%d createOk=0 code=%d createMs=%lld err=%s\n", i + 1, (int)error_code,
                    loop_create_end - loop_create_start, error ? error : "");
        stemtex_renderer_free_string(error);
        continue;
      }
      StemTeXRenderResult result{};
      long long render_start = now_ms();
      int ok = stemtex_renderer_render(loop_renderer, probe, width_pt, &result, &error_code, &error);
      long long render_end = now_ms();
      std::printf("lifecycle run=%d createOk=1 createMs=%lld renderOk=%d code=%d renderMs=%lld\n", i + 1,
                  loop_create_end - loop_create_start, ok, (int)error_code, render_end - render_start);
      if (ok) {
        stemtex_renderer_free_result(&result);
      } else {
        ++failures;
        std::printf("lifecycle run=%d err=%s\n", i + 1, error ? error : "");
        stemtex_renderer_free_string(error);
        error = nullptr;
      }
      stemtex_renderer_destroy(loop_renderer);
    }
    std::printf("lifecycle passed=%d failed=%d total=%d\n", runs - failures, failures, runs);
    return failures == 0 ? 0 : 1;
  }

  const char *snippet =
      "StemTeX C++ DLL smoke test: $E=mc^2$ "
      "\\[\\int_0^1 x^2\\,dx=\\frac13\\]";
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
      int ok = stemtex_renderer_render(renderer, bad, width_pt, &result, &error_code, &error);
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
  } else if (case_name == "--bad-output-corpus") {
    struct BadCase {
      const char *name;
      const char *snippet;
    };
    struct FormatCase {
      StemTeXOutputFormat format;
      const char *name;
    };
    const std::vector<BadCase> cases = {
        {"missing-brace-exp", "$e^{L_p$"},
        {"undefined-command", "$\\notacommand{x}$"},
        {"mathbf-text-mode", "\\mathbf{Circulant matrix}"},
        {"mathrm-text-mode", "\\mathrm{Roman text}"},
        {"mathbb-text-mode", "\\mathbb{R}"},
        {"operatorname-text-mode", "\\operatorname{rank}"},
        {"fontdimen-then-error", "\\fontdimen2\\font=123pt \\errmessage{STEMTEX forced after fontdimen}"},
        {"native-font-then-error",
         "\\font\\stemtexrollbackfont=\"[lmmonoltcond10-oblique.otf]\" at 9pt "
         "\\stemtexrollbackfont polluted\\errmessage{STEMTEX forced after native font load}"},
        {"hyphenation-then-error", "\\hyphenation{stem-tex-hot-state}\\errmessage{STEMTEX forced after hyphenation}"},
        {"unclosed-enumerate", "\\begin{enumerate}\\item leaked"},
        {"unclosed-itemize", "\\begin{itemize}\\item leaked"},
        {"mismatched-list-end", "\\begin{enumerate}\\item one\\end{itemize}"},
        {"open-textbf", "This is \\textbf{unfinished"},
        {"input-missing-file", "\\input{definitely-not-existing-file}"},
    };
    const std::vector<FormatCase> formats = {
        {STEMTEX_OUTPUT_PDF, "pdf"},
        {STEMTEX_OUTPUT_SVG, "svg"},
    };
    const char *good = "Recovered fragment: \\textbf{Circulant matrix}. $E=mc^2$.";
    int passed = 0;
    int failed = 0;
    wait_for_spares(renderer, 30);
    print_snapshot(renderer, "beforeBadOutputCorpus");
    for (const auto &fmt : formats) {
      for (const auto &bad_case : cases) {
        StemTeXRenderOutputResult bad_result{};
        long long bad_start = now_ms();
        int bad_ok = stemtex_renderer_render_output(renderer, bad_case.snippet, width_pt, fmt.format, &bad_result,
                                                    &error_code, &error);
        long long bad_end = now_ms();
        bool bad_expected = !bad_ok && error_code == STEMTEX_ERROR_TEX_SNIPPET;
        std::printf("badOutput format=%s case=%s badOk=%d code=%d ms=%lld\n", fmt.name, bad_case.name, bad_ok,
                    (int)error_code, bad_end - bad_start);
        if (bad_ok) {
          std::printf("badOutput format=%s case=%s unexpectedOutput=%s\n", fmt.name, bad_case.name,
                      bad_result.output_path_utf8 ? bad_result.output_path_utf8 : "");
          stemtex_renderer_free_output_result(&bad_result);
        } else {
          if (error) {
            std::string err(error);
            size_t nl = err.find('\n');
            if (nl != std::string::npos) err.resize(nl);
            std::printf("badOutput format=%s case=%s error=%s\n", fmt.name, bad_case.name, err.c_str());
          }
          stemtex_renderer_free_string(error);
          error = nullptr;
        }

        StemTeXEngineSnapshot after_bad = get_snapshot(renderer);
        bool worker_kept = after_bad.status == STEMTEX_STATUS_READY && after_bad.primary_ready &&
                           after_bad.spare_ready == after_bad.spare_target && !after_bad.spare_rebuilding;

        StemTeXRenderOutputResult good_result{};
        long long good_start = now_ms();
        int good_ok = stemtex_renderer_render_output(renderer, good, width_pt, fmt.format, &good_result, &error_code,
                                                     &error);
        long long good_end = now_ms();
        if (good_ok) {
          std::printf("badOutput format=%s case=%s recoveryOk=1 ms=%lld output=%s\n", fmt.name, bad_case.name,
                      good_end - good_start, good_result.output_path_utf8 ? good_result.output_path_utf8 : "");
          stemtex_renderer_free_output_result(&good_result);
        } else {
          std::printf("badOutput format=%s case=%s recoveryOk=0 code=%d ms=%lld err=%s\n", fmt.name, bad_case.name,
                      (int)error_code, good_end - good_start, error ? error : "");
          stemtex_renderer_free_string(error);
          error = nullptr;
        }

        StemTeXEngineSnapshot after_good = get_snapshot(renderer);
        bool recovery_ready = after_good.status == STEMTEX_STATUS_READY && after_good.primary_ready;
        if (bad_expected && worker_kept && good_ok && recovery_ready) {
          passed += 1;
        } else {
          failed += 1;
        }
      }
    }
    char *tail = stemtex_renderer_get_log_tail(renderer, 4096);
    std::printf("badOutput passed=%d failed=%d total=%zu\n", passed, failed, cases.size() * formats.size());
    std::printf("badOutput logTail:\n%s\n", tail ? tail : "");
    stemtex_renderer_free_string(tail);
    stemtex_renderer_destroy(renderer);
    return failed == 0 ? 0 : 1;
  } else if (case_name == "--bad-corpus" || case_name == "--checkpoint-critical") {
    struct BadCase {
      const char *name;
      const char *snippet;
      const char *recovery_probe;
    };
    const std::vector<BadCase> cases = {
        {"missing-brace-exp", "$e^{L_p$"},
        {"missing-frac-brace", "$\\frac{1}{2$"},
        {"bad-sqrt-option", "$\\sqrt[3{x}$"},
        {"missing-right-delimiter", "$\\left( x + y$"},
        {"bad-matrix-row", "$\\begin{matrix} a & b \\\\ c \\end{pmatrix}$"},
        {"unterminated-aligned", "$\\begin{aligned} a &= b \\\\ c &= d$"},
        {"undefined-command", "$\\notacommand{x}$"},
        {"undefined-text-command", "\\unknownmacro"},
        {"mathbf-text-mode", "\\mathbf{Circulant matrix}"},
        {"mathrm-text-mode", "\\mathrm{Roman text}"},
        {"mathit-text-mode", "\\mathit{Italic text}"},
        {"mathsf-text-mode", "\\mathsf{Sans text}"},
        {"mathbb-text-mode", "\\mathbb{R}"},
        {"boldsymbol-text-mode", "\\boldsymbol{x}"},
        {"operatorname-text-mode", "\\operatorname{rank}"},
        {"orphan-end", "\\end{equation}"},
        {"wrong-env-end", "\\begin{array}{cc} a & b \\end{matrix}"},
        {"fontdimen-then-error", "\\fontdimen2\\font=123pt \\errmessage{STEMTEX forced after fontdimen}",
         "\\ifdim\\fontdimen2\\font=123pt \\errmessage{STEMTEX fontdimen leaked}\\fi fontdimen ok"},
        {"native-font-then-error",
         "\\font\\stemtexrollbackfont=\"[lmmonoltcond10-oblique.otf]\" at 9pt "
         "\\stemtexrollbackfont polluted\\errmessage{STEMTEX forced after native font load}"},
        {"hyphenation-then-error", "\\hyphenation{stem-tex-hot-state}\\errmessage{STEMTEX forced after hyphenation}"},
        {"sparse-register-then-error",
         "\\count32767=12345 \\toks32767={polluted}\\errmessage{STEMTEX forced after sparse register}",
         "\\ifnum\\count32767=12345 \\errmessage{STEMTEX sparse register leaked}\\fi sparse ok"},
        {"missing-frac-arg", "$\\frac{1}$"},
        {"missing-overset-arg", "$\\overset{a}$"},
        {"subscript-text-mode", "_abc"},
        {"superscript-text-mode", "^abc"},
        {"item-outside-list", "\\item hello"},
        {"unclosed-enumerate", "\\begin{enumerate}\\item leaked"},
        {"unclosed-itemize", "\\begin{itemize}\\item leaked"},
        {"mismatched-list-end", "\\begin{enumerate}\\item one\\end{itemize}"},
        {"cr-outside-alignment", "\\cr"},
        {"extra-close-brace", "hello }"},
        {"open-textbf", "This is \\textbf{unfinished"},
        {"open-group", "\\begingroup unfinished"},
        {"input-missing-file", "\\input{definitely-not-existing-file}"},
        {"missing-image", "\\includegraphics{definitely-not-existing-image.png}"},
        {"halign-in-math", "$\\halign{#\\cr a&b\\cr}$"},
    };
    const std::vector<BadCase> critical_cases = {
        {"undefined-command", "$\\notacommand{x}$"},
        {"mathbf-text-mode", "\\mathbf{Circulant matrix}"},
        {"mathrm-text-mode", "\\mathrm{Roman text}"},
        {"bad-matrix-row", "$\\begin{matrix} a & b \\\\ c \\end{pmatrix}$"},
        {"orphan-end", "\\end{equation}"},
        {"unclosed-enumerate", "\\begin{enumerate}\\item leaked"},
        {"fontdimen-then-error", "\\fontdimen2\\font=123pt \\errmessage{STEMTEX forced after fontdimen}",
         "\\ifdim\\fontdimen2\\font=123pt \\errmessage{STEMTEX fontdimen leaked}\\fi fontdimen ok"},
        {"native-font-then-error",
         "\\font\\stemtexrollbackfont=\"[lmmonoltcond10-oblique.otf]\" at 9pt "
         "\\stemtexrollbackfont polluted\\errmessage{STEMTEX forced after native font load}"},
        {"hyphenation-then-error", "\\hyphenation{stem-tex-hot-state}\\errmessage{STEMTEX forced after hyphenation}"},
        {"sparse-register-then-error",
         "\\count32767=12345 \\toks32767={polluted}\\errmessage{STEMTEX forced after sparse register}",
         "\\ifnum\\count32767=12345 \\errmessage{STEMTEX sparse register leaked}\\fi sparse ok"},
        {"open-textbf", "This is \\textbf{unfinished"},
        {"open-group", "\\begingroup unfinished"},
        {"input-missing-file", "\\input{definitely-not-existing-file}"},
    };
    const auto &selected_cases = case_name == "--checkpoint-critical" ? critical_cases : cases;
    const char *good =
        "StemTeX recovery probe: $E=mc^2$.\n\n"
        "\\begin{equation}\nE = mc^2\n\\end{equation}\n\n"
        "\\[\\int_0^1 x^2\\,dx = \\frac{1}{3},\\quad \\langle\\psi,\\phi\\rangle\\]";
    int passed = 0;
    int failed = 0;
    wait_for_spares(renderer, 30);
    print_snapshot(renderer, "beforeBadCorpus");
    for (size_t i = 0; i < selected_cases.size(); ++i) {
      StemTeXRenderResult bad_result{};
      long long bad_start = now_ms();
      int bad_ok = stemtex_renderer_render(renderer, selected_cases[i].snippet, width_pt, &bad_result, &error_code, &error);
      long long bad_end = now_ms();
      bool bad_expected = !bad_ok && error_code == STEMTEX_ERROR_TEX_SNIPPET;
      std::printf("badCorpus case=%zu name=%s badOk=%d code=%d ms=%lld\n", i + 1, selected_cases[i].name, bad_ok,
                  (int)error_code, bad_end - bad_start);
      if (bad_ok) {
        std::printf("badCorpus case=%s unexpectedPdf=%s\n", selected_cases[i].name,
                    bad_result.pdf_path_utf8 ? bad_result.pdf_path_utf8 : "");
        stemtex_renderer_free_result(&bad_result);
      } else {
        if (error) {
          std::string err(error);
          size_t nl = err.find('\n');
          if (nl != std::string::npos) err.resize(nl);
          std::printf("badCorpus case=%s error=%s\n", selected_cases[i].name, err.c_str());
        }
        stemtex_renderer_free_string(error);
        error = nullptr;
      }

      StemTeXEngineSnapshot after_bad = get_snapshot(renderer);
      std::printf("badCorpus case=%s afterBad status=%d stage=%d primary=%d spare=%d/%d rebuilding=%d\n",
                  selected_cases[i].name, (int)after_bad.status, (int)after_bad.stage, after_bad.primary_ready,
                  after_bad.spare_ready, after_bad.spare_target, after_bad.spare_rebuilding);

      StemTeXRenderResult good_result{};
      long long good_start = now_ms();
      const char *recovery = selected_cases[i].recovery_probe ? selected_cases[i].recovery_probe : good;
      int good_ok = stemtex_renderer_render(renderer, recovery, width_pt, &good_result, &error_code, &error);
      long long good_end = now_ms();
      if (good_ok) {
        std::printf("badCorpus case=%s recoveryOk=1 ms=%lld pdf=%s\n", selected_cases[i].name, good_end - good_start,
                    good_result.pdf_path_utf8 ? good_result.pdf_path_utf8 : "");
        stemtex_renderer_free_result(&good_result);
      } else {
        std::printf("badCorpus case=%s recoveryOk=0 code=%d ms=%lld err=%s\n", selected_cases[i].name, (int)error_code,
                    good_end - good_start, error ? error : "");
        stemtex_renderer_free_string(error);
        error = nullptr;
      }

      StemTeXEngineSnapshot after_good = get_snapshot(renderer);
      bool status_ok = after_good.status == STEMTEX_STATUS_READY || after_good.status == STEMTEX_STATUS_RESTARTING;
      if (bad_expected && good_ok && status_ok) {
        passed += 1;
      } else {
        failed += 1;
      }
    }
    char *tail = stemtex_renderer_get_log_tail(renderer, 4096);
    std::printf("badCorpus passed=%d failed=%d total=%zu\n", passed, failed, selected_cases.size());
    std::printf("badCorpus logTail:\n%s\n", tail ? tail : "");
    stemtex_renderer_free_string(tail);
    stemtex_renderer_destroy(renderer);
    return failed == 0 ? 0 : 1;
  } else if (case_name == "--list-state") {
    auto list_state_probe = [](const char *label) {
      return std::string("\\ifnum\\csname @listdepth\\endcsname=0 \\else\\errmessage{STEMTEX listdepth leaked at ") +
             label +
             "}\\fi"
             "\\ifnum\\csname @itemdepth\\endcsname=0 \\else\\errmessage{STEMTEX itemdepth leaked at " +
             label +
             "}\\fi"
             "\\ifnum\\csname @enumdepth\\endcsname=0 \\else\\errmessage{STEMTEX enumdepth leaked at " +
             label +
             "}\\fi"
             "\\ifnum\\csname c@enumi\\endcsname=0 \\else\\errmessage{STEMTEX enumi leaked at " +
             label +
             "}\\fi"
             "\\ifnum\\csname c@enumii\\endcsname=0 \\else\\errmessage{STEMTEX enumii leaked at " +
             label + "}\\fi"
             "\\typeout{STEMTEX_LIST_STATE_OK:" +
             label + "} state probe";
    };
    auto render_success = [&](const char *name, const std::string &body) {
      StemTeXRenderResult result{};
      long long render_start = now_ms();
      int ok = stemtex_renderer_render_with_font_size(renderer, body.c_str(), width_pt, font_size_pt, &result,
                                                      &error_code, &error);
      long long render_end = now_ms();
      if (ok) {
        std::printf("listState case=%s ok=1 code=%d ms=%lld pdf=%s\n", name, (int)error_code, render_end - render_start,
                    result.pdf_path_utf8 ? result.pdf_path_utf8 : "");
        stemtex_renderer_free_result(&result);
        return true;
      }
      std::printf("listState case=%s ok=0 code=%d ms=%lld err=%s\n", name, (int)error_code, render_end - render_start,
                  error ? error : "");
      stemtex_renderer_free_string(error);
      error = nullptr;
      return false;
    };
    auto render_tex_error = [&](const char *name, const std::string &body) {
      StemTeXRenderResult result{};
      long long render_start = now_ms();
      int ok = stemtex_renderer_render_with_font_size(renderer, body.c_str(), width_pt, font_size_pt, &result,
                                                      &error_code, &error);
      long long render_end = now_ms();
      bool expected = !ok && error_code == STEMTEX_ERROR_TEX_SNIPPET;
      std::printf("listState case=%s ok=%d code=%d expectedTexError=%d ms=%lld\n", name, ok, (int)error_code,
                  expected ? 1 : 0, render_end - render_start);
      if (ok) {
        stemtex_renderer_free_result(&result);
      } else {
        if (error) {
          std::string err(error);
          size_t nl = err.find('\n');
          if (nl != std::string::npos) err.resize(nl);
          std::printf("listState case=%s error=%s\n", name, err.c_str());
        }
        stemtex_renderer_free_string(error);
        error = nullptr;
      }
      return expected;
    };

    int failed = 0;
    failed += render_success("closed-nested-lists",
                             "\\begin{enumerate}\\item outer\\begin{enumerate}\\item inner\\end{enumerate}"
                             "\\item second\\end{enumerate}\\begin{itemize}\\item bullet\\end{itemize}")
                  ? 0
                  : 1;
    failed += render_success("probe-after-good", list_state_probe("after-good")) ? 0 : 1;
    failed += render_tex_error("unclosed-enumerate", "\\begin{enumerate}\\item leaked\\begin{itemize}\\item nested")
                  ? 0
                  : 1;
    StemTeXEngineSnapshot after_bad = get_snapshot(renderer);
    bool worker_ready = after_bad.status == STEMTEX_STATUS_READY && after_bad.primary_ready;
    std::printf("listState afterBad status=%d stage=%d primary=%d spare=%d/%d rebuilding=%d\n",
                (int)after_bad.status, (int)after_bad.stage, after_bad.primary_ready, after_bad.spare_ready,
                after_bad.spare_target, after_bad.spare_rebuilding);
    if (!worker_ready) ++failed;
    failed += render_success("probe-after-bad", list_state_probe("after-bad")) ? 0 : 1;
    failed += render_success("fresh-lists",
                             "\\begin{itemize}\\item fresh bullet\\end{itemize}"
                             "\\begin{enumerate}\\item fresh one\\item fresh two\\end{enumerate}")
                  ? 0
                  : 1;
    failed += render_success("probe-after-fresh", list_state_probe("after-fresh")) ? 0 : 1;

    std::printf("listState failed=%d\n", failed);
    stemtex_renderer_destroy(renderer);
    return failed == 0 ? 0 : 1;
  } else if (case_name == "--chem-text") {
    snippet = u8"\u6b63\u6587\u6a21\u5f0f\u5316\u5b66: "
              u8"\\ce{H2O}, \\ce{CO2}, \\ce{2H2 + O2 -> 2H2O}.";
  } else if (case_name == "--physics") {
    snippet = u8"\u7269\u7406\u516c\u5f0f: "
              u8"\\[\\ip{\\psi}{\\phi}\\quad \\dv{x}\\sin x=\\cos x\\quad \\vb{v}\\cdot\\vu{n}\\]";
  } else if (case_name == "--fonts") {
    snippet = u8"\u5b57\u4f53\u6d4b\u8bd5: \u4e2d\u6587\u5b8b\u4f53, "
              u8"{\\sffamily \u4e2d\u6587\u9ed1\u4f53}, "
              u8"\\textbf{bold \u4e0e \u7c97\u4f53\u4e2d\u6587}, \\textit{italic \u4e0e \u659c\u4f53\u4e2d\u6587}, \\texttt{mono}; "
              u8"$E=mc^2,\\;\\symbf{E},\\;\\mathbf{E}$.";
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
      if (!stemtex_renderer_render_async(renderer, snippet, width_pt, &job_id, callback, &state, &error_code, &error)) {
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
  } else if (case_name == "--async-callback-throw") {
    struct ThrowState {
      std::mutex mu;
      std::condition_variable cv;
      int callbacks = 0;
    } state;
    auto callback = [](uint64_t job_id, int ok, const StemTeXRenderResult *, StemTeXErrorCode code, const char *err,
                       void *data) {
      auto *state = static_cast<ThrowState *>(data);
      std::printf("throwing callback job=%llu ok=%d code=%d err=%s\n", (unsigned long long)job_id, ok, (int)code,
                  err ? err : "");
      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->callbacks += 1;
      }
      state->cv.notify_all();
      throw std::runtime_error("intentional async callback exception");
    };
    uint64_t job_id = 0;
    if (!stemtex_renderer_render_async(renderer, snippet, width_pt, &job_id, callback, &state, &error_code, &error)) {
      std::fprintf(stderr, "async submit failed code=%d: %s\n", (int)error_code, error ? error : "");
      stemtex_renderer_free_string(error);
      stemtex_renderer_destroy(renderer);
      return 1;
    }
    {
      std::unique_lock<std::mutex> lock(state.mu);
      state.cv.wait_for(lock, std::chrono::seconds(90), [&]() { return state.callbacks > 0; });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    StemTeXRenderResult result{};
    int ok = stemtex_renderer_render(renderer, snippet, width_pt, &result, &error_code, &error);
    if (!ok) {
      std::fprintf(stderr, "render after throwing callback failed code=%d: %s\n", (int)error_code, error ? error : "");
      stemtex_renderer_free_string(error);
    } else {
      stemtex_renderer_free_result(&result);
    }
    char *tail = stemtex_renderer_get_log_tail(renderer, 4096);
    bool logged = tail && std::string(tail).find("async callback threw") != std::string::npos;
    std::printf("asyncCallbackThrow callbacks=%d survived=%d logged=%d\n", state.callbacks, ok, logged ? 1 : 0);
    stemtex_renderer_free_string(tail);
    stemtex_renderer_destroy(renderer);
    return state.callbacks == 1 && ok && logged ? 0 : 1;
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
    if (!stemtex_renderer_render_async(renderer, hang, width_pt, &job_id, callback, &state, &error_code, &error)) {
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
    int ok = stemtex_renderer_render(renderer, snippet, width_pt, &result, &error_code, &error);
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
    bool output_case = case_name == "--output-pdf" || case_name == "--output-pdf-bytes" ||
                       case_name == "--svg" || case_name == "--svg-bytes";
    long long render_start = now_ms();
    if (output_case) {
      bool svg_case = case_name == "--svg" || case_name == "--svg-bytes";
      bool bytes_case = case_name == "--output-pdf-bytes" || case_name == "--svg-bytes";
      StemTeXOutputFormat format = svg_case ? STEMTEX_OUTPUT_SVG : STEMTEX_OUTPUT_PDF;
      StemTeXRenderOutputResult result{};
      StemTeXOutputBytes output{};
      int ok = bytes_case
                   ? stemtex_renderer_render_output_bytes_with_font_size(renderer, snippet, width_pt, font_size_pt,
                                                                         format, &output, &result, &error_code, &error)
                   : stemtex_renderer_render_output_with_font_size(renderer, snippet, width_pt, font_size_pt, format,
                                                                   &result, &error_code, &error);
      if (!ok) {
        long long render_end = now_ms();
        std::fprintf(stderr, "run=%d renderMs=%lld\n", i + 1, render_end - render_start);
        std::fprintf(stderr, "render failed code=%d: %s\n", (int)error_code, error ? error : "");
        print_snapshot(renderer, "afterFailure");
        stemtex_renderer_free_string(error);
        stemtex_renderer_destroy(renderer);
        return 1;
      }
      long long render_end = now_ms();
      std::printf("run=%d renderMs=%lld output=%s format=%s outputBytes=%zu\nsummary=%s\n", i + 1,
                  render_end - render_start, result.output_path_utf8 ? result.output_path_utf8 : "",
                  result.output_format_utf8 ? result.output_format_utf8 : "", output.size,
                  result.summary_json_utf8 ? result.summary_json_utf8 : "");
      print_snapshot(renderer, "afterSuccess");
      if (!result.output_path_utf8 || !fs::exists(result.output_path_utf8)) {
        std::fprintf(stderr, "expected output file, got path=%s\n",
                     result.output_path_utf8 ? result.output_path_utf8 : "");
        stemtex_renderer_free_output_bytes(&output);
        stemtex_renderer_free_output_result(&result);
        stemtex_renderer_destroy(renderer);
        return 1;
      }
      bool dll_summary_ok = svg_case ? summary_has_svg_dll_mode(result.summary_json_utf8)
                                     : summary_has_dll_mode(result.summary_json_utf8);
      if (expect_dll && !dll_summary_ok) {
        std::fprintf(stderr, "expected daemon-dll output summary, got summary=%s\n",
                     result.summary_json_utf8 ? result.summary_json_utf8 : "");
        stemtex_renderer_free_output_bytes(&output);
        stemtex_renderer_free_output_result(&result);
        stemtex_renderer_destroy(renderer);
        return 1;
      }
      stemtex_renderer_free_output_bytes(&output);
      stemtex_renderer_free_output_result(&result);
      continue;
    }

    StemTeXRenderResult result{};
    StemTeXPdfBytes pdf{};
    int ok = case_name == "--bytes"
                 ? stemtex_renderer_render_pdf_bytes_with_font_size(renderer, snippet, width_pt, font_size_pt, &pdf,
                                                                    &result, &error_code, &error)
                 : stemtex_renderer_render_with_font_size(renderer, snippet, width_pt, font_size_pt, &result,
                                                          &error_code, &error);
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
      snippet = "Recovery test: $E=mc^2$ "
                "\\[\\langle\\psi,\\phi\\rangle\\quad \\int_0^1 x^2\\,dx\\]";
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
