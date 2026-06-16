cmake_minimum_required(VERSION 3.20)
project(stemtex_daemon LANGUAGES CXX)

add_library(stemtex-renderer SHARED stemtex_renderer.cpp stemtex_renderer.h)
target_compile_features(stemtex-renderer PRIVATE cxx_std_17)
target_compile_definitions(stemtex-renderer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX STEMTEX_RENDERER_EXPORTS)

add_executable(stemtex-renderer-smoke stemtex_renderer_smoke.cpp)
target_compile_features(stemtex-renderer-smoke PRIVATE cxx_std_17)
target_link_libraries(stemtex-renderer-smoke PRIVATE stemtex-renderer)
