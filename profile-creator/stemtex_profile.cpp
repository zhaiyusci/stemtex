#include "stemtex_profile.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifndef STEMTEX_PROFILE_VERSION
#define STEMTEX_PROFILE_VERSION "0.0.0-dev"
#endif

constexpr const char *kVersion = STEMTEX_PROFILE_VERSION;
constexpr const char *kAbiVersion = "2";

class ProfileException : public std::runtime_error {
 public:
  ProfileException(StemTeXProfileErrorCode code, const std::string &message)
      : std::runtime_error(message), code(code) {}
  StemTeXProfileErrorCode code;
};

enum class RequirementRoot { TexmfDist, SystemFonts };

struct Requirement {
  RequirementRoot root;
  const char *relative_path;
};

struct FontRecipe {
  const char *id;
  const char *role;
  const char *display_name;
  const char *source;
  const char *description;
  std::vector<Requirement> requirements;
  const char *tex;
};

enum class PackagePhase { BeforeFonts, AfterFonts };

struct PackageRecipe {
  const char *id;
  const char *display_name;
  const char *category;
  const char *description;
  Requirement requirement;
  bool default_enabled;
  PackagePhase phase;
  int order;
  const char *options;
  const char *after_load_tex;
  const char *warmup_probe;
  std::vector<const char *> requires;
  std::vector<const char *> after;
};

const std::vector<PackageRecipe> &package_recipes() {
  static const std::vector<PackageRecipe> value = {
      {"mathtools", "mathtools", "math", "AMS mathematics extensions and a stable pre-font math foundation",
       {RequirementRoot::TexmfDist, "tex/latex/mathtools/mathtools.sty"}, true,
       PackagePhase::BeforeFonts, 100, "", "", "Math tools: $a \\coloneqq b$.\n", {}, {}},
      {"mhchem", "mhchem", "chemistry", "Chemical formulae and equations through \\ce",
       {RequirementRoot::TexmfDist, "tex/latex/mhchem/mhchem.sty"}, true,
       PackagePhase::AfterFonts, 200, "version=4", "", "\\ce{H2O} \\ce{2H2 + O2 -> 2H2O}.\n",
       {"mathtools"}, {}},
      {"physics", "physics", "physics", "Common derivatives, vectors, operators, and bra-ket notation",
       {RequirementRoot::TexmfDist, "tex/latex/physics/physics.sty"}, true,
       PackagePhase::AfterFonts, 300, "", "", "Physics: $\\dv{x}{t}$ and $\\qty(1+x)$.\n",
       {"mathtools"}, {}},
      {"xcolor", "xcolor", "text", "Named colors and color-aware text and mathematics",
       {RequirementRoot::TexmfDist, "tex/latex/xcolor/xcolor.sty"}, true,
       PackagePhase::AfterFonts, 400, "", "", "\\textcolor{blue}{StemTeX color probe}.\n", {}, {}},
      {"cancel", "cancel", "math", "Cancellation strokes for mathematical expressions",
       {RequirementRoot::TexmfDist, "tex/latex/cancel/cancel.sty"}, true,
       PackagePhase::AfterFonts, 500, "", "", "Cancel: $\\cancel{x}+y$.\n", {}, {"xcolor"}},
      {"tikz", "TikZ", "graphics", "General-purpose native TeX vector graphics",
       {RequirementRoot::TexmfDist, "tex/latex/pgf/frontendlayer/tikz.sty"}, false,
       PackagePhase::AfterFonts, 600, "", "",
       R"TEX(\begin{tikzpicture}[baseline=-0.5ex]
  \draw[->] (0,0) -- (0.8,0);
\end{tikzpicture}
)TEX",
       {"xcolor"}, {}},
      {"pgfplots", "PGFPlots", "plots", "Two- and three-dimensional data and function plots",
       {RequirementRoot::TexmfDist, "tex/latex/pgfplots/pgfplots.sty"}, false,
       PackagePhase::AfterFonts, 700, "", "\\pgfplotsset{compat=newest}\n",
       R"TEX(\begin{tikzpicture}
  \begin{axis}[width=3cm,height=2cm,ticks=none]
    \addplot coordinates {(0,0) (1,1)};
  \end{axis}
\end{tikzpicture}
)TEX",
       {"tikz"}, {}},
      {"tikz-cd", "tikz-cd", "diagrams", "Commutative diagrams with TikZ",
       {RequirementRoot::TexmfDist, "tex/latex/tikz-cd/tikz-cd.sty"}, false,
       PackagePhase::AfterFonts, 800, "", "",
       R"TEX(\begin{tikzcd}
  A \arrow[r] & B
\end{tikzcd}
)TEX",
       {"tikz"}, {}},
      {"circuitikz", "CircuiTikZ", "diagrams", "Electrical and electronic circuit diagrams",
       {RequirementRoot::TexmfDist, "tex/latex/circuitikz/circuitikz.sty"}, false,
       PackagePhase::AfterFonts, 900, "", "",
       R"TEX(\begin{circuitikz}
  \draw (0,0) to[R] (1,0);
\end{circuitikz}
)TEX",
       {"tikz"}, {}},
      {"forest", "forest", "diagrams", "Compact linguistic and general tree diagrams",
       {RequirementRoot::TexmfDist, "tex/latex/forest/forest.sty"}, false,
       PackagePhase::AfterFonts, 1000, "", "",
       R"TEX(\begin{forest}
  [A [B] [C]]
\end{forest}
)TEX",
       {"tikz"}, {}},
      {"chemfig", "chemfig", "chemistry", "Two-dimensional chemical structure diagrams",
       {RequirementRoot::TexmfDist, "tex/generic/chemfig/chemfig.sty"}, false,
       PackagePhase::AfterFonts, 1100, "", "", "\\chemfig{H-O-H}\n", {"tikz"}, {}},
      {"quantikz", "quantikz", "diagrams", "Quantum circuit diagrams with TikZ",
       {RequirementRoot::TexmfDist, "tex/latex/quantikz/quantikz.sty"}, false,
       PackagePhase::AfterFonts, 1200, "", "",
       R"TEX(\begin{quantikz}
  \lstick{$q_0$} & \gate{H} & \meter{}
\end{quantikz}
)TEX",
       {"tikz"}, {}},
  };
  return value;
}

