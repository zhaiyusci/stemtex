#include "stemtex_renderer.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
struct Size { double width; double height; };

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

Size render(StemTeXRenderer *renderer, const char *source, bool block = true,
            double font = 10, double width = 180, StemTeXOutputFormat format = STEMTEX_OUTPUT_SVG) {
  StemTeXRenderOutputResult result{};
  StemTeXErrorCode code{};
  char *error = nullptr;
  const int ok = block
      ? stemtex_renderer_render_block(renderer, source, width, font, format, &result, &code, &error)
      : stemtex_renderer_render_output_with_font_size(renderer, source, width, font, format, &result, &code, &error);
  const std::string message = error ? error : "render failed";
  stemtex_renderer_free_string(error);
  if (!ok) throw std::runtime_error(message);
  std::ifstream input(fs::u8path(result.output_path_utf8), std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(input)), {});
  std::cout << (block ? "block" : "legacy") << " font=" << font << " path=" << result.output_path_utf8 << '\n';
  stemtex_renderer_free_output_result(&result);
  std::smatch match;
  const std::string number = "([-+0-9.eE]+)";
  const std::regex bounds(format == STEMTEX_OUTPUT_SVG
      ? "viewBox=['\"]" + number + "\\s+" + number + "\\s+" + number + "\\s+" + number
      : "/MediaBox\\s*\\[\\s*" + number + "\\s+" + number + "\\s+" + number + "\\s+" + number);
  require(std::regex_search(data, match, bounds), "output has no readable page bounds");
  Size size{std::stod(match[3]), std::stod(match[4])};
  if (format == STEMTEX_OUTPUT_PDF) {
    size.width -= std::stod(match[1]);
    size.height -= std::stod(match[2]);
  }
  require(size.width > 0 && size.height > 0, "output has invalid dimensions");
  std::cout << "width=" << size.width << " height=" << size.height << '\n';
  return size;
}

bool near(double a, double b) { return std::abs(a - b) < 0.03; }

