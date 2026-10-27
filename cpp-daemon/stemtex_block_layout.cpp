#include "stemtex_block_layout.h"

namespace stemtex {
std::string prepare_block_source(const std::string &source) {
  // Use the selected font's strut as the minimum first-line ascent and
  // last-line descent. Typeset once; leave TeX's paragraphs, displays and
  // colour operations alone. All box contents and aliases are request-local.
  std::string result = R"TEX(\begingroup%
\ifcsname stemtex@block@body\endcsname\else\expandafter\newbox\csname stemtex@block@body\endcsname\fi%
\ifcsname stemtex@block@first\endcsname\else\expandafter\newbox\csname stemtex@block@first\endcsname\fi%
\expandafter\let\expandafter\StemTeXBlockBody\csname stemtex@block@body\endcsname%
\expandafter\let\expandafter\StemTeXBlockFirst\csname stemtex@block@first\endcsname%
\let\StemTeXBlockFinish\relax%
\def\StemTeXBlockTextEnd{\def\StemTeXBlockFinish{\ifdim\dp\StemTeXBlockBody<\dp\strutbox\dp\StemTeXBlockBody=\dp\strutbox\fi}}%
\setbox\StemTeXBlockBody=\vbox{\begingroup\noindent%
)TEX";
  result += source;
  // Terminate a final comment without introducing whitespace into the body.
  size_t backslashes = 0;
  for (size_t i = source.size(); i && source[i - 1] == '\\'; --i) ++backslashes;
  result += backslashes % 2 ? "\n" : "%\n";
  result += R"TEX(\par%
\ifnum\lastnodetype=1\relax\aftergroup\aftergroup\aftergroup\StemTeXBlockTextEnd\fi%
\endgroup}%
\setbox\StemTeXBlockFirst=\vtop{\unvcopy\StemTeXBlockBody}%
\ifdim\ht\StemTeXBlockFirst>0pt\relax%
\ifdim\ht\StemTeXBlockFirst<\ht\strutbox%
\setbox\StemTeXBlockBody=\vbox{\kern\dimexpr\ht\strutbox-\ht\StemTeXBlockFirst\relax\unvbox\StemTeXBlockBody}%
\fi\fi%
\StemTeXBlockFinish%
\wd\StemTeXBlockBody=\hsize%
\box\StemTeXBlockBody%
\endgroup
)TEX";
  return result;
}
}
