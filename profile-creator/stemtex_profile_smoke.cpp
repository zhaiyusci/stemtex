#include "stemtex_profile.h"
#include "stemtex_renderer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::string read_text(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream text;
  text << stream.rdbuf();
  return text.str();
}

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

  char *package_catalog = stemtex_profile_package_catalog_json(&context, &code, &error);
  if (!package_catalog) {
    std::cerr << (error ? error : "package catalog failed") << '\n';
    stemtex_profile_free_string(error);
    return 1;
  }
  std::string package_catalog_text = package_catalog;
  std::cout << package_catalog << '\n';
  stemtex_profile_free_string(package_catalog);
  stemtex_profile_free_string(error);
  const std::vector<const char *> known_packages = {
      "mathtools", "mhchem", "physics", "xcolor", "cancel", "tikz", "pgfplots",
      "tikz-cd", "circuitikz", "forest", "chemfig", "quantikz"};
  for (const char *id : known_packages) {
    if (package_catalog_text.find(std::string("\"id\":\"") + id + "\"") == std::string::npos) {
      std::cerr << "package catalog missing " << id << '\n';
      return 1;
    }
  }

  const char *plan_ids[] = {"physics", "cancel", "xcolor"};
  char *package_plan = stemtex_profile_package_plan_json(&context, plan_ids, 3, &code, &error);
  if (!package_plan) {
    std::cerr << (error ? error : "package plan failed") << '\n';
    stemtex_profile_free_string(error);
    return 1;
  }
  std::string package_plan_text = package_plan;
  std::cout << package_plan << '\n';
  stemtex_profile_free_string(package_plan);
  stemtex_profile_free_string(error);
  size_t mathtools_position = package_plan_text.find("\"id\":\"mathtools\"");
  size_t physics_position = package_plan_text.find("\"id\":\"physics\"");
  size_t xcolor_position = package_plan_text.find("\"id\":\"xcolor\"");
  size_t cancel_position = package_plan_text.find("\"id\":\"cancel\"");
  if (mathtools_position == std::string::npos || physics_position == std::string::npos ||
      xcolor_position == std::string::npos || cancel_position == std::string::npos ||
      !(mathtools_position < physics_position && xcolor_position < cancel_position) ||
      package_plan_text.find("\"id\":\"mathtools\",\"displayName\":\"mathtools\",\"explicit\":false") ==
          std::string::npos) {
    std::cerr << "package plan did not resolve dependencies/order as expected\n";
    return 1;
  }

  const char *drawing_plan_ids[] = {"pgfplots", "chemfig"};
  package_plan = stemtex_profile_package_plan_json(&context, drawing_plan_ids, 2, &code, &error);
  if (!package_plan) {
    std::cerr << (error ? error : "drawing package plan failed") << '\n';
    stemtex_profile_free_string(error);
    return 1;
  }
  package_plan_text = package_plan;
  std::cout << package_plan << '\n';
  stemtex_profile_free_string(package_plan);
  stemtex_profile_free_string(error);
  xcolor_position = package_plan_text.find("\"id\":\"xcolor\"");
  size_t tikz_position = package_plan_text.find("\"id\":\"tikz\"");
  size_t pgfplots_position = package_plan_text.find("\"id\":\"pgfplots\"");
  size_t chemfig_position = package_plan_text.find("\"id\":\"chemfig\"");
  if (xcolor_position == std::string::npos || tikz_position == std::string::npos ||
      pgfplots_position == std::string::npos || chemfig_position == std::string::npos ||
      !(xcolor_position < tikz_position && tikz_position < pgfplots_position &&
        pgfplots_position < chemfig_position) ||
      package_plan_text.find("\"id\":\"tikz\",\"displayName\":\"TikZ\",\"explicit\":false") ==
          std::string::npos) {
    std::cerr << "drawing package plan did not resolve dependencies/order as expected\n";
    return 1;
  }

  fs::path root = fs::temp_directory_path() / "stemtex-profile-smoke";
  std::error_code ignored;
  fs::remove_all(root, ignored);
  fs::create_directories(root);
  struct SmokeSpec {
    const char *name;
    const char *text;
    const char *math;
    const char *cjk;
    std::vector<const char *> packages;
    bool package_aware;
  };
  const std::vector<SmokeSpec> cases = {
      {"package-selection", "latin-modern", "latin-modern-math", "none", {"mhchem", "xcolor", "cancel"}, true},
      {"package-none", "latin-modern", "latin-modern-math", "none", {}, true},
      {"drawing-packages", "latin-modern", "latin-modern-math", "none",
       {"pgfplots", "tikz-cd", "circuitikz", "forest", "chemfig", "quantikz"}, true},
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
    char *result_json = nullptr;
    error = nullptr;
    code = STEMTEX_PROFILE_OK;
    int ok = 0;
    if (!item.package_aware) {
      StemTeXProfileSpec spec{item.name, item.text, item.math, item.cjk};
      ok = stemtex_profile_materialize(&context, &spec, profiles_root.c_str(), &result_json, &code, &error);
    } else {
      StemTeXProfileSpecV2 spec{item.name, item.text, item.math, item.cjk,
                               item.packages.data(), item.packages.size()};
      ok = stemtex_profile_materialize_v2(&context, &spec, profiles_root.c_str(), &result_json, &code, &error);
    }
    if (!ok && (code == STEMTEX_PROFILE_ERROR_FONT_UNAVAILABLE ||
                code == STEMTEX_PROFILE_ERROR_PACKAGE_UNAVAILABLE)) {
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

    if (std::string(item.name) == "package-selection") {
      const std::string preamble = read_text(profile / "preamble.tex");
      const std::string manifest = read_text(profile / "profile.json");
      const size_t mathtools = preamble.find("\\usepackage{mathtools}");
      const size_t fonts = preamble.find("\\usepackage{unicode-math}");
      const size_t mhchem = preamble.find("\\usepackage[version=4]{mhchem}");
      const size_t xcolor = preamble.find("\\usepackage{xcolor}");
      const size_t cancel = preamble.find("\\usepackage{cancel}");
      const size_t preview = preamble.find("\\usepackage[active,tightpage]{preview}");
      if (mathtools == std::string::npos || fonts == std::string::npos || mhchem == std::string::npos ||
          xcolor == std::string::npos || cancel == std::string::npos || preview == std::string::npos ||
          !(mathtools < fonts && fonts < mhchem && mhchem < xcolor && xcolor < cancel && cancel < preview) ||
          preamble.find("\\usepackage{physics}") != std::string::npos ||
          manifest.find("\"schemaVersion\": 2") == std::string::npos ||
          manifest.find("\"selected\": [\"mhchem\", \"xcolor\", \"cancel\"]") == std::string::npos ||
          manifest.find("\"resolvedOrder\": [\"mathtools\", \"mhchem\", \"xcolor\", \"cancel\"]") ==
              std::string::npos) {
        std::cerr << "package-aware profile output is incorrect\n";
        fs::remove_all(root, ignored);
        return 1;
      }
    }
    if (std::string(item.name) == "package-none") {
      const std::string preamble = read_text(profile / "preamble.tex");
      const std::string warmup = read_text(profile / "warmup.tex");
      for (const char *package : known_packages) {
        if (preamble.find(std::string("{") + package + "}") != std::string::npos) {
          std::cerr << "empty package selection unexpectedly loaded " << package << '\n';
          fs::remove_all(root, ignored);
          return 1;
        }
      }
      if (warmup.find("\\ce") != std::string::npos || warmup.find("\\cancel") != std::string::npos ||
          warmup.find("\\textcolor") != std::string::npos) {
        std::cerr << "empty package selection retained an optional warmup probe\n";
        fs::remove_all(root, ignored);
        return 1;
      }
    }
    if (std::string(item.name) == "drawing-packages") {
      const std::string preamble = read_text(profile / "preamble.tex");
      const std::string warmup = read_text(profile / "warmup.tex");
      const size_t xcolor = preamble.find("\\usepackage{xcolor}");
      const size_t tikz = preamble.find("\\usepackage{tikz}");
      const size_t pgfplots = preamble.find("\\usepackage{pgfplots}");
      const size_t compat = preamble.find("\\pgfplotsset{compat=newest}");
      const size_t tikz_cd = preamble.find("\\usepackage{tikz-cd}");
      const size_t circuitikz = preamble.find("\\usepackage{circuitikz}");
      const size_t forest = preamble.find("\\usepackage{forest}");
      const size_t chemfig = preamble.find("\\usepackage{chemfig}");
      const size_t quantikz = preamble.find("\\usepackage{quantikz}");
      const size_t preview = preamble.find("\\usepackage[active,tightpage]{preview}");
      if (xcolor == std::string::npos || tikz == std::string::npos || pgfplots == std::string::npos ||
          compat == std::string::npos || tikz_cd == std::string::npos || circuitikz == std::string::npos ||
          forest == std::string::npos || chemfig == std::string::npos || quantikz == std::string::npos ||
          preview == std::string::npos ||
          !(xcolor < tikz && tikz < pgfplots && pgfplots < compat && compat < tikz_cd &&
            tikz_cd < circuitikz && circuitikz < forest && forest < chemfig && chemfig < quantikz &&
            quantikz < preview) ||
          warmup.find("\\begin{axis}") == std::string::npos ||
          warmup.find("\\begin{tikzcd}") == std::string::npos ||
          warmup.find("\\begin{circuitikz}") == std::string::npos ||
          warmup.find("\\begin{forest}") == std::string::npos ||
          warmup.find("\\chemfig") == std::string::npos ||
          warmup.find("\\begin{quantikz}") == std::string::npos) {
        std::cerr << "drawing package profile output is incorrect\n";
        fs::remove_all(root, ignored);
        return 1;
      }
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
      const char *snippet = nullptr;
      if (std::string(item.name) == "drawing-packages") {
        snippet = R"TEX(\begin{tikzpicture}
\begin{axis}[
  width=.92\linewidth,
  height=180pt,
  axis lines=middle,
  xlabel={$x$},
  ylabel={$y$},
  xmin=-6.4, xmax=6.4,
  ymin=-1.25, ymax=1.25,
  domain=-6.283:6.283,
  samples=161,
  grid=both,
  legend pos=north east
]
  \addplot[blue, very thick] {sin(deg(x))};
  \addlegendentry{$\sin x$}
  \addplot[red, very thick, dashed] {cos(deg(x))};
  \addlegendentry{$\cos x$}
\end{axis}
\end{tikzpicture})TEX";
      } else {
        snippet = std::string(item.cjk) == "none"
                      ? "StemTeX font profile: $E=mc^2$ and $\\int_0^1x^2\\,dx=\\frac13$."
                      : u8"字体组合验证：$E=mc^2$，以及 $\\int_0^1x^2\\,dx=\\frac13$。";
      }
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
