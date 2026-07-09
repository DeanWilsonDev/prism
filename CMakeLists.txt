cmake_minimum_required(VERSION 3.20)
project(
  prism
  VERSION 0.1.0
  LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LLVM_DIR
    /usr/lib/cmake/llvm
    CACHE PATH "Path to LLVM cmake config")
set(Clang_DIR
    /usr/lib/cmake/clang
    CACHE PATH "Path to Clang cmake config")

include(FetchContent)

# Dependencies

# Firefly

FetchContent_Declare(
  firefly
  GIT_REPOSITORY https://github.com/DeanWilsonDev/firefly.git
  GIT_TAG main)

FetchContent_MakeAvailable(firefly)

# Cimmerian

FetchContent_Declare(
  cimmerian
  GIT_REPOSITORY https://github.com/DeanWilsonDev/cimmerian.git
  GIT_TAG main)

FetchContent_MakeAvailable(cimmerian)

# Amanuensis (first-party JSON serialiser)
#
# Firefly already vendors Amanuensis via add_subdirectory and defines the
# `amanuensis` target when it is made available above. Declaring our own copy
# and calling FetchContent_MakeAvailable unconditionally would try to define the
# `amanuensis` target a second time and fail. Guard on the target so we reuse
# Firefly's copy when present and fetch from the canonical URL otherwise.

FetchContent_Declare(
  amanuensis
  GIT_REPOSITORY https://github.com/DeanWilsonDev/amanuensis.git
  GIT_TAG main)

if(NOT TARGET amanuensis)
  FetchContent_MakeAvailable(amanuensis)
endif()

find_package(Clang REQUIRED CONFIG)

# Core library: the analysis pipeline (parser, graph builder, metrics, exporter)
# shared by both the CLI executable and the test binary so tests exercise the
# real implementation rather than re-declaring it.
add_library(
  ${PROJECT_NAME}-core STATIC
  src/parser.cpp
  src/dependency-graph.cpp
  src/graph-builder.cpp
  src/metrics-engine.cpp
  src/exporter.cpp
  include/prism/logging/log.cpp)

target_precompile_headers(${PROJECT_NAME}-core PRIVATE include/pch.hpp)

target_include_directories(${PROJECT_NAME}-core PUBLIC include)
target_include_directories(${PROJECT_NAME}-core PRIVATE ${CLANG_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME}-core PUBLIC libclang firefly amanuensis)

add_executable(${PROJECT_NAME} src/main.cpp src/example/test-input.cpp)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/pch.hpp)

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}-core)

add_executable(
  ${PROJECT_NAME}-tests
  test/test-main.cpp test/exporter.test.cpp test/graph-builder.test.cpp
  test/parser.test.cpp test/metrics-engine.test.cpp
  test/header-extraction.test.cpp)

target_include_directories(${PROJECT_NAME}-tests PRIVATE include)

target_compile_definitions(
  ${PROJECT_NAME}-tests
  PRIVATE PRISM_TEST_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/example")

target_link_libraries(${PROJECT_NAME}-tests PRIVATE cimmerian ${PROJECT_NAME}-core)
