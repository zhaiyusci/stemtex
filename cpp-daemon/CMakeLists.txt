cmake_minimum_required(VERSION 3.20)
project(stemtex_daemon LANGUAGES CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" STEMTEX_VERSION)
if(PROJECT_IS_TOP_LEVEL)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
endif()
string(STRIP "${STEMTEX_VERSION}" STEMTEX_VERSION)
if(NOT STEMTEX_VERSION)
  message(FATAL_ERROR "VERSION is empty")
endif()

add_library(stemtex-renderer SHARED stemtex_renderer.cpp stemtex_renderer.h)
target_compile_features(stemtex-renderer PRIVATE cxx_std_17)
target_compile_definitions(stemtex-renderer PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  STEMTEX_RENDERER_EXPORTS
  STEMTEX_RENDERER_VERSION="${STEMTEX_VERSION}"
)

add_executable(stemtex-renderer-smoke stemtex_renderer_smoke.cpp)
target_compile_features(stemtex-renderer-smoke PRIVATE cxx_std_17)
target_link_libraries(stemtex-renderer-smoke PRIVATE stemtex-renderer)
