#include "stemtex_block_layout.h"

namespace stemtex {
std::string prepare_block_source(const std::string &source) {
  // Typeset between two actual strut-only lines, then remove exactly their
  // height + depth. No content inspection, extra margin or display special case.
  std::string result = R"TEX(\begingroup%
\ifcsname stemtex@block@body\endcsname\else\expandafter\newbox\csname stemtex@block@body\endcsname\fi%
\expandafter\let\expandafter\StemTeXBlockBody\csname stemtex@block@body\endcsname%
\edef\StemTeXBlockSupportSize{\the\dimexpr\ht\strutbox+\dp\strutbox\relax}%
\setbox\StemTeXBlockBody=\vbox{\noindent\strut\par\begingroup\noindent%
)TEX";
  result += source;
  size_t backslashes = 0;
  for (size_t i = source.size(); i && source[i - 1] == '\\'; --i) ++backslashes;
  result += backslashes % 2 ? "\n" : "%\n";
  result += R"TEX(\par\endgroup\noindent\strut\par}%
\setbox\StemTeXBlockBody=\vbox{\kern-\StemTeXBlockSupportSize\unvbox\StemTeXBlockBody\kern-\StemTeXBlockSupportSize}%
\wd\StemTeXBlockBody=\hsize%
\box\StemTeXBlockBody%
\endgroup
)TEX";
  return result;
}
}