const std::vector<FontRecipe> &recipes() {
  static const std::vector<FontRecipe> value = {
      {"latin-modern", "text", "Latin Modern", "texlive",
       "Computer Modern-compatible OpenType text",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/lm/lmroman10-regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/lm/lmroman10-bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/lm/lmroman10-italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/lm/lmroman10-bolditalic.otf"}},
       R"TEX(\setmainfont{lmroman10-regular.otf}[
  BoldFont=lmroman10-bold.otf,
  ItalicFont=lmroman10-italic.otf,
  BoldItalicFont=lmroman10-bolditalic.otf
]
\setsansfont{lmsans10-regular.otf}[
  BoldFont=lmsans10-bold.otf,
  ItalicFont=lmsans10-oblique.otf,
  BoldItalicFont=lmsans10-boldoblique.otf
]
\setmonofont{lmmono10-regular.otf}[ItalicFont=lmmono10-italic.otf]
)TEX"},
      {"xits", "text", "XITS", "texlive", "Times-compatible scientific text",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/xits/XITS-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/xits/XITS-Bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/xits/XITS-Italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/xits/XITS-BoldItalic.otf"}},
       R"TEX(\setmainfont{XITS-Regular.otf}[
  BoldFont=XITS-Bold.otf,
  ItalicFont=XITS-Italic.otf,
  BoldItalicFont=XITS-BoldItalic.otf
]
\setsansfont{lmsans10-regular.otf}[
  BoldFont=lmsans10-bold.otf,
  ItalicFont=lmsans10-oblique.otf,
  BoldItalicFont=lmsans10-boldoblique.otf
]
\setmonofont{lmmono10-regular.otf}[ItalicFont=lmmono10-italic.otf]
)TEX"},
      {"tex-gyre-termes", "text", "TeX Gyre Termes", "texlive", "Times-compatible TeX Gyre text",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyretermes-regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyretermes-bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyretermes-italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyretermes-bolditalic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyreheros-regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyreheros-bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyreheros-italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyreheros-bolditalic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyrecursor-regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyrecursor-bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyrecursor-italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre/texgyrecursor-bolditalic.otf"}},
       R"TEX(\setmainfont{texgyretermes-regular.otf}[
  BoldFont=texgyretermes-bold.otf,
  ItalicFont=texgyretermes-italic.otf,
  BoldItalicFont=texgyretermes-bolditalic.otf
]
\setsansfont{texgyreheros-regular.otf}[
  BoldFont=texgyreheros-bold.otf,
  ItalicFont=texgyreheros-italic.otf,
  BoldItalicFont=texgyreheros-bolditalic.otf
]
\setmonofont{texgyrecursor-regular.otf}[
  BoldFont=texgyrecursor-bold.otf,
  ItalicFont=texgyrecursor-italic.otf,
  BoldItalicFont=texgyrecursor-bolditalic.otf
]
)TEX"},
      {"libertinus-serif", "text", "Libertinus Serif", "texlive", "OpenType serif text for technical documents",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSerif-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSerif-Bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSerif-Italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSerif-BoldItalic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSans-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSans-Bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusSans-Italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusMono-Regular.otf"}},
       R"TEX(\setmainfont{LibertinusSerif-Regular.otf}[
  BoldFont=LibertinusSerif-Bold.otf,
  ItalicFont=LibertinusSerif-Italic.otf,
  BoldItalicFont=LibertinusSerif-BoldItalic.otf
]
\setsansfont{LibertinusSans-Regular.otf}[
  BoldFont=LibertinusSans-Bold.otf,
  ItalicFont=LibertinusSans-Italic.otf
]
\setmonofont{LibertinusMono-Regular.otf}
)TEX"},
      {"stix-two-text", "text", "STIX Two Text", "texlive", "STIX scientific text family",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/stix2-otf/STIXTwoText-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/stix2-otf/STIXTwoText-Bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/stix2-otf/STIXTwoText-Italic.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/stix2-otf/STIXTwoText-BoldItalic.otf"}},
       R"TEX(\setmainfont{STIXTwoText-Regular.otf}[
  BoldFont=STIXTwoText-Bold.otf,
  ItalicFont=STIXTwoText-Italic.otf,
  BoldItalicFont=STIXTwoText-BoldItalic.otf
]
\setsansfont{lmsans10-regular.otf}[BoldFont=lmsans10-bold.otf,ItalicFont=lmsans10-oblique.otf]
\setmonofont{lmmono10-regular.otf}[ItalicFont=lmmono10-italic.otf]
)TEX"},
      {"arial", "text", "Arial", "system", "Windows Arial text",
       {{RequirementRoot::SystemFonts, "arial.ttf"},
        {RequirementRoot::SystemFonts, "arialbd.ttf"},
       {RequirementRoot::SystemFonts, "ariali.ttf"},
        {RequirementRoot::SystemFonts, "arialbi.ttf"}},
       R"TEX(\setmainfont{Arial}[
  BoldFont={Arial Bold},
  ItalicFont={Arial Italic},
  BoldItalicFont={Arial Bold Italic}
]
\setsansfont{Arial}[
  BoldFont={Arial Bold},
  ItalicFont={Arial Italic},
  BoldItalicFont={Arial Bold Italic}
]
\setmonofont{lmmono10-regular.otf}[ItalicFont=lmmono10-italic.otf]
)TEX"},

      {"arsenal-math", "math", "Arsenal Math", "texlive", "Arsenal-based sans-serif Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/arsenal-math/ArsenalMath-Sans.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/arsenal-math/ArsenalMath-SansBold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{ArsenalMath-Sans.otf}[BoldFont=ArsenalMath-SansBold.otf]
)TEX"},
      {"asana-math", "math", "Asana Math", "texlive", "Palatino-style Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/asana-math/Asana-Math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{Asana-Math.otf}
)TEX"},
      {"concrete-math", "math", "Concrete Math", "texlive", "Concrete-style Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/concmath-otf/Concrete-Math.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/concmath-otf/Concrete-Math-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{Concrete-Math.otf}[BoldFont=Concrete-Math-Bold.otf]
)TEX"},
      {"erewhon-math", "math", "Erewhon Math", "texlive", "Utopia-style Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/erewhon-math/Erewhon-Math.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/erewhon-math/Erewhon-Math-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{Erewhon-Math.otf}[BoldFont=Erewhon-Math-Bold.otf]
)TEX"},
      {"euler-math", "math", "Euler Math", "texlive", "OpenType version of Hermann Zapf's Euler mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/euler-math/Euler-Math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{Euler-Math.otf}
)TEX"},
      {"fira-math", "math", "Fira Math", "texlive", "Fira sans-serif Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/firamath/FiraMath-Regular.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{FiraMath-Regular.otf}
)TEX"},
      {"garamond-math", "math", "Garamond Math", "texlive", "Unicode mathematics matching EB Garamond",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/garamond-math/Garamond-Math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{Garamond-Math.otf}
)TEX"},
      {"gfs-neohellenic-math", "math", "GFS Neohellenic Math", "texlive", "Neo-Hellenic sans-serif Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/gfsneohellenicmath/GFSNeohellenicMath.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{GFSNeohellenicMath.otf}
)TEX"},
      {"ibm-plex-math", "math", "IBM Plex Math", "texlive", "Unicode mathematics matching IBM Plex",
       {{RequirementRoot::TexmfDist, "fonts/opentype/ibm/plex/IBMPlexMath-Regular.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{IBMPlexMath-Regular.otf}
)TEX"},
      {"kp-math", "math", "KpMath", "texlive", "Serif Unicode mathematics from the Kpfonts family",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/kpfonts-otf/KpMath-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/kpfonts-otf/KpMath-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{KpMath-Regular.otf}[BoldFont=KpMath-Bold.otf]
)TEX"},
      {"kp-sans-math", "math", "KpMath Sans", "texlive", "Sans-serif Unicode mathematics from the Kpfonts family",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/kpfonts-otf/KpMath-Sans.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/kpfonts-otf/KpMath-SansBold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{KpMath-Sans.otf}[BoldFont=KpMath-SansBold.otf]
)TEX"},
      {"latin-modern-math", "math", "Latin Modern Math", "texlive", "Unicode Computer Modern mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/lm-math/latinmodern-math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{latinmodern-math.otf}
)TEX"},
      {"lete-sans-math", "math", "Lete Sans Math", "texlive", "Sans-serif Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/lete-sans-math/LeteSansMath.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/lete-sans-math/LeteSansMath-Bold.otf"},
        {RequirementRoot::TexmfDist, "tex/latex/lete-sans-math/lete-sans-math.sty"}},
       R"TEX(\usepackage[textrm,textit,textbf,textsf]{lete-sans-math}
)TEX"},
      {"libertinus-math", "math", "Libertinus Math", "texlive", "Unicode mathematics matching Libertinus text",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/libertinus-fonts/LibertinusMath-Regular.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{LibertinusMath-Regular.otf}
)TEX"},
      {"luciole-math", "math", "Luciole Math", "texlive", "Accessibility-focused Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/luciole/Luciole-Math.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/luciole/Luciole-Math-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{Luciole-Math.otf}[BoldFont=Luciole-Math-Bold.otf]
)TEX"},
      {"new-computer-modern-math", "math", "New Computer Modern Math", "texlive", "Expanded Computer Modern Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/newcomputermodern/NewCMMath-Book.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/newcomputermodern/NewCMMath-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{NewCMMath-Book.otf}[BoldFont=NewCMMath-Bold.otf]
)TEX"},
      {"new-computer-modern-sans-math", "math", "New Computer Modern Sans Math", "texlive", "Sans-serif Computer Modern Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/newcomputermodern/NewCMSansMath-Regular.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{NewCMSansMath-Regular.otf}
)TEX"},
      {"old-standard-math", "math", "Old Standard Math", "texlive", "Historic-style Unicode mathematics matching Old Standard",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/oldstandard/OldStandard-Math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{OldStandard-Math.otf}
)TEX"},
      {"pennstander-math", "math", "Pennstander Math", "texlive", "Rounded sans-serif Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/pennstander-otf/PennstanderMath-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/pennstander-otf/PennstanderMath-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{PennstanderMath-Regular.otf}[BoldFont=PennstanderMath-Bold.otf]
)TEX"},
      {"pl46-math", "math", "PL46 Math", "texlive", "Line-segment OpenType Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/pl46-fonts/PL46-Math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{PL46-Math.otf}
)TEX"},
      {"stix-math", "math", "STIX Math", "texlive", "Original STIX Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/stix/STIXMath-Regular.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{STIXMath-Regular.otf}
)TEX"},
      {"stix-two-math", "math", "STIX Two Math", "texlive", "STIX Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/stix2-otf/STIXTwoMath-Regular.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{STIXTwoMath-Regular.otf}
)TEX"},
      {"tex-gyre-bonum-math", "math", "TeX Gyre Bonum Math", "texlive", "Bookman-compatible TeX Gyre mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre-math/texgyrebonum-math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{texgyrebonum-math.otf}
)TEX"},
      {"tex-gyre-dejavu-math", "math", "TeX Gyre DejaVu Math", "texlive", "DejaVu-compatible TeX Gyre mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre-math/texgyredejavu-math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{texgyredejavu-math.otf}
)TEX"},
      {"tex-gyre-pagella-math", "math", "TeX Gyre Pagella Math", "texlive", "Palatino-compatible TeX Gyre mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre-math/texgyrepagella-math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{texgyrepagella-math.otf}
)TEX"},
      {"tex-gyre-schola-math", "math", "TeX Gyre Schola Math", "texlive", "Century Schoolbook-compatible TeX Gyre mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre-math/texgyreschola-math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{texgyreschola-math.otf}
)TEX"},
      {"tex-gyre-termes-math", "math", "TeX Gyre Termes Math", "texlive", "Times-compatible TeX Gyre mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/tex-gyre-math/texgyretermes-math.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{texgyretermes-math.otf}
)TEX"},
      {"xcharter-math", "math", "XCharter Math", "texlive", "Charter-style Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/xcharter-math/XCharter-Math.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/xcharter-math/XCharter-Math-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{XCharter-Math.otf}[BoldFont=XCharter-Math-Bold.otf]
)TEX"},
      {"xits-math", "math", "XITS Math", "texlive", "Times-compatible Unicode mathematics",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/xits/XITSMath-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/xits/XITSMath-Bold.otf"}},
       R"TEX(\usepackage{unicode-math}
\setmathfont{XITSMath-Regular.otf}[BoldFont=XITSMath-Bold.otf]
)TEX"},

      {"none", "cjk", "No CJK font", "none", "Do not load xeCJK", {}, ""},
      {"simsun", "cjk", "SimSun / SimHei", "system", "Windows SimSun body with SimHei sans text",
       {{RequirementRoot::SystemFonts, "simsun.ttc"}, {RequirementRoot::SystemFonts, "simhei.ttf"}},
       R"TEX(\usepackage{xeCJK}
\setCJKmainfont{SimSun}
\setCJKsansfont{SimHei}
\setCJKmonofont{SimSun}
)TEX"},
      {"simhei", "cjk", "SimHei", "system", "Windows SimHei with deterministic synthetic shapes",
       {{RequirementRoot::SystemFonts, "simhei.ttf"}},
       R"TEX(\usepackage{xeCJK}
\setCJKmainfont{SimHei}[AutoFakeBold=1.5,AutoFakeSlant=0.2]
\setCJKsansfont{SimHei}[AutoFakeBold=1.5,AutoFakeSlant=0.2]
\setCJKmonofont{SimHei}[AutoFakeBold=1.5,AutoFakeSlant=0.2]
)TEX"},
      {"microsoft-yahei", "cjk", "Microsoft YaHei", "system", "Windows Microsoft YaHei CJK text",
       {{RequirementRoot::SystemFonts, "msyh.ttc"}, {RequirementRoot::SystemFonts, "msyhbd.ttc"}},
       R"TEX(\usepackage{xeCJK}
\setCJKmainfont{Microsoft YaHei}[BoldFont={Microsoft YaHei Bold}]
\setCJKsansfont{Microsoft YaHei}[BoldFont={Microsoft YaHei Bold}]
\setCJKmonofont{Microsoft YaHei}[BoldFont={Microsoft YaHei Bold}]
)TEX"},
      {"fandol-song", "cjk", "Fandol Song", "texlive", "TeX Live Fandol Song, Kai, Hei, and Fang stack",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolSong-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolSong-Bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolKai-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolHei-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolHei-Bold.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolFang-Regular.otf"}},
       R"TEX(\usepackage{xeCJK}
\setCJKmainfont{FandolSong-Regular.otf}[
  BoldFont=FandolSong-Bold.otf,
  ItalicFont=FandolKai-Regular.otf
]
\setCJKsansfont{FandolHei-Regular.otf}[BoldFont=FandolHei-Bold.otf]
\setCJKmonofont{FandolFang-Regular.otf}
)TEX"},
      {"fandol-hei", "cjk", "Fandol Hei", "texlive", "TeX Live Fandol sans-serif CJK text",
       {{RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolHei-Regular.otf"},
        {RequirementRoot::TexmfDist, "fonts/opentype/public/fandol/FandolHei-Bold.otf"}},
       R"TEX(\usepackage{xeCJK}
\setCJKmainfont{FandolHei-Regular.otf}[BoldFont=FandolHei-Bold.otf]
\setCJKsansfont{FandolHei-Regular.otf}[BoldFont=FandolHei-Bold.otf]
\setCJKmonofont{FandolHei-Regular.otf}[BoldFont=FandolHei-Bold.otf]
)TEX"},
  };
  return value;
}

