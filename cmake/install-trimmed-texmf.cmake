function(stemtex_install_texmf dist engine)
  set(src "${STEMTEX_TEXLIVE_ROOT}/texmf-dist")
  set(STEMTEX_TEXMF_FILES
    dvipdfmx/dvipdfmx.cfg
    fonts/map/fontname/texfonts.map
    fonts/opentype/public/lm/lmmono10-italic.otf
    fonts/opentype/public/lm/lmmono10-regular.otf
    fonts/opentype/public/lm/lmroman10-bold.otf
    fonts/opentype/public/lm/lmroman10-bolditalic.otf
    fonts/opentype/public/lm/lmroman10-italic.otf
    fonts/opentype/public/lm/lmroman10-regular.otf
    fonts/opentype/public/lm/lmsans10-bold.otf
    fonts/opentype/public/lm/lmsans10-boldoblique.otf
    fonts/opentype/public/lm/lmsans10-oblique.otf
    fonts/opentype/public/lm/lmsans10-regular.otf
    fonts/opentype/public/lm-math/latinmodern-math.otf
    tex/generic/babel/hyphen.cfg
    tex/generic/hyphen/dumyhyph.tex
    tex/generic/hyphen/hyphen.tex
    tex/generic/hyphen/zerohyph.tex
    web2c/fmtutil.cnf
    web2c/texmf.cnf
  )
  set(STEMTEX_TEXMF_DIRS
    fonts/tfm/public/cm
    fonts/tfm/public/amsfonts/cmextra
    fonts/type1/public/amsfonts/cmextra
    fonts/tfm/public/latex-fonts
    tex/latex/amsmath
    tex/latex/base
    tex/latex/tex-ini-files
    tex/latex/firstaid
    tex/latex/fontspec
    tex/generic/config
    tex/generic/iftex
    tex/generic/unicode-data
    tex/latex/l3backend
    tex/latex/l3kernel
    tex/latex/l3packages/l3keys2e
    tex/latex/l3packages/xparse
    tex/latex/l3packages/xtemplate
    tex/latex/preview
    tex/latex/unicode-math
    tex/latex/lm
  )
  if(engine STREQUAL "LuaHBTeX")
    list(APPEND STEMTEX_TEXMF_FILES
      tex/generic/tex-ini-files/luatexconfig.tex
      tex/generic/tex-ini-files/pdftexconfig.tex
      tex/generic/babel/luababel.def
      tex/generic/pdftex/glyphtounicode.tex
      tex/generic/luatex85/luatex85.sty
    )
    list(APPEND STEMTEX_TEXMF_DIRS
      tex/latex/etoolbox
      tex/lualatex/lualatex-math
      tex/luatex/lualibs
      tex/luatex/luaotfload
      tex/luatex/lua-uni-algos
    )
    install(DIRECTORY "${src}/fonts/type1/public/amsfonts/latxfont/"
      DESTINATION "${dist}/fonts/type1/public/amsfonts/latxfont" ${ARGN}
      FILES_MATCHING PATTERN "*.pfb")
  else()
    list(APPEND STEMTEX_TEXMF_FILES
      fonts/misc/xetex/fontmapping/base/tex-text.tec
      tex/generic/babel/xebabel.def)
  endif()

  foreach(rel IN LISTS STEMTEX_TEXMF_FILES)
    get_filename_component(parent "${rel}" DIRECTORY)
    install(FILES "${src}/${rel}" DESTINATION "${dist}/${parent}" ${ARGN})
  endforeach()
  foreach(rel IN LISTS STEMTEX_TEXMF_DIRS)
    get_filename_component(parent "${rel}" DIRECTORY)
    install(DIRECTORY "${src}/${rel}" DESTINATION "${dist}/${parent}" ${ARGN})
  endforeach()

  # The embedded classic math families use Type 1 outlines at varying sizes.
  install(DIRECTORY "${src}/fonts/type1/public/amsfonts/cm/"
    DESTINATION "${dist}/fonts/type1/public/amsfonts/cm" ${ARGN}
    FILES_MATCHING PATTERN "*.pfb")
endfunction()