int main(int argc, char **argv) {
  if (argc < 3 || argc > 5 || (argc == 5 && std::string(argv[4]) != "--color")) {
    std::cerr << "usage: stemtex-block-smoke RUNTIME PROFILE [TEXMF [--color]]\n";
    return 2;
  }
  try {
    const std::string repo = fs::current_path().u8string();
    StemTeXConfig config{};
    config.repo_root_utf8 = repo.c_str();
    config.runtime_root_utf8 = argv[1];
    config.profile_root_utf8 = argv[2];
    config.texmf_root_utf8 = argc >= 4 ? argv[3] : argv[1];
    config.request_timeout_ms = 90000;
    config.xdvipdfmx_timeout_ms = 90000;
    StemTeXErrorCode code{};
    char *error = nullptr;
    std::unique_ptr<StemTeXRenderer, decltype(&stemtex_renderer_destroy)> renderer(
        stemtex_renderer_create(&config, &code, &error), stemtex_renderer_destroy);
    const std::string message = error ? error : "renderer create failed";
    stemtex_renderer_free_string(error);
    require(renderer != nullptr, message.c_str());

    const auto legacy = render(renderer.get(), "a", false);
    const auto a = render(renderer.get(), "a");
    require(a.height > legacy.height, "lowercase text did not acquire full line metrics");
    require(near(a.height, render(renderer.get(), "\\strut a\\strut", false).height),
            "text bounds disagree with an actual strut-supported line");
    for (const char *source : {"A", "g", "$\\alpha$", "a% trailing comment", "a\r\n", "a\n\n"}) {
      require(near(render(renderer.get(), source).height, a.height), "first/last line metrics vary with glyphs or terminal whitespace");
    }
    require(near(render(renderer.get(), "a", true, 10, 180, STEMTEX_OUTPUT_PDF).height, a.height),
            "PDF and SVG page heights disagree");
    const auto large = render(renderer.get(), "a", true, 20);
    require(large.height > a.height * 1.7, "line height did not follow the selected font size");
    require(near(render(renderer.get(), "g", true, 20).height, large.height), "large-font final depth is incomplete");
    const auto lines = render(renderer.get(), "a\\\\a");
    require(near(lines.height - a.height, 12 * 72 / 72.27), "two-line block changed the native baseline distance");
    const auto tall = render(renderer.get(), "$\\displaystyle\\frac{1}{\\frac{2}{3}}$");
    require(tall.height > a.height, "tall math was constrained to one line height");
    const char *prose = "One two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen.";
    require(render(renderer.get(), prose, true, 10, 90).height > render(renderer.get(), prose, true, 10, 300).height,
            "height did not grow when text wrapped");
    const auto display = render(renderer.get(), "\\[\\alpha\\]");
    // Compare with an ordinary TeX box, retaining native display skips.
    const auto natural = render(renderer.get(), "\\vbox{\\noindent\\[\\alpha\\]\\par}", false);
    require(near(display.height, natural.height), "display acquired a synthetic full text line");
    require(near(display.width, a.width), "a display did not retain the requested block width");
    require(near(render(renderer.get(), "\\def\\MyDisplay{\\[\\alpha\\]}\\MyDisplay").height, display.height),
            "macro-defined display differs from literal display");
    require(near(render(renderer.get(), "% comment\n\\[\\alpha\\]").height, display.height), "leading comments changed display detection");
    require(near(render(renderer.get(), "$$\\alpha$$").height, display.height), "dollar display acquired extra space");
    require(near(render(renderer.get(), "\\begin% comment\n{equation*}\\alpha\\end{equation*}").height, display.height),
            "display environment with comments was not recognized");
    require(near(render(renderer.get(), "\\[\\alpha\\]a").height,
                 render(renderer.get(), "\\[\\alpha\\]g").height), "prose after display has incomplete final depth");
    const auto displayPdf = render(renderer.get(), "\\[\\alpha\\]", true, 10, 180, STEMTEX_OUTPUT_PDF);
    require(near(displayPdf.height, display.height), "display PDF and SVG heights disagree");
    require(near(render(renderer.get(), "\\[\\alpha\\]\n\n").height, display.height),
            "terminal blank lines hid the display glue");
    const std::string reportedFormula = R"TEX($$
\sum_{A} \boldsymbol{X}_{A} \times \frac{\partial E_{P S}}{\partial \boldsymbol{X}_{A}}+\sum_{A} \boldsymbol{P}_{A} \times \frac{\partial E_{P S}}{\partial \boldsymbol{P}_{A}}=0 . %{\color{red}s}
$$)TEX";
    const auto formula = render(renderer.get(), reportedFormula.c_str(), true, 10, 360);
    require(near(render(renderer.get(), reportedFormula.c_str(), true, 10, 360, STEMTEX_OUTPUT_PDF).height, formula.height),
            "reported formula PDF/SVG heights disagree");
    const auto explicitSpace = render(renderer.get(), (reportedFormula + "\\par\\vspace{20pt}").c_str(), true, 10, 360);
    require(explicitSpace.height > formula.height + 19, "explicit terminal vertical space was removed");
    if (argc == 5) {
      for (const char *source : {"\\color{red}a", "{\\color{red}g}", "\\textcolor{red}{a}"}) {
        require(near(render(renderer.get(), source).height, a.height), "colored text lost full line metrics");
      }
      const std::string prefix = "f\n\nStemTeX Renderer GUI: inline mathematics $E=mc^2$.\n\n"
          "\\[\\int_0^1 x^2\\,dx=\\frac{1}{3},\\quad\\langle\\psi,\\phi ";
      const std::string openColor = prefix + "\\color{red}\\rangle\\]\n\nf";
      const std::string groupedColor = prefix + "{\\color{red}\\rangle}\\]\n\nf";
      const auto nativeOpen = render(renderer.get(), openColor.c_str(), false);
      const auto nativeGrouped = render(renderer.get(), groupedColor.c_str(), false);
      const auto blockOpen = render(renderer.get(), openColor.c_str());
      const auto blockGrouped = render(renderer.get(), groupedColor.c_str());
      require(near(blockOpen.height - blockGrouped.height, nativeOpen.height - nativeGrouped.height),
              "block altered native color-group spacing");
      require(near(render(renderer.get(), openColor.c_str(), true, 10, 180, STEMTEX_OUTPUT_PDF).height,
                   blockOpen.height), "colored block PDF/SVG heights disagree");
    }
    render(renderer.get(), "Text before display.\\[\\alpha\\]");
    render(renderer.get(), "\\begin{itemize}\\item First\\item Last\\end{itemize}");
    render(renderer.get(), "\\begin{tabular}{ll}a&b\\\\c&d\\end{tabular}");
    render(renderer.get(), "\\rule{20pt}{40pt}");
    const auto beforeError = render(renderer.get(), "a", false);
    require(near(beforeError.height, legacy.height), "successful block calls changed the legacy preview border");

    StemTeXRenderOutputResult invalid{};
    error = nullptr;
    require(!stemtex_renderer_render_block(renderer.get(), nullptr, 180, 10, STEMTEX_OUTPUT_SVG,
                                           &invalid, &code, &error) && code == STEMTEX_ERROR_INVALID_ARGUMENT,
            "null source not rejected");
    stemtex_renderer_free_string(error);
    error = nullptr;
    require(!stemtex_renderer_render_block(renderer.get(), "\\UndefinedBlockCommand", 180, 10, STEMTEX_OUTPUT_SVG,
                                           &invalid, &code, &error) && code == STEMTEX_ERROR_TEX_SNIPPET,
            "bad block did not produce a TeX error");
    stemtex_renderer_free_string(error);
    require(near(render(renderer.get(), "a").height, a.height), "block did not recover after a TeX error");
    const auto after = render(renderer.get(), "a", false);
    require(near(legacy.width, after.width) && near(legacy.height, after.height), "block changed legacy render geometry");
    std::cout << "block layout checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