fs::path utf8_path(const char *value) {
  return fs::u8path(value ? value : "");
}

std::string path_utf8(const fs::path &path) {
  return path.u8string();
}

fs::path system_fonts_root() {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH]{};
  UINT count = GetWindowsDirectoryW(buffer, MAX_PATH);
  if (count > 0 && count < MAX_PATH) return fs::path(buffer) / L"Fonts";
#endif
  return fs::path();
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

char *copy_string(const std::string &value) {
  char *copy = static_cast<char *>(std::malloc(value.size() + 1));
  if (!copy) return nullptr;
  std::memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

void clear_outputs(StemTeXProfileErrorCode *code, char **error) {
  if (code) *code = STEMTEX_PROFILE_OK;
  if (error) *error = nullptr;
}

void set_error(StemTeXProfileErrorCode value, const std::string &message,
               StemTeXProfileErrorCode *code, char **error) {
  if (code) *code = value;
  if (error) *error = copy_string(message);
}

fs::path require_texmf_root(const StemTeXProfileContext *context) {
  if (!context || !context->texmf_root_utf8 || !*context->texmf_root_utf8) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, "texmf_root_utf8 is required");
  }
  fs::path root = fs::absolute(utf8_path(context->texmf_root_utf8)).lexically_normal();
  if (!fs::is_directory(root / "texmf-dist") || !fs::is_directory(root / "texmf-dist" / "web2c")) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_BAD_TEXMF_ROOT,
                           "TeX Live root must contain texmf-dist and texmf-dist/web2c: " + path_utf8(root));
  }
  return root;
}

