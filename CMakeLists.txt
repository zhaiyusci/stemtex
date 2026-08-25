cmake_minimum_required(VERSION 3.20)
project(StemTeX LANGUAGES CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" STEMTEX_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
string(STRIP "${STEMTEX_VERSION}" STEMTEX_VERSION)
if(NOT STEMTEX_VERSION)
  message(FATAL_ERROR "VERSION is empty")
endif()

option(STEMTEX_BUILD_GUI "Build the Qt StemTeX Renderer GUI" ON)
option(STEMTEX_INSTALL_TEXMF "Install the bundled trimmed texmf-dist tree" ON)

find_package(Python3 COMPONENTS Interpreter)

set(STEMTEX_TEXLIVE_ROOT "C:/texlive/2026" CACHE PATH "TeX Live root used as the texmf source")
set(STEMTEX_STANDALONE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/texlive-xetex/out/standalone-msvc"
    CACHE PATH "Directory containing daemon engine build outputs")
set(STEMTEX_RUNTIME_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/stemtex-texlive-daemon-static"
    CACHE PATH "Optional runtime side tree containing a prebuilt xelatexdaemon.fmt")

add_subdirectory(cpp-daemon)
add_subdirectory(profile-creator)
if(STEMTEX_BUILD_GUI)
  add_subdirectory(gui)
endif()

set(STEMTEX_ICU_DATA "${CMAKE_CURRENT_SOURCE_DIR}/texlive-xetex/prebuilt-msvc/share/icu-data/icudt78l.dat")
set(STEMTEX_FORMAT_FILE "${STEMTEX_RUNTIME_SOURCE}/texmf-var/web2c/xetex/xelatexdaemon.fmt")

install(TARGETS stemtex-renderer stemtex-profile
  RUNTIME DESTINATION runtime/bin/sdk
  LIBRARY DESTINATION runtime/bin/sdk
  ARCHIVE DESTINATION runtime/sdk/lib
)
install(TARGETS stemtex-worker-host RUNTIME DESTINATION runtime/bin/windows)
install(FILES
  cpp-daemon/stemtex_renderer.h
  profile-creator/stemtex_profile.h
  DESTINATION runtime/sdk/include
)
install(FILES VERSION DESTINATION runtime)
install(FILES gui/assets/stemtex-renderer-gui.ico DESTINATION runtime RENAME StemTeX.ico)
install(FILES
  "${STEMTEX_STANDALONE_DIR}/xetexdaemon.exe"
  "${STEMTEX_STANDALONE_DIR}/xetexdaemon.dll"
  "${STEMTEX_STANDALONE_DIR}/xdvipdfmxdaemon.exe"
  "${STEMTEX_STANDALONE_DIR}/dvipdfmxdaemon.dll"
  "${STEMTEX_STANDALONE_DIR}/dvisvgmdaemon.exe"
  "${STEMTEX_STANDALONE_DIR}/dvisvgmdaemon.dll"
  DESTINATION runtime/bin/windows
)
install(FILES "${STEMTEX_ICU_DATA}" DESTINATION runtime/bin/windows/icu-data)
if(EXISTS "${STEMTEX_FORMAT_FILE}")
  install(FILES "${STEMTEX_FORMAT_FILE}" DESTINATION runtime/texmf-var/web2c/xetex)
else()
  message(STATUS "No prebuilt xelatexdaemon.fmt found; the renderer will generate it in the user cache")
endif()
install(FILES
  cmake/run-xelatexdaemon.bat
  cmake/refresh-profile-cache.bat
  DESTINATION runtime
)

if(STEMTEX_BUILD_GUI)
  install(TARGETS stemtex-renderer-gui stemtex-profile-creator RUNTIME DESTINATION gui)
  install(DIRECTORY "$<TARGET_FILE_DIR:stemtex-renderer-gui>/"
    DESTINATION gui
    FILES_MATCHING
      PATTERN "*.dll"
      PATTERN "*.exe"
      PATTERN "qt.conf"
      PATTERN "CMakeFiles" EXCLUDE
      PATTERN "*_autogen" EXCLUDE
      PATTERN "stemtex-renderer.dll" EXCLUDE
      PATTERN "stemtex-renderer.lib" EXCLUDE
      PATTERN "stemtex-renderer.exp" EXCLUDE
      PATTERN "stemtex-profile.dll" EXCLUDE
      PATTERN "stemtex-profile.lib" EXCLUDE
      PATTERN "stemtex-profile.exp" EXCLUDE
  )
  install(CODE [[
    file(REMOVE_RECURSE
      "${CMAKE_INSTALL_PREFIX}/gui/CMakeFiles"
      "${CMAKE_INSTALL_PREFIX}/gui/stemtex-renderer-gui_autogen"
      "${CMAKE_INSTALL_PREFIX}/gui/cmake_install.cmake"
    )
  ]])
endif()

install(DIRECTORY gui/profiles/unicodemath/
  DESTINATION gui/profiles/unicodemath
  FILES_MATCHING
    PATTERN "*.tex"
    PATTERN "*.aux" EXCLUDE
    PATTERN "*.log" EXCLUDE
    PATTERN "*.pdf" EXCLUDE
    PATTERN "*.xdv" EXCLUDE
    PATTERN "*.synctex.gz" EXCLUDE
)

if(STEMTEX_INSTALL_TEXMF)
  set(STEMTEX_TEXMF_DIST "${STEMTEX_TEXLIVE_ROOT}/texmf-dist")

  set(STEMTEX_TEXMF_FILES
    dvipdfmx/dvipdfmx.cfg
    fonts/misc/xetex/fontmapping/base/tex-text.tec
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
    tex/generic/babel/xebabel.def
    tex/generic/hyphen/dumyhyph.tex
    tex/generic/hyphen/hyphen.tex
    tex/generic/hyphen/zerohyph.tex
    web2c/fmtutil.cnf
    web2c/texmf.cnf
  )
  foreach(rel IN LISTS STEMTEX_TEXMF_FILES)
    get_filename_component(rel_dir "${rel}" DIRECTORY)
    install(FILES "${STEMTEX_TEXMF_DIST}/${rel}" DESTINATION "runtime/texmf-dist/${rel_dir}")
  endforeach()

  set(STEMTEX_TEXMF_DIRS
    fonts/tfm/public/cm
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
  foreach(rel IN LISTS STEMTEX_TEXMF_DIRS)
    get_filename_component(rel_parent "${rel}" DIRECTORY)
    install(DIRECTORY "${STEMTEX_TEXMF_DIST}/${rel}" DESTINATION "runtime/texmf-dist/${rel_parent}")
  endforeach()

  # Dynamic font-size requests can select the classic Computer Modern math
  # families embedded in the daemon format. Keep their metrics and Type 1
  # outlines, but omit the unused PFM companions and the rest of AMSFonts.
  install(DIRECTORY "${STEMTEX_TEXMF_DIST}/fonts/type1/public/amsfonts/cm/"
    DESTINATION "runtime/texmf-dist/fonts/type1/public/amsfonts/cm"
    FILES_MATCHING PATTERN "*.pfb"
  )

  install(CODE [=[
    set(runtime_root "${CMAKE_INSTALL_PREFIX}/runtime")
    set(cnf "${runtime_root}/texmf-dist/web2c/texmf.cnf")
    if(EXISTS "${cnf}")
      file(READ "${cnf}" existing_cnf)
      file(WRITE "${cnf}" "% StemTeX staged texmf-dist overrides.
% Keep this block at the top: kpathsea uses the first assignment it sees.
TEXMFROOT = $SELFAUTOPARENT
TEXMFDIST = $TEXMFROOT/texmf-dist
TEXMFMAIN = $TEXMFDIST
TEXMFSYSVAR = $TEXMFROOT/texmf-var
TEXMFSYSCONFIG = $TEXMFROOT/texmf-config
TEXMFVAR = $TEXMFROOT/texmf-var
TEXMFCONFIG = $TEXMFROOT/texmf-config
TEXMFLOCAL = $TEXMFROOT/texmf-local
TEXMFHOME = $TEXMFROOT/texmf-home
TEXMF = {$TEXMFVAR,$TEXMFCONFIG,$TEXMFDIST}
TEXMFDBS = {$TEXMFVAR,$TEXMFCONFIG,$TEXMFDIST}
SYSTEXMF = $TEXMFSYSVAR;$TEXMFLOCAL;$TEXMFDIST
TEXMFCACHE = $TEXMFVAR
VARTEXFONTS = $TEXMFVAR/fonts
TEXFORMATS = $TEXMFVAR/web2c/$engine;$TEXMFVAR/web2c
TEXPOOL = $TEXMFVAR/web2c/$engine;$TEXMFDIST/web2c
OSFONTDIR = C:/Windows/fonts
FC_CACHEDIR = $TEXMFVAR/fonts/cache
XE_FC_CACHEDIR = $TEXMFVAR/fonts/cache
FONTCONFIG_PATH = $TEXMFVAR/fonts/conf
XE_FONTCONFIG_PATH = $TEXMFVAR/fonts/conf

${existing_cnf}")
    endif()

    file(MAKE_DIRECTORY
      "${runtime_root}/texmf-dist/tex/generic/config"
      "${runtime_root}/texmf-var/fonts/conf/conf.d"
      "${runtime_root}/texmf-var/fonts/cache"
    )
    file(WRITE "${runtime_root}/texmf-dist/tex/generic/config/language.dat" "% StemTeX intentionally keeps format hyphenation minimal.
english hyphen.tex
=usenglish
=USenglish
=american
nohyphenation zerohyph.tex
dumylang dumyhyph.tex
")
    file(TO_CMAKE_PATH "${runtime_root}" runtime_xml)
    file(WRITE "${runtime_root}/texmf-var/fonts/conf/fonts.conf" "<?xml version=\"1.0\"?>
<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">
<fontconfig>
  <dir>C:/Windows/fonts</dir>
  <dir>${runtime_xml}/texmf-dist/fonts/opentype</dir>
  <dir>${runtime_xml}/texmf-dist/fonts/truetype</dir>
  <cachedir>${runtime_xml}/texmf-var/fonts/cache</cachedir>
  <include ignore_missing=\"yes\">conf.d</include>
  <config><rescan><int>30</int></rescan></config>
</fontconfig>
")
    file(WRITE "${runtime_root}/texmf-var/fonts/conf/conf.d/51-local.conf" "<?xml version=\"1.0\"?>
<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">
<fontconfig></fontconfig>
")
  ]=])
endif()

set(STEMTEX_STAGE_DEPENDS stemtex-renderer stemtex-profile stemtex-worker-host)
if(TARGET stemtex-renderer-gui)
  list(APPEND STEMTEX_STAGE_DEPENDS stemtex-renderer-gui stemtex-profile-creator)
endif()

add_custom_target(stage
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --config "$<CONFIG>" --prefix "${CMAKE_CURRENT_SOURCE_DIR}/staging"
  DEPENDS ${STEMTEX_STAGE_DEPENDS}
  COMMENT "Installing StemTeX staging tree"
)

if(Python3_Interpreter_FOUND)
  add_custom_target(stemtex-checkpoint-audit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit-stemtex-checkpoint.py"
            --repo "${CMAKE_CURRENT_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Auditing StemTeX XeTeX checkpoint state mapping"
  )
endif()

if(WIN32)
  add_custom_target(dvisvgmdaemon-msvc
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/texlive-xetex/build-dvisvgmdaemon-msvc.ps1"
            -OutDir "${STEMTEX_STANDALONE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building dvisvgmdaemon with MSVC"
  )
endif()
