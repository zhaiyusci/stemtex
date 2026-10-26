#include "stemtex_block_layout.h"

#include <string_view>

namespace stemtex {
namespace {
bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Only recognize literal leading displays. Do not expand author macros or
// reinterpret their catcodes. Comments may occur between environment tokens.
void skip_space_comments(const std::string &source, size_t &pos) {
  while (pos < source.size()) {
    if (space(source[pos])) ++pos;
    else if (source[pos] == '%') {
      while (pos < source.size() && source[pos] != '\r' && source[pos] != '\n') ++pos;
    } else break;
  }
}

bool leading_display(const std::string &source, size_t &pos) {
  pos = 0;
  skip_space_comments(source, pos);
  const std::string_view rest(source.data() + pos, source.size() - pos);
  if (rest.substr(0, 2) == "$$" || rest.substr(0, 2) == "\\[") return true;
  if (rest.substr(0, 6) != "\\begin") return false;
  size_t cursor = pos + 6;
  skip_space_comments(source, cursor);
  if (cursor == source.size() || source[cursor++] != '{') return false;
  skip_space_comments(source, cursor);
  const size_t start = cursor;
  while (cursor < source.size() &&
         ((source[cursor] >= 'a' && source[cursor] <= 'z') || source[cursor] == '*')) ++cursor;
  const std::string name = source.substr(start, cursor - start);
  skip_space_comments(source, cursor);
  if (cursor == source.size() || source[cursor] != '}') return false;
  for (const char *display : {"displaymath", "equation", "equation*", "align", "align*",
                              "gather", "gather*", "multline", "multline*", "flalign",
                              "flalign*", "alignat", "alignat*"}) {
    if (name == display) return true;
  }
  return false;
}
}

std::string prepare_block_source(const std::string &source) {
  size_t first = 0;
  const bool display = leading_display(source, first);
  // Private registers are allocated once per worker. Register values and aliases
  // are local to this request; the existing preview border is left untouched.
  std::string result = R"TEX(\begingroup%
\ifcsname stemtex@block@body\endcsname\else\expandafter\newbox\csname stemtex@block@body\endcsname\fi%
\ifcsname stemtex@block@first\endcsname\else\expandafter\newbox\csname stemtex@block@first\endcsname\fi%
\expandafter\let\expandafter\StemTeXBlockBody\csname stemtex@block@body\endcsname%
\expandafter\let\expandafter\StemTeXBlockFirst\csname stemtex@block@first\endcsname%
\let\StemTeXBlockFinish\relax%
\def\StemTeXBlockTextEnd{\def\StemTeXBlockFinish{\ifdim\dp\StemTeXBlockBody<\dp\strutbox\dp\StemTeXBlockBody=\dp\strutbox\fi}}%
)TEX";
  if (!display) result += "\\setbox\\StemTeXBlockBody=\\vbox{\\begingroup%\n";
  result.append(source, 0, first);
  if (display) {
    // Keep the requested width, without creating a text line before the display.
    result += "\\hrule width\\hsize height0pt depth0pt\\relax\\noindent%\n";
  }
  result.append(source, first, std::string::npos);
  size_t backslashes = 0;
  for (size_t i = source.size(); i && source[i - 1] == '\\'; --i) ++backslashes;
  result += backslashes % 2 ? "\n" : "%\n";
  if (display) {
    // A nested vbox changes leading-display spacing in the worker minipage.
    // Typeset directly in that minipage instead. If prose follows the display,
    // extend only its final hlist, retaining the already calculated baseline
    // glue. A pure display ends in glue and is left completely untouched.
    result += R"TEX(\par%
\ifnum\lastnodetype=1\relax%
\setbox\StemTeXBlockBody=\lastbox%
\ifdim\dp\StemTeXBlockBody<\dp\strutbox\dp\StemTeXBlockBody=\dp\strutbox\fi%
\nointerlineskip\box\StemTeXBlockBody%
\fi%
\endgroup
)TEX";
    return result;
  }
  // Inspect the last paragraph node before colour/aftergroup whatsits are added.
  // Defer the depth correction until after the vbox and its inner group close.
  result += R"TEX(\par%
\ifnum\lastnodetype=1\relax\aftergroup\aftergroup\aftergroup\StemTeXBlockTextEnd\fi%
\endgroup}%
)TEX";
  // A vtop exposes the first box's ascent. An actual kern moves the ink down;
  // changing only the reported box height would not move it in the output.
  // Leading glue/whatsits with no measurable ascent remain author-owned.
  result += R"TEX(\setbox\StemTeXBlockFirst=\vtop{\unvcopy\StemTeXBlockBody}%
\ifdim\ht\StemTeXBlockFirst>0pt\relax%
\ifdim\ht\StemTeXBlockFirst<\ht\strutbox%
\setbox\StemTeXBlockBody=\vbox{\kern\dimexpr\ht\strutbox-\ht\StemTeXBlockFirst\relax\unvbox\StemTeXBlockBody}%
\fi\fi%
)TEX";
  result += "\\StemTeXBlockFinish%\n\\box\\StemTeXBlockBody%\n\\endgroup\n";
  return result;
}
}