fs::path requirement_path(const fs::path &texmf_root, const Requirement &requirement) {
  if (requirement.root == RequirementRoot::TexmfDist) {
    return texmf_root / "texmf-dist" / utf8_path(requirement.relative_path);
  }
  return system_fonts_root() / utf8_path(requirement.relative_path);
}

std::vector<std::string> missing_requirements(const std::vector<Requirement> &requirements,
                                              const fs::path &texmf_root) {
  std::vector<std::string> missing;
  for (const Requirement &requirement : requirements) {
    fs::path path = requirement_path(texmf_root, requirement);
    if (path.empty() || !fs::is_regular_file(path)) missing.push_back(requirement.relative_path);
  }
  return missing;
}

std::vector<std::string> missing_requirements(const FontRecipe &recipe, const fs::path &texmf_root) {
  return missing_requirements(recipe.requirements, texmf_root);
}

std::vector<std::string> missing_requirements(const PackageRecipe &recipe, const fs::path &texmf_root) {
  return missing_requirements(std::vector<Requirement>{recipe.requirement}, texmf_root);
}

const FontRecipe &find_recipe(const char *role, const char *id) {
  if (!id || !*id) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, std::string(role) + " font id is required");
  }
  for (const FontRecipe &recipe : recipes()) {
    if (std::string(recipe.role) == role && std::string(recipe.id) == id) return recipe;
  }
  throw ProfileException(STEMTEX_PROFILE_ERROR_UNKNOWN_FONT,
                         "Unknown " + std::string(role) + " font id: " + id);
}

