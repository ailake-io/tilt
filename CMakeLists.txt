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
  src/runtime/json.cpp
  src/runtime/tensor.cpp
  src/runtime/llm.cpp
  src/runtime/vectorstore.cpp
  src/runtime/http_server.cpp
  src/interp/interpreter.cpp
  src/cli/cli.cpp)
target_include_directories(tilt_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_executable(tilt src/main.cpp)
target_link_libraries(tilt PRIVATE tilt_core)
set_target_properties(tilt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(TILT_STATIC_LIBSTDCXX "Link libstdc++/libgcc statically (portable Linux binary)" ON)
if(TILT_STATIC_LIBSTDCXX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(tilt PRIVATE -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------- install / package

include(GNUInstallDirs)
install(TARGETS tilt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY exemplos/ DESTINATION ${CMAKE_INSTALL_DATADIR}/tilt/exemplos)
install(FILES README.md LICENSE CLAUDE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/tilt)

set(CPACK_PACKAGE_NAME tilt)
set(CPACK_PACKAGE_VENDOR "ailake-io")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "ailake-io")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Linguagem Tilt: compilador/runtime C++20 para ML, DL, engenharia de dados, LLMs e agentes")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR "TGZ")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, curl")
if(CMAKE_SYSTEM_PROCESSOR)
  set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
include(CPack)
