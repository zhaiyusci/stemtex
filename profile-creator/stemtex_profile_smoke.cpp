#include "stemtex_profile.h"
#include "stemtex_renderer.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
  std::string texmf_root;
  std::string runtime_root;
  std::string selected_case;
  for (int index = 1; index < argc; ++index) {
    std::string option = argv[index];
    if (option == "--texmf" && index + 1 < argc) texmf_root = argv[++index];
    else if (option == "--runtime" && index + 1 < argc) runtime_root = argv[++index];
    else if (option == "--case" && index + 1 < argc) selected_case = argv[++index];
    else {
      std::cerr << "usage: stemtex-profile-smoke --texmf TEXLIVE_ROOT [--runtime STEMTEX_RUNTIME] [--case NAME]\n";
      return 2;
    }
  }
  if (texmf_root.empty()) {
    std::cerr << "usage: stemtex-profile-smoke --texmf TEXLIVE_ROOT [--runtime STEMTEX_RUNTIME] [--case NAME]\n";
    return 2;
  }

  StemTeXProfileContext context{};
  context.texmf_root_utf8 = texmf_root.c_str();
  StemTeXProfileErrorCode code = STEMTEX_PROFILE_OK;
  char *error = nullptr;
  char *catalog = stemtex_profile_font_catalog_json(&context, &code, &error);
  if (!catalog) {
    std::cerr << (error ? error : "catalog failed") << '\n';
    stemtex_profile_free_string(error);
    return 1;
  }
  std::cout << catalog << '\n';
  stemtex_profile_free_string(catalog);

  fs::path root = fs::temp_directory_path() / "stemtex-profile-smoke";
  std::error_code ignored;
  fs::remove_all(root, ignored);
  fs::create_directories(root);
  struct SmokeSpec {
    const char *name;
    const char *text;
    const char *math;
    const char *cjk;
  };
  const std::vector<SmokeSpec> cases = {
      {"math-arsenal", "latin-modern", "arsenal-math", "none"},
      {"math-asana", "latin-modern", "asana-math", "none"},
      {"math-concrete", "latin-modern", "concrete-math", "none"},
      {"math-erewhon", "latin-modern", "erewhon-math", "none"},
      {"math-euler", "latin-modern", "euler-math", "none"},
      {"math-fira", "latin-modern", "fira-math", "none"},
      {"math-garamond", "latin-modern", "garamond-math", "none"},
      {"math-gfs-neohellenic", "latin-modern", "gfs-neohellenic-math", "none"},
      {"math-ibm-plex", "latin-modern", "ibm-plex-math", "none"},
      {"math-kp", "latin-modern", "kp-math", "none"},
      {"math-kp-sans", "latin-modern", "kp-sans-math", "none"},
      {"math-latin-modern", "latin-modern", "latin-modern-math", "none"},
      {"math-lete-sans", "latin-modern", "lete-sans-math", "none"},
      {"math-libertinus", "latin-modern", "libertinus-math", "none"},
      {"math-luciole", "latin-modern", "luciole-math", "none"},
      {"math-new-computer-modern", "latin-modern", "new-computer-modern-math", "none"},
      {"math-new-computer-modern-sans", "latin-modern", "new-computer-modern-sans-math", "none"},
      {"math-old-standard", "latin-modern", "old-standard-math", "none"},
      {"math-pennstander", "latin-modern", "pennstander-math", "none"},
      {"math-pl46", "latin-modern", "pl46-math", "none"},
      {"math-stix", "latin-modern", "stix-math", "none"},
      {"math-stix-two", "latin-modern", "stix-two-math", "none"},
      {"math-tex-gyre-bonum", "latin-modern", "tex-gyre-bonum-math", "none"},
      {"math-tex-gyre-dejavu", "latin-modern", "tex-gyre-dejavu-math", "none"},
      {"math-tex-gyre-pagella", "latin-modern", "tex-gyre-pagella-math", "none"},
      {"math-tex-gyre-schola", "latin-modern", "tex-gyre-schola-math", "none"},
      {"math-tex-gyre-termes", "latin-modern", "tex-gyre-termes-math", "none"},
      {"math-xcharter", "latin-modern", "xcharter-math", "none"},
      {"math-xits", "latin-modern", "xits-math", "none"},
      {"text-xits-cjk-simsun", "xits", "xits-math", "simsun"},
      {"text-arial-cjk-simhei", "arial", "lete-sans-math", "simhei"},
      {"text-libertinus-cjk-fandol", "libertinus-serif", "libertinus-math", "fandol-song"},
      {"text-stix-cjk-yahei", "stix-two-text", "stix-two-math", "microsoft-yahei"},
  };

  int tested = 0;
  int skipped = 0;
  int selected = 0;
  std::string profiles_root = root.u8string();
  for (const SmokeSpec &item : cases) {
    if (!selected_case.empty() && selected_case != item.name) continue;
    ++selected;
    StemTeXProfileSpec spec{item.name, item.text, item.math, item.cjk};
    char *result_json = nullptr;
    error = nullptr;
    code = STEMTEX_PROFILE_OK;
    int ok = stemtex_profile_materialize(&context, &spec, profiles_root.c_str(), &result_json, &code, &error);
    if (!ok && code == STEMTEX_PROFILE_ERROR_FONT_UNAVAILABLE) {
      std::cout << "skip=" << item.name << " reason=" << (error ? error : "unavailable") << '\n';
      stemtex_profile_free_string(error);
      ++skipped;
      continue;
    }
    if (!ok) {
      std::cerr << (error ? error : "materialize failed") << '\n';
      stemtex_profile_free_string(error);
      fs::remove_all(root, ignored);
      return 1;
    }
    std::cout << (result_json ? result_json : "{}") << '\n';
    stemtex_profile_free_string(result_json);
    stemtex_profile_free_string(error);

    fs::path profile = root / item.name;
    bool files_ok = fs::is_regular_file(profile / "profile.json") &&
                    fs::is_regular_file(profile / "preamble.tex") &&
                    fs::is_regular_file(profile / "warmup.tex");
    if (!files_ok) {
      std::cerr << "generated files missing for " << item.name << '\n';
      fs::remove_all(root, ignored);
      return 1;
    }

    if (!runtime_root.empty()) {
      std::string repo = root.u8string();
      std::string profile_string = profile.u8string();
      StemTeXConfig renderer_config{};
      renderer_config.repo_root_utf8 = repo.c_str();
      renderer_config.runtime_root_utf8 = runtime_root.c_str();
      renderer_config.texmf_root_utf8 = texmf_root.c_str();
      renderer_config.profile_root_utf8 = profile_string.c_str();
      renderer_config.request_timeout_ms = 90000;
      renderer_config.xdvipdfmx_timeout_ms = 90000;
      renderer_config.default_width_pt = 360.0;
      StemTeXErrorCode renderer_code = STEMTEX_OK;
      char *renderer_error = nullptr;
      StemTeXRenderer *renderer = stemtex_renderer_create(&renderer_config, &renderer_code, &renderer_error);
      if (!renderer) {
        std::cerr << "renderer create failed for " << item.name << ": "
                  << (renderer_error ? renderer_error : "unknown") << '\n';
        stemtex_renderer_free_string(renderer_error);
        fs::remove_all(root, ignored);
        return 1;
      }
      stemtex_renderer_free_string(renderer_error);
      const char *snippet = std::string(item.cjk) == "none"
                                ? "StemTeX font profile: $E=mc^2$ and $\\int_0^1x^2\\,dx=\\frac13$."
                                : u8"字体组合验证：$E=mc^2$，以及 $\\int_0^1x^2\\,dx=\\frac13$。";
      StemTeXRenderResult render_result{};
      renderer_error = nullptr;
      ok = stemtex_renderer_render_with_font_size(renderer, snippet, 360.0, 12.0, &render_result,
                                                  &renderer_code, &renderer_error);
      if (!ok) {
        std::cerr << "render failed for " << item.name << ": "
                  << (renderer_error ? renderer_error : "unknown") << '\n';
        stemtex_renderer_free_string(renderer_error);
        stemtex_renderer_free_result(&render_result);
        stemtex_renderer_destroy(renderer);
        fs::remove_all(root, ignored);
        return 1;
      }
      std::cout << "render=" << item.name << " pdf="
                << (render_result.pdf_path_utf8 ? render_result.pdf_path_utf8 : "") << '\n';
      stemtex_renderer_free_string(renderer_error);
      stemtex_renderer_free_result(&render_result);
      stemtex_renderer_destroy(renderer);
    }
    ++tested;
  }
  std::cout << "tested=" << tested << " skipped=" << skipped << '\n';
  fs::remove_all(root, ignored);
  if (selected == 0) {
    std::cerr << "unknown smoke case: " << selected_case << '\n';
    return 2;
  }
  return tested > 0 ? 0 : 1;
}
