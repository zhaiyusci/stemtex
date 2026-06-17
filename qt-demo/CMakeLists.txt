cmake_minimum_required(VERSION 3.20)
project(stemtex_qt_demo LANGUAGES CXX)

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

add_executable(stemtex-qt-demo WIN32 main.cpp)
target_compile_features(stemtex-qt-demo PRIVATE cxx_std_17)
target_link_libraries(stemtex-qt-demo PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets stemtex-renderer "${QSCINTILLA_LIBRARY}")
target_include_directories(stemtex-qt-demo PRIVATE ../cpp-daemon "${QSCINTILLA_INCLUDE_DIR}")

add_custom_command(TARGET stemtex-qt-demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:stemtex-renderer>
          $<TARGET_FILE_DIR:stemtex-qt-demo>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/qt.conf
          $<TARGET_FILE_DIR:stemtex-qt-demo>/qt.conf
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${QSCINTILLA_DLL}
          $<TARGET_FILE_DIR:stemtex-qt-demo>
)

if(TARGET Qt6::windeployqt)
  add_custom_command(TARGET stemtex-qt-demo POST_BUILD
    COMMAND $<TARGET_FILE:Qt6::windeployqt>
            --no-translations
            --no-system-d3d-compiler
            --no-opengl-sw
            $<TARGET_FILE:stemtex-qt-demo>
    COMMENT "Deploying Qt runtime for stemtex-qt-demo"
  )
endif()
