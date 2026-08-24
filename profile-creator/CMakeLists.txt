cmake_minimum_required(VERSION 3.20)
project(stemtex_profile_creator_core LANGUAGES CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" STEMTEX_VERSION)
string(STRIP "${STEMTEX_VERSION}" STEMTEX_VERSION)

add_library(stemtex-profile SHARED
  stemtex_profile.cpp
  stemtex_profile.h
)
target_compile_features(stemtex-profile PRIVATE cxx_std_17)
target_compile_definitions(stemtex-profile PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  STEMTEX_PROFILE_EXPORTS
  STEMTEX_PROFILE_VERSION="${STEMTEX_VERSION}"
)

add_executable(stemtex-profile-smoke stemtex_profile_smoke.cpp)
target_compile_features(stemtex-profile-smoke PRIVATE cxx_std_17)
target_link_libraries(stemtex-profile-smoke PRIVATE stemtex-profile stemtex-renderer)
target_include_directories(stemtex-profile-smoke PRIVATE ../cpp-daemon)

if(MSVC)
  target_compile_options(stemtex-profile PRIVATE /EHsc /utf-8)
  target_compile_options(stemtex-profile-smoke PRIVATE /EHsc /utf-8)
endif()
