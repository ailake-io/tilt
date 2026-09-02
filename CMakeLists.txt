cmake_minimum_required(VERSION 3.20)
project(tilt VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(TILT_WERROR "Treat warnings as errors" OFF)
if(TILT_WERROR)
  add_compile_options(-Werror)
endif()

option(TILT_SANITIZE "Enable AddressSanitizer + UBSan" OFF)
if(TILT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/tilt/version.hpp @ONLY)

add_library(tilt_core STATIC
  src/common/source.cpp
  src/diagnostics/codes.cpp
  src/diagnostics/diagnostic.cpp
  src/lexer/token.cpp
  src/lexer/lexer.cpp
  src/parser/parser.cpp
  src/parser/ast_dump.cpp
  src/semantic/type.cpp
  src/semantic/checker.cpp
  src/runtime/value.cpp
  src/interp/interpreter.cpp
  src/cli/cli.cpp)
target_include_directories(tilt_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_executable(tilt src/main.cpp)
target_link_libraries(tilt PRIVATE tilt_core)
set_target_properties(tilt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()
add_subdirectory(tests)
