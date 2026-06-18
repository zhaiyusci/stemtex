cmake_minimum_required(VERSION 3.20)
project(stemtex_renderer_gui LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Pdf PdfWidgets)

add_subdirectory(../cpp-daemon cpp-daemon-build)

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
target_link_libraries(stemtex-renderer-gui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets stemtex-renderer "${QSCINTILLA_LIBRARY}")
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
          $<TARGET_FILE:stemtex-renderer>
          $<TARGET_FILE_DIR:stemtex-renderer-gui>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${QSCINTILLA_DLL}
          $<TARGET_FILE_DIR:stemtex-renderer-gui>
)

if(TARGET Qt6::windeployqt)
  add_custom_command(TARGET stemtex-renderer-gui POST_BUILD
    COMMAND $<TARGET_FILE:Qt6::windeployqt>
            --no-translations
            --no-system-d3d-compiler
            --no-opengl-sw
            $<TARGET_FILE:stemtex-renderer-gui>
    COMMENT "Deploying Qt runtime for stemtex-renderer-gui"
  )
endif()
