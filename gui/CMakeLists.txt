cmake_minimum_required(VERSION 3.20)
project(stemtex_renderer_gui LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Pdf Svg LinguistTools)

if(NOT TARGET stemtex-renderer)
  add_subdirectory(../cpp-daemon cpp-daemon-build)
endif()

set(QSCINTILLA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(QSCINTILLA_INCLUDE_DIR "${QSCINTILLA_ROOT}/qscintilla-src/src")
set(QSCINTILLA_LIBRARY "${QSCINTILLA_ROOT}/qscintilla-build/release/qscintilla2_qt6.lib")
set(QSCINTILLA_DLL "${QSCINTILLA_ROOT}/qscintilla-build/release/qscintilla2_qt6.dll")
if(NOT EXISTS "${QSCINTILLA_LIBRARY}" OR NOT EXISTS "${QSCINTILLA_DLL}")
  message(FATAL_ERROR "QScintilla is not built. Expected ${QSCINTILLA_LIBRARY} and ${QSCINTILLA_DLL}")
endif()

add_executable(stemtex-renderer-gui WIN32
  main.cpp
  resources.qrc
  stemtex-renderer-gui.rc
)
target_compile_features(stemtex-renderer-gui PRIVATE cxx_std_17)
target_link_libraries(stemtex-renderer-gui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Pdf Qt6::Svg stemtex-renderer "${QSCINTILLA_LIBRARY}")
target_include_directories(stemtex-renderer-gui PRIVATE ../cpp-daemon "${QSCINTILLA_INCLUDE_DIR}")
if(MSVC)
  target_link_libraries(stemtex-renderer-gui PRIVATE delayimp)
  target_link_options(stemtex-renderer-gui PRIVATE "/DELAYLOAD:stemtex-renderer.dll")
endif()

add_custom_command(TARGET stemtex-renderer-gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/qt.conf
          $<TARGET_FILE_DIR:stemtex-renderer-gui>/qt.conf
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${QSCINTILLA_DLL}
          $<TARGET_FILE_DIR:stemtex-renderer-gui>
)

add_custom_target(stemtex-renderer-gui-copy-renderer
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:stemtex-renderer-gui>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:stemtex-renderer>
          $<TARGET_FILE_DIR:stemtex-renderer-gui>
  DEPENDS stemtex-renderer
  VERBATIM
)
add_dependencies(stemtex-renderer-gui stemtex-renderer-gui-copy-renderer)

add_executable(stemtex-profile-creator WIN32
  ../profile-creator-qt/main.cpp
  resources.qrc
  stemtex-renderer-gui.rc
)
qt_add_translations(stemtex-profile-creator
  LUPDATE_TARGET stemtex-profile-creator_lupdate
  LRELEASE_TARGET stemtex-profile-creator_lrelease
  LRELEASE_OPTIONS -fail-on-unfinished
  TS_FILES ../profile-creator-qt/i18n/stemtex-profile-creator_zh_CN.ts
  SOURCES ../profile-creator-qt/main.cpp ../profile-creator-qt/i18n.h
          ../profile-creator-qt/catalog-translations.h
  RESOURCE_PREFIX "/i18n"
)
# Embed Qt's standard button/dialog translations too, so installed builds do
# not depend on the developer's Qt installation or loose .qm files.
get_target_property(qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
execute_process(COMMAND "${qt_qmake_executable}" -query QT_INSTALL_TRANSLATIONS
  OUTPUT_VARIABLE qt_translations_dir OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
if(NOT EXISTS "${qt_translations_dir}/qtbase_zh_CN.qm")
  message(FATAL_ERROR "Qt Simplified Chinese translations are required: ${qt_translations_dir}/qtbase_zh_CN.qm")
endif()
qt_add_resources(stemtex-profile-creator "profile_creator_qt_translations"
  PREFIX "/i18n"
  BASE "${qt_translations_dir}"
  FILES "${qt_translations_dir}/qtbase_zh_CN.qm"
)
target_compile_features(stemtex-profile-creator PRIVATE cxx_std_17)
target_link_libraries(stemtex-profile-creator PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets stemtex-profile
)
target_include_directories(stemtex-profile-creator PRIVATE
  ../profile-creator
)
if(MSVC)
  target_compile_options(stemtex-profile-creator PRIVATE /utf-8)
  target_link_libraries(stemtex-profile-creator PRIVATE delayimp)
  target_link_options(stemtex-profile-creator PRIVATE
    "/DELAYLOAD:stemtex-profile.dll"
  )
endif()

add_custom_command(TARGET stemtex-profile-creator POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/qt.conf
          $<TARGET_FILE_DIR:stemtex-profile-creator>/qt.conf
)

# Refresh the SDK even when only the DLL changes and the GUI needs no relink.
add_custom_target(stemtex-profile-creator-copy-profile
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:stemtex-profile-creator>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:stemtex-profile>
          $<TARGET_FILE_DIR:stemtex-profile-creator>
  DEPENDS stemtex-profile
  VERBATIM
)
add_dependencies(stemtex-profile-creator stemtex-profile-creator-copy-profile)

add_custom_target(stemtex-profile-creator-i18n-smoke
  COMMAND $<TARGET_FILE:stemtex-profile-creator> --smoke-i18n --language en
  COMMAND $<TARGET_FILE:stemtex-profile-creator> --smoke-i18n --language zh_CN
  COMMAND $<TARGET_FILE:stemtex-profile-creator> --smoke-i18n --language system
  DEPENDS stemtex-profile-creator
  COMMENT "Checking embedded Profile Creator translations"
  VERBATIM
)

# Both GUI executables share one output directory and one deployed Qt runtime.
add_dependencies(stemtex-profile-creator stemtex-renderer-gui stemtex-profile)

if(TARGET Qt6::windeployqt)
  add_custom_command(TARGET stemtex-renderer-gui POST_BUILD
    COMMAND $<TARGET_FILE:Qt6::windeployqt>
            --no-translations
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-compiler-runtime
            --no-opengl-sw
            --skip-plugin-types generic,iconengines,imageformats,networkinformation,styles,tls
            $<TARGET_FILE:stemtex-renderer-gui>
    COMMAND ${CMAKE_COMMAND} -E rm -f
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/dxcompiler.dll
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/dxil.dll
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/vc_redist.x64.exe
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/generic
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/iconengines
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/imageformats
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/networkinformation
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/styles
            $<TARGET_FILE_DIR:stemtex-renderer-gui>/tls
    COMMENT "Deploying Qt runtime for stemtex-renderer-gui"
  )
endif()