void validate_recipe_available(const FontRecipe &recipe, const fs::path &texmf_root) {
  std::vector<std::string> missing = missing_requirements(recipe, texmf_root);
  if (missing.empty()) return;
  std::ostringstream message;
  message << recipe.display_name << " is unavailable; missing";
  for (const std::string &item : missing) message << " " << item;
  throw ProfileException(STEMTEX_PROFILE_ERROR_FONT_UNAVAILABLE, message.str());
}

const PackageRecipe &find_package_recipe(const std::string &id) {
  if (id.empty()) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, "package id is required");
  }
  for (const PackageRecipe &recipe : package_recipes()) {
    if (id == recipe.id) return recipe;
  }
  throw ProfileException(STEMTEX_PROFILE_ERROR_UNKNOWN_PACKAGE, "Unknown whitelist package id: " + id);
}

void validate_recipe_available(const PackageRecipe &recipe, const fs::path &texmf_root) {
  std::vector<std::string> missing = missing_requirements(recipe, texmf_root);
  if (missing.empty()) return;
  std::ostringstream message;
  message << recipe.display_name << " is unavailable; missing";
  for (const std::string &item : missing) message << " " << item;
  throw ProfileException(STEMTEX_PROFILE_ERROR_PACKAGE_UNAVAILABLE, message.str());
}

const char *package_phase_id(PackagePhase phase) {
  return phase == PackagePhase::BeforeFonts ? "before-fonts" : "after-fonts";
}

int package_phase_rank(PackagePhase phase) {
  return phase == PackagePhase::BeforeFonts ? 0 : 1;
}

std::vector<std::string> default_package_ids() {
  std::vector<std::string> result;
  for (const PackageRecipe &recipe : package_recipes()) {
    if (recipe.default_enabled) result.emplace_back(recipe.id);
  }
  return result;
}

struct ResolvedPackages {
  std::set<std::string> explicit_ids;
  std::vector<const PackageRecipe *> ordered;
  std::map<std::string, std::vector<std::string>> required_by;
};

ResolvedPackages resolve_packages(const fs::path &texmf_root, const std::vector<std::string> &explicit_ids) {
  ResolvedPackages result;
  std::map<std::string, const PackageRecipe *> selected;
  std::map<std::string, int> visit_state;

  for (const std::string &id : explicit_ids) {
    if (!result.explicit_ids.insert(id).second) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT,
                             "Whitelist package is selected more than once: " + id);
    }
  }

  std::function<void(const std::string &, const std::string *)> include_recipe;
  include_recipe = [&](const std::string &id, const std::string *required_by) {
    const PackageRecipe &recipe = find_package_recipe(id);
    if (required_by) {
      std::vector<std::string> &parents = result.required_by[id];
      if (std::find(parents.begin(), parents.end(), *required_by) == parents.end()) parents.push_back(*required_by);
    }
    int &state = visit_state[id];
    if (state == 1) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL,
                             "Cycle in whitelist package requirements at " + id);
    }
    if (state == 2) return;
    state = 1;
    selected[id] = &recipe;
    for (const char *dependency : recipe.requires) {
      const std::string parent = recipe.id;
      include_recipe(dependency, &parent);
    }
    state = 2;
  };

  for (const std::string &id : explicit_ids) include_recipe(id, nullptr);
  for (const auto &entry : selected) validate_recipe_available(*entry.second, texmf_root);

  std::set<std::string> emitted;
  while (result.ordered.size() < selected.size()) {
    const PackageRecipe *candidate = nullptr;
    for (const auto &entry : selected) {
      const PackageRecipe &recipe = *entry.second;
      if (emitted.count(recipe.id)) continue;
      bool ready = true;
      auto wait_for = [&](const char *id) {
        auto found = selected.find(id);
        if (found == selected.end()) return;
        if (package_phase_rank(found->second->phase) > package_phase_rank(recipe.phase)) {
          throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL,
                                 "Invalid whitelist package phase dependency: " + std::string(recipe.id) +
                                     " waits for " + id);
        }
        if (!emitted.count(id)) ready = false;
      };
      for (const char *id : recipe.requires) wait_for(id);
      for (const char *id : recipe.after) wait_for(id);
      if (!ready) continue;
      if (!candidate || package_phase_rank(recipe.phase) < package_phase_rank(candidate->phase) ||
          (recipe.phase == candidate->phase && recipe.order < candidate->order) ||
          (recipe.phase == candidate->phase && recipe.order == candidate->order &&
           std::string(recipe.id) < candidate->id)) {
        candidate = &recipe;
      }
    }
    if (!candidate) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL,
                             "Cycle in whitelist package load-order constraints");
    }
    result.ordered.push_back(candidate);
    emitted.insert(candidate->id);
  }
  return result;
}

std::vector<std::string> package_ids_from_api(const char *const *ids, size_t count) {
  if (count > 0 && !ids) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT,
                           "package_ids_utf8 is required when package_count is nonzero");
  }
  std::vector<std::string> result;
  result.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    if (!ids[index] || !*ids[index]) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT,
                             "package_ids_utf8 contains an empty package id");
    }
    result.emplace_back(ids[index]);
  }
  return result;
}

void append_package_tex(std::ostringstream &preamble, const PackageRecipe &recipe) {
  preamble << "\\usepackage";
  if (recipe.options && *recipe.options) preamble << '[' << recipe.options << ']';
  preamble << '{' << recipe.id << "}\n";
  if (recipe.after_load_tex && *recipe.after_load_tex) preamble << recipe.after_load_tex;
}

