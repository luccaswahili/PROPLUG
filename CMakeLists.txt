cmake_minimum_required(VERSION 3.16)
# Premiere Pro CC 2015 (9.0) runs on OS X 10.9+, so the plug-in must load there too.
# Without this the binary is stamped with the build machine's macOS version.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.9" CACHE STRING "Minimum macOS version")
project(LookMatch C CXX)

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
  src/core/engine.cpp
  src/core/scene.cpp)
target_include_directories(lookmatch_core PUBLIC src/core)
target_link_libraries(lookmatch_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(lookmatch_core PRIVATE /W4 /fp:fast)
else()
  target_compile_options(lookmatch_core PRIVATE -Wall -Wextra -O3)
endif()
# Release hardening: hide the engine's symbols so they don't export from the plugin that links this in
# (the plugin's own CMake dead-strips and strips too). Harmless to the CLI/test executables.
if(APPLE)
  target_compile_options(lookmatch_core PRIVATE
    $<$<CONFIG:Release>:-fvisibility=hidden>
    $<$<CONFIG:Release>:-fvisibility-inlines-hidden>)
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

# Source-aware match consistency: real frames from tests/frames (tools/extract_frames.swift) matched to
# LOOKMATCH_TEST_REF under exposure / white balance / curve / log perturbations. Synthetic without them.
set(LOOKMATCH_TEST_REF "$ENV{HOME}/Desktop/Screenshot 2026-10-29 at 15.38.33.png" CACHE FILEPATH "Reference still for consistency_tests")
add_executable(consistency_tests tests/consistency_tests.cpp)
target_include_directories(consistency_tests PRIVATE third_party)
target_compile_definitions(consistency_tests PRIVATE LOOKMATCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                                     LOOKMATCH_DEFAULT_REF="${LOOKMATCH_TEST_REF}")
target_link_libraries(consistency_tests PRIVATE lookmatch_core)
add_test(NAME consistency_tests COMMAND consistency_tests)
set_tests_properties(consistency_tests PROPERTIES TIMEOUT 1800)
add_executable(banding_tests tests/banding_tests.cpp)
target_include_directories(banding_tests PRIVATE third_party)
target_compile_definitions(banding_tests PRIVATE LOOKMATCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                                 LOOKMATCH_TEST_REF="${LOOKMATCH_TEST_REF}")
target_link_libraries(banding_tests PRIVATE lookmatch_core)
add_test(NAME banding_tests COMMAND banding_tests)
add_executable(reference_tests tests/reference_tests.cpp)
target_include_directories(reference_tests PRIVATE third_party)
target_compile_definitions(reference_tests PRIVATE LOOKMATCH_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(reference_tests PRIVATE lookmatch_core)
add_test(NAME reference_tests COMMAND reference_tests)
add_executable(slot_source_tests tests/slot_source_tests.cpp)
target_link_libraries(slot_source_tests PRIVATE lookmatch_core)
add_test(NAME slot_source_tests COMMAND slot_source_tests)
add_executable(adjust_tests tests/adjust_tests.cpp)
target_include_directories(adjust_tests PRIVATE src/core)
add_test(NAME adjust_tests COMMAND adjust_tests)

add_executable(license_tests tests/license_tests.cpp third_party/tweetnacl/tweetnacl.c)
target_include_directories(license_tests PRIVATE src/premiere third_party/tweetnacl)
add_test(NAME license_tests COMMAND license_tests)

# GPU kernel (Premiere GPU render) vs CPU engine, and a timing benchmark. macOS / Metal only.
if(APPLE)
  enable_language(OBJCXX)
  foreach(t gpu_tests gpu_bench)
    add_executable(${t} tests/${t}.mm)
    target_link_libraries(${t} PRIVATE lookmatch_core "-framework Metal" "-framework Foundation")
    target_include_directories(${t} PRIVATE src/core)
    set_target_properties(${t} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  endforeach()
  add_test(NAME gpu_tests COMMAND gpu_tests)
endif()
