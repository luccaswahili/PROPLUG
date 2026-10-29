cmake_minimum_required(VERSION 3.16)
project(LookMatch CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# Portable render engine: no dependencies, shared by the CLI and the Premiere effect.
add_library(lookmatch_core STATIC
  src/core/lut3d.cpp
  src/core/matcher.cpp
  src/core/film.cpp
  src/core/engine.cpp)
target_include_directories(lookmatch_core PUBLIC src/core)
target_link_libraries(lookmatch_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(lookmatch_core PRIVATE /W4 /fp:fast)
else()
  target_compile_options(lookmatch_core PRIVATE -Wall -Wextra -O3)
endif()

add_executable(lookmatch-render src/cli/main.cpp)
target_include_directories(lookmatch-render PRIVATE third_party)
target_link_libraries(lookmatch-render PRIVATE lookmatch_core)

# Premiere Pro / After Effects effect. Needs the Adobe After Effects SDK (free download from
# Adobe's developer site), pointed to with -DAE_SDK_DIR=/path/to/AfterEffectsSDK/Examples
option(LOOKMATCH_BUILD_PLUGIN "Build the Premiere Pro effect" OFF)
if(LOOKMATCH_BUILD_PLUGIN)
  add_subdirectory(src/premiere)
endif()

enable_testing()
add_executable(engine_tests tests/engine_tests.cpp)
target_link_libraries(engine_tests PRIVATE lookmatch_core)
add_test(NAME engine_tests COMMAND engine_tests)