void validate_base(const fs::path &texmf_root, bool needs_cjk) {
  const std::vector<const char *> required = {
      "tex/latex/base/article.cls",
      "tex/latex/preview/preview.sty",
      "tex/latex/fontspec/fontspec.sty",
      "tex/latex/unicode-math/unicode-math.sty",
      "fonts/opentype/public/lm/lmsans10-regular.otf",
      "fonts/opentype/public/lm/lmsans10-bold.otf",
      "fonts/opentype/public/lm/lmsans10-oblique.otf",
      "fonts/opentype/public/lm/lmsans10-boldoblique.otf",
      "fonts/opentype/public/lm/lmmono10-regular.otf",
      "fonts/opentype/public/lm/lmmono10-italic.otf",
  };
  for (const char *relative : required) {
    if (!fs::is_regular_file(texmf_root / "texmf-dist" / utf8_path(relative))) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_FONT_UNAVAILABLE,
                             std::string("Selected TeX Live tree is missing profile dependency: ") + relative);
    }
  }
  if (needs_cjk && !fs::is_regular_file(texmf_root / "texmf-dist" / "tex/xelatex/xecjk/xeCJK.sty")) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_FONT_UNAVAILABLE,
                           "Selected TeX Live tree is missing profile dependency: tex/xelatex/xecjk/xeCJK.sty");
  }
}

struct GeneratedProfile {
  const FontRecipe *text;
  const FontRecipe *math;
  const FontRecipe *cjk;
  ResolvedPackages packages;
  std::string preamble;
  std::string warmup;
  std::string fingerprint;
};

struct InternalProfileSpec {
  const char *name = nullptr;
  const char *text_font_id = nullptr;
  const char *math_font_id = nullptr;
  const char *cjk_font_id = nullptr;
  std::vector<std::string> package_ids;
};

InternalProfileSpec legacy_spec(const StemTeXProfileSpec *spec) {
  if (!spec) throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, "profile spec is required");
  return {spec->name_utf8, spec->text_font_id_utf8, spec->math_font_id_utf8,
          spec->cjk_font_id_utf8, default_package_ids()};
}

InternalProfileSpec package_aware_spec(const StemTeXProfileSpecV2 *spec) {
  if (!spec) throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, "profile spec is required");
  return {spec->name_utf8, spec->text_font_id_utf8, spec->math_font_id_utf8,
          spec->cjk_font_id_utf8, package_ids_from_api(spec->package_ids_utf8, spec->package_count)};
}

std::string fingerprint(const std::string &value) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

GeneratedProfile generate_profile(const StemTeXProfileContext *context, const InternalProfileSpec &spec) {
  fs::path texmf_root = require_texmf_root(context);
  const FontRecipe &text = find_recipe("text", spec.text_font_id);
  const FontRecipe &math = find_recipe("math", spec.math_font_id);
  const FontRecipe &cjk = find_recipe("cjk", spec.cjk_font_id);
  validate_recipe_available(text, texmf_root);
  validate_recipe_available(math, texmf_root);
  validate_recipe_available(cjk, texmf_root);
  validate_base(texmf_root, std::string(cjk.id) != "none");
  ResolvedPackages packages = resolve_packages(texmf_root, spec.package_ids);

  std::ostringstream preamble;
  preamble << "% Generated by StemTeX Profile Creator.\n"
           << "% Font recipes: text=" << text.id << ", math=" << math.id << ", cjk=" << cjk.id << ".\n"
           << "% Whitelist packages are dependency-resolved and emitted in managed order.\n"
           << "\\documentclass{article}\n";
  for (const PackageRecipe *recipe : packages.ordered) {
    if (recipe->phase == PackagePhase::BeforeFonts) append_package_tex(preamble, *recipe);
  }
  preamble << math.tex << text.tex << cjk.tex;
  for (const PackageRecipe *recipe : packages.ordered) {
    if (recipe->phase == PackagePhase::AfterFonts) append_package_tex(preamble, *recipe);
  }
  preamble << "\\usepackage[active,tightpage]{preview}\n"
           << "\\PreviewBorder=1pt\n";

  std::ostringstream warmup;
  warmup << "% Generated by StemTeX Profile Creator.\n"
         << "% This is a readiness probe, not a font coverage corpus.\n"
         << "\\input{preamble.tex}\n\n"
         << "\\begin{document}\n"
         << "\\begin{preview}\n\n"
         << "StemTeX warmup: ABC xyz 0123456789.\n"
         << "\\textbf{bold} \\textit{italic} \\textsf{sans} \\texttt{mono}.\n\n";
  if (std::string(cjk.id) != "none") {
    warmup << u8"中文预热：数学、化学、物理；\\textbf{中文粗体}；\\textit{中文斜体}。\n\n";
  }
  warmup << "\\[\n"
          << "  E=mc^2 \\quad \\alpha+\\beta=\\gamma \\quad \\mathcal{L}_{p} \\quad \\mathbf{x}\n"
          << "\\]\n\n";
  for (const PackageRecipe *recipe : packages.ordered) {
    if (recipe->warmup_probe && *recipe->warmup_probe) warmup << recipe->warmup_probe << '\n';
  }
  warmup << "\\end{preview}\n"
          << "\\end{document}\n";

  std::string preamble_text = preamble.str();
  std::string warmup_text = warmup.str();
  return {&text, &math, &cjk, std::move(packages), preamble_text, warmup_text,
          fingerprint(preamble_text + "\n" + warmup_text)};
}

void validate_profile_name(const char *name) {
  if (!name || !*name) throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, "profile name is required");
  std::string value(name);
  if (value == "." || value == ".." || value.find_first_of("<>:\"/\\|?*") != std::string::npos ||
      value.back() == '.' || value.back() == ' ') {
    throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT,
                           "profile name must be a single valid Windows directory name");
  }
  for (unsigned char ch : value) {
    if (ch < 0x20) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT,
                             "profile name contains a control character");
    }
  }
  std::string stem = value.substr(0, value.find('.'));
  for (char &ch : stem) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  static const std::vector<std::string> reserved = {
      "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
      "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const std::string &item : reserved) {
    if (stem == item) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT,
                             "profile name is reserved by Windows: " + value);
    }
  }
}

void write_text(const fs::path &path, const std::string &text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw ProfileException(STEMTEX_PROFILE_ERROR_FILESYSTEM, "Cannot write " + path_utf8(path));
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream) throw ProfileException(STEMTEX_PROFILE_ERROR_FILESYSTEM, "Cannot finish writing " + path_utf8(path));
}

