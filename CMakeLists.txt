cmake_minimum_required(VERSION 3.30)
project(
  prism
  VERSION 0.1.0
  LANGUAGES CXX C)

if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
   OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19))
  message(FATAL_ERROR "Prism needs GCC 14 or later or Clang 19 or later; found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 26)
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

# nlohmann/json

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0)

FetchContent_MakeAvailable(nlohmann_json)

find_package(Clang REQUIRED CONFIG)

add_executable(
  ${PROJECT_NAME} src/main.cpp src/parser.cpp src/dependency-graph.cpp
                  src/metrics-engine.cpp src/exporter.cpp src/example/test-input.cpp)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/pch.hpp)

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PRIVATE ${CLANG_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME} PRIVATE libclang firefly
                                              nlohmann_json::nlohmann_json)

add_executable(
  ${PROJECT_NAME}-tests test/test-main.cpp test/exporter.test.cpp
                        test/graph-builder.test.cpp test/parser.test.cpp)

target_include_directories(${PROJECT_NAME}-tests PRIVATE include)

target_link_libraries(${PROJECT_NAME}-tests PRIVATE cimmerian)
