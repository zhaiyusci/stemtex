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
  if (fs::exists(side_tree / "bin" / "windows" / "xetexdaemon.exe")) {
    return side_tree;
  }
  return fs::path();
}

struct SmokeOptions {
  fs::path repo_root = fs::current_path();
  fs::path runtime_root;
  fs::path texmf_root;
  fs::path profile_root;
  int runs = 1;
  std::string case_name;
  int spare_workers = 0;
  std::string worker_template;
  bool allow_exe = false;
};

static void print_usage(const char *argv0) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s [--repo PATH] [--runtime PATH] [--texmf PATH] --profile PATH [--runs N] [--case NAME] [--spares N]\n"
               "  %s --profile PATH --case async --runs 5 --spares 2\n"
               "\n"
               "Cases: default, validate, refresh, physics, fonts, chem-text, bad,\n"
               "       bad-then-good, bad-then-good-wait, bad-then-good-wait-long,\n"
               "       bad-stress, latin-math, latin-text, restart, async, cancel,\n"
               "       recover-no-worker, bad-corpus, lifecycle-stress,\n"
               "       profile-switch-stress, bytes\n",
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
    } else if (arg == "--worker-template") {
      opts.worker_template = need_value("--worker-template");
    } else if (arg == "--allow-exe") {
      opts.allow_exe = true;
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
  cfg.state_root_utf8 = state_root_utf8.c_str();
  cfg.renders_root_utf8 = renders_root_utf8.c_str();
  int runs = opts.runs;
  std::string case_name = opts.case_name;
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
  std::printf("runtimeHasXetexdaemon=%d runtimeHasDvipdfmxDaemonDll=%d profileHasWarmup=%d profileHasWarmupXdv=%d\n",
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

  if (case_name == "--lifecycle-stress") {
    stemtex_renderer_destroy(renderer);
    renderer = nullptr;
    const char *probe = u8"Lifecycle probe: $E=mc^2$ \\[\\ce{H2O}\\]";
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
      int ok = stemtex_renderer_render(loop_renderer, probe, 360, &result, &error_code, &error);
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

  if (case_name == "--profile-switch-stress") {
    stemtex_renderer_destroy(renderer);
    renderer = nullptr;
    fs::path profile_parent = opts.profile_root.parent_path();
    std::vector<fs::path> profiles = {
        profile_parent / "unicodemath_cjk",
        profile_parent / "unicodemath",
    };
    int failures = 0;
    for (int i = 0; i < runs; ++i) {
      fs::path profile_path = profiles[(size_t)i % profiles.size()];
      std::string loop_profile_utf8 = profile_path.generic_string();
      StemTeXConfig loop_cfg = cfg;
      loop_cfg.profile_root_utf8 = loop_profile_utf8.c_str();
      const char *probe = profile_path.filename() == "unicodemath"
                              ? u8"Profile switch probe: $E=mc^2$ \\[\\ce{H2O}\\]"
                              : u8"\u4e2d\u6587 profile switch probe: $E=mc^2$ \\[\\ce{H2O}\\]";
      error = nullptr;
      long long loop_create_start = now_ms();
      StemTeXRenderer *loop_renderer = stemtex_renderer_create(&loop_cfg, &error_code, &error);
      long long loop_create_end = now_ms();
      if (!loop_renderer) {
        ++failures;
        std::printf("profileSwitch run=%d profile=%s createOk=0 code=%d createMs=%lld err=%s\n", i + 1,
                    loop_profile_utf8.c_str(), (int)error_code, loop_create_end - loop_create_start, error ? error : "");
        stemtex_renderer_free_string(error);
        continue;
      }
      StemTeXRenderResult result{};
      long long render_start = now_ms();
      int ok = stemtex_renderer_render(loop_renderer, probe, 360, &result, &error_code, &error);
      long long render_end = now_ms();
      std::printf("profileSwitch run=%d profile=%s createOk=1 createMs=%lld renderOk=%d code=%d renderMs=%lld\n", i + 1,
                  profile_path.filename().generic_string().c_str(), loop_create_end - loop_create_start, ok,
                  (int)error_code, render_end - render_start);
      if (ok) {
        stemtex_renderer_free_result(&result);
      } else {
        ++failures;
        std::printf("profileSwitch run=%d err=%s\n", i + 1, error ? error : "");
        stemtex_renderer_free_string(error);
        error = nullptr;
      }
      stemtex_renderer_destroy(loop_renderer);
    }
    std::printf("profileSwitch passed=%d failed=%d total=%d\n", runs - failures, failures, runs);
    return failures == 0 ? 0 : 1;
  }

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
  } else if (case_name == "--bad-corpus" || case_name == "--checkpoint-critical") {
    struct BadCase {
      const char *name;
      const char *snippet;
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
        {"orphan-end", "\\end{equation}"},
        {"wrong-env-end", "\\begin{array}{cc} a & b \\end{matrix}"},
        {"missing-frac-arg", "$\\frac{1}$"},
        {"missing-overset-arg", "$\\overset{a}$"},
        {"subscript-text-mode", "_abc"},
        {"superscript-text-mode", "^abc"},
        {"item-outside-list", "\\item hello"},
        {"cr-outside-alignment", "\\cr"},
        {"extra-close-brace", "hello }"},
        {"open-textcolor", u8"\u8fd9\u662f\u4e00\u6bb5\uff1a\\textcolor{blue}{\u84dd\u8272\u6587\u5b57"},
        {"open-group", "\\begingroup unfinished"},
        {"input-missing-file", "\\input{definitely-not-existing-file}"},
        {"missing-image", "\\includegraphics{definitely-not-existing-image.png}"},
        {"halign-in-math", "$\\halign{#\\cr a&b\\cr}$"},
    };
    const std::vector<BadCase> critical_cases = {
        {"undefined-command", "$\\notacommand{x}$"},
        {"bad-matrix-row", "$\\begin{matrix} a & b \\\\ c \\end{pmatrix}$"},
        {"orphan-end", "\\end{equation}"},
        {"open-textcolor", u8"\u8fd9\u662f\u4e00\u6bb5\uff1a\\textcolor{blue}{\u84dd\u8272\u6587\u5b57"},
        {"open-group", "\\begingroup unfinished"},
        {"input-missing-file", "\\input{definitely-not-existing-file}"},
    };
    const auto &selected_cases = case_name == "--checkpoint-critical" ? critical_cases : cases;
    const char *good =
        u8"\u8fd9\u662f\u4e00\u6bb5 StemTeX Renderer GUI \u91cc\u7684\u4e2d\u6587\u3001"
        u8"\u6570\u5b66\u548c\u5316\u5b66\u9884\u89c8\uff1a$E=mc^2$\uff0c"
        u8"\u4ee5\u53ca \\textcolor{blue}{\u84dd\u8272\u6587\u5b57}\u3002\n\n"
        u8"\\begin{equation}\nE = mc^2\n\\end{equation}\n\n"
        u8"\\[\\int_0^1 x^2\\,dx = \\frac{1}{3},\\quad \\langle\\psi,\\phi\\rangle\\]\n\n"
        u8"\\ce{2H2 + O2 -> 2H2O}";
    int passed = 0;
    int failed = 0;
    wait_for_spares(renderer, 30);
    print_snapshot(renderer, "beforeBadCorpus");
    for (size_t i = 0; i < selected_cases.size(); ++i) {
      StemTeXRenderResult bad_result{};
      long long bad_start = now_ms();
      int bad_ok = stemtex_renderer_render(renderer, selected_cases[i].snippet, 360, &bad_result, &error_code, &error);
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
      int good_ok = stemtex_renderer_render(renderer, good, 360, &good_result, &error_code, &error);
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