std::string profile_manifest(const InternalProfileSpec &spec, const GeneratedProfile &generated) {
  std::ostringstream out;
  out << "{\n"
      << "  \"schemaVersion\": 2,\n"
      << "  \"generator\": \"StemTeX Profile Creator " << json_escape(kVersion) << "\",\n"
      << "  \"name\": \"" << json_escape(spec.name ? spec.name : "") << "\",\n"
      << "  \"fonts\": {\n"
      << "    \"text\": \"" << generated.text->id << "\",\n"
      << "    \"math\": \"" << generated.math->id << "\",\n"
      << "    \"cjk\": \"" << generated.cjk->id << "\"\n"
      << "  },\n"
      << "  \"packages\": {\n"
      << "    \"selected\": [";
  for (size_t index = 0; index < spec.package_ids.size(); ++index) {
    if (index) out << ", ";
    out << "\"" << json_escape(spec.package_ids[index]) << "\"";
  }
  out << "],\n"
      << "    \"resolvedOrder\": [";
  for (size_t index = 0; index < generated.packages.ordered.size(); ++index) {
    if (index) out << ", ";
    out << "\"" << generated.packages.ordered[index]->id << "\"";
  }
  out << "]\n"
      << "  },\n"
      << "  \"fingerprint\": \"" << generated.fingerprint << "\"\n"
      << "}\n";
  return out.str();
}

std::string unique_temp_name() {
  static std::atomic<unsigned long> serial{0};
  std::ostringstream name;
#ifdef _WIN32
  name << ".stemtex-profile-" << GetCurrentProcessId() << "-";
#else
  name << ".stemtex-profile-";
#endif
  name << ++serial;
  return name.str();
}

std::string package_plan_json(const ResolvedPackages &packages) {
  std::ostringstream json;
  json << "{\"schemaVersion\":1,\"packages\":[";
  for (size_t index = 0; index < packages.ordered.size(); ++index) {
    const PackageRecipe &recipe = *packages.ordered[index];
    if (index) json << ',';
    json << "{\"id\":\"" << recipe.id << "\",\"displayName\":\""
         << json_escape(recipe.display_name) << "\",\"explicit\":"
         << (packages.explicit_ids.count(recipe.id) ? "true" : "false")
         << ",\"phase\":\"" << package_phase_id(recipe.phase) << "\",\"requiredBy\":[";
    auto parents = packages.required_by.find(recipe.id);
    if (parents != packages.required_by.end()) {
      for (size_t parent_index = 0; parent_index < parents->second.size(); ++parent_index) {
        if (parent_index) json << ',';
        json << "\"" << json_escape(parents->second[parent_index]) << "\"";
      }
    }
    json << "]}";
  }
  json << "],\"loadOrder\":[";
  bool first = true;
  auto append_load_item = [&](const char *kind, const char *id, const char *display_name) {
    if (!first) json << ',';
    first = false;
    json << "{\"kind\":\"" << kind << "\",\"id\":\"" << id
         << "\",\"displayName\":\"" << json_escape(display_name) << "\"}";
  };
  for (const PackageRecipe *recipe : packages.ordered) {
    if (recipe->phase == PackagePhase::BeforeFonts) append_load_item("package", recipe->id, recipe->display_name);
  }
  append_load_item("managed", "fonts", "Font recipes");
  for (const PackageRecipe *recipe : packages.ordered) {
    if (recipe->phase == PackagePhase::AfterFonts) append_load_item("package", recipe->id, recipe->display_name);
  }
  append_load_item("managed", "preview", "StemTeX preview");
  json << "]}";
  return json.str();
}

int materialize_profile(const StemTeXProfileContext *context, const InternalProfileSpec &spec,
                        const char *profiles_root_utf8, char **result_json_utf8) {
  if (!profiles_root_utf8 || !*profiles_root_utf8) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_INVALID_ARGUMENT, "profiles root is required");
  }
  validate_profile_name(spec.name);
  GeneratedProfile generated = generate_profile(context, spec);
  fs::path profiles_root = fs::absolute(utf8_path(profiles_root_utf8)).lexically_normal();
  fs::create_directories(profiles_root);
  fs::path target = profiles_root / utf8_path(spec.name);
  if (fs::exists(target)) {
    throw ProfileException(STEMTEX_PROFILE_ERROR_PROFILE_EXISTS,
                           "Profile already exists: " + path_utf8(target));
  }

  fs::path temporary = profiles_root / unique_temp_name();
  while (fs::exists(temporary)) temporary = profiles_root / unique_temp_name();
  fs::create_directory(temporary);
  try {
    write_text(temporary / "preamble.tex", generated.preamble);
    write_text(temporary / "warmup.tex", generated.warmup);
    write_text(temporary / "profile.json", profile_manifest(spec, generated));
    fs::rename(temporary, target);
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(temporary, ignored);
    throw;
  }

  std::ostringstream result;
  result << "{\"profilePath\":\"" << json_escape(path_utf8(target))
         << "\",\"fingerprint\":\"" << generated.fingerprint << "\"}";
  if (result_json_utf8) {
    *result_json_utf8 = copy_string(result.str());
    if (!*result_json_utf8) {
      throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL, "Cannot allocate materialize result JSON");
    }
  }
  return 1;
}

template <typename Function>
auto api_call(Function &&function, StemTeXProfileErrorCode *error_code, char **error_utf8)
    -> decltype(function()) {
  clear_outputs(error_code, error_utf8);
  try {
    return function();
  } catch (const ProfileException &error) {
    set_error(error.code, error.what(), error_code, error_utf8);
  } catch (const fs::filesystem_error &error) {
    set_error(STEMTEX_PROFILE_ERROR_FILESYSTEM, error.what(), error_code, error_utf8);
  } catch (const std::exception &error) {
    set_error(STEMTEX_PROFILE_ERROR_INTERNAL, error.what(), error_code, error_utf8);
  } catch (...) {
    set_error(STEMTEX_PROFILE_ERROR_INTERNAL, "Unknown internal profile creator error", error_code, error_utf8);
  }
  using Return = decltype(function());
  return Return{};
}

}  // namespace

extern "C" {

STEMTEX_PROFILE_API char *stemtex_profile_font_catalog_json(
    const StemTeXProfileContext *context, StemTeXProfileErrorCode *error_code, char **error_utf8) {
  return api_call([&]() -> char * {
    fs::path texmf_root = require_texmf_root(context);
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"texmfRoot\":\"" << json_escape(path_utf8(texmf_root)) << "\",\"fonts\":[";
    bool first = true;
    for (const FontRecipe &recipe : recipes()) {
      std::vector<std::string> missing = missing_requirements(recipe, texmf_root);
      if (!first) json << ',';
      first = false;
      json << "{\"id\":\"" << recipe.id << "\",\"role\":\"" << recipe.role
           << "\",\"displayName\":\"" << json_escape(recipe.display_name)
           << "\",\"source\":\"" << recipe.source
           << "\",\"description\":\"" << json_escape(recipe.description)
           << "\",\"available\":" << (missing.empty() ? "true" : "false") << ",\"missing\":[";
      for (size_t index = 0; index < missing.size(); ++index) {
        if (index) json << ',';
        json << "\"" << json_escape(missing[index]) << "\"";
      }
      json << "]}";
    }
    json << "]}";
    char *result = copy_string(json.str());
    if (!result) throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL, "Cannot allocate catalog JSON");
    return result;
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API char *stemtex_profile_package_catalog_json(
    const StemTeXProfileContext *context, StemTeXProfileErrorCode *error_code, char **error_utf8) {
  return api_call([&]() -> char * {
    fs::path texmf_root = require_texmf_root(context);
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"texmfRoot\":\"" << json_escape(path_utf8(texmf_root))
         << "\",\"packages\":[";
    bool first = true;
    for (const PackageRecipe &recipe : package_recipes()) {
      std::vector<std::string> missing = missing_requirements(recipe, texmf_root);
      if (!first) json << ',';
      first = false;
      json << "{\"id\":\"" << recipe.id << "\",\"displayName\":\""
           << json_escape(recipe.display_name) << "\",\"category\":\"" << recipe.category
           << "\",\"description\":\"" << json_escape(recipe.description)
           << "\",\"relativePath\":\"" << json_escape(recipe.requirement.relative_path)
           << "\",\"defaultEnabled\":" << (recipe.default_enabled ? "true" : "false")
           << ",\"phase\":\"" << package_phase_id(recipe.phase) << "\",\"order\":" << recipe.order
           << ",\"options\":\"" << json_escape(recipe.options ? recipe.options : "")
           << "\",\"afterLoadTex\":\"" << json_escape(recipe.after_load_tex ? recipe.after_load_tex : "")
           << "\",\"available\":" << (missing.empty() ? "true" : "false") << ",\"requires\":[";
      for (size_t index = 0; index < recipe.requires.size(); ++index) {
        if (index) json << ',';
        json << "\"" << json_escape(recipe.requires[index]) << "\"";
      }
      json << "],\"after\":[";
      for (size_t index = 0; index < recipe.after.size(); ++index) {
        if (index) json << ',';
        json << "\"" << json_escape(recipe.after[index]) << "\"";
      }
      json << "],\"missing\":[";
      for (size_t index = 0; index < missing.size(); ++index) {
        if (index) json << ',';
        json << "\"" << json_escape(missing[index]) << "\"";
      }
      json << "]}";
    }
    json << "]}";
    char *result = copy_string(json.str());
    if (!result) throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL, "Cannot allocate package catalog JSON");
    return result;
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API char *stemtex_profile_package_plan_json(
    const StemTeXProfileContext *context, const char *const *package_ids_utf8, size_t package_count,
    StemTeXProfileErrorCode *error_code, char **error_utf8) {
  return api_call([&]() -> char * {
    fs::path texmf_root = require_texmf_root(context);
    ResolvedPackages packages = resolve_packages(texmf_root,
                                                 package_ids_from_api(package_ids_utf8, package_count));
    char *result = copy_string(package_plan_json(packages));
    if (!result) throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL, "Cannot allocate package plan JSON");
    return result;
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API char *stemtex_profile_preamble_utf8(
    const StemTeXProfileContext *context, const StemTeXProfileSpec *spec,
    StemTeXProfileErrorCode *error_code, char **error_utf8) {
  return api_call([&]() -> char * {
    GeneratedProfile generated = generate_profile(context, legacy_spec(spec));
    char *result = copy_string(generated.preamble);
    if (!result) throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL, "Cannot allocate generated preamble");
    return result;
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API char *stemtex_profile_preamble_v2_utf8(
    const StemTeXProfileContext *context, const StemTeXProfileSpecV2 *spec,
    StemTeXProfileErrorCode *error_code, char **error_utf8) {
  return api_call([&]() -> char * {
    GeneratedProfile generated = generate_profile(context, package_aware_spec(spec));
    char *result = copy_string(generated.preamble);
    if (!result) throw ProfileException(STEMTEX_PROFILE_ERROR_INTERNAL, "Cannot allocate generated preamble");
    return result;
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API int stemtex_profile_materialize(
    const StemTeXProfileContext *context, const StemTeXProfileSpec *spec,
    const char *profiles_root_utf8, char **result_json_utf8,
    StemTeXProfileErrorCode *error_code, char **error_utf8) {
  if (result_json_utf8) *result_json_utf8 = nullptr;
  return api_call([&]() -> int {
    return materialize_profile(context, legacy_spec(spec), profiles_root_utf8, result_json_utf8);
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API int stemtex_profile_materialize_v2(
    const StemTeXProfileContext *context, const StemTeXProfileSpecV2 *spec,
    const char *profiles_root_utf8, char **result_json_utf8,
    StemTeXProfileErrorCode *error_code, char **error_utf8) {
  if (result_json_utf8) *result_json_utf8 = nullptr;
  return api_call([&]() -> int {
    return materialize_profile(context, package_aware_spec(spec), profiles_root_utf8, result_json_utf8);
  }, error_code, error_utf8);
}

STEMTEX_PROFILE_API const char *stemtex_profile_version(void) { return kVersion; }
STEMTEX_PROFILE_API const char *stemtex_profile_abi_version(void) { return kAbiVersion; }
STEMTEX_PROFILE_API void stemtex_profile_free_string(char *value) { std::free(value); }

}  // extern "C"
