cmake_minimum_required(VERSION 3.20)
project(tilt VERSION 0.1.0 LANGUAGES CXX)

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
  src/runtime/gpu_runtime.cpp
  src/runtime/parquet.cpp
  src/runtime/delta.cpp
  src/runtime/iceberg.cpp
  src/runtime/qdrant.cpp
  src/runtime/redis.cpp
  src/runtime/kafka.cpp
  src/runtime/mongo.cpp
  src/runtime/tls.cpp
  src/runtime/s3.cpp
  src/runtime/sha256.cpp
  src/runtime/sqlite.cpp
  src/runtime/postgres.cpp
  src/runtime/pgvector.cpp
  src/vm/compiler.cpp
  src/vm/vm.cpp
  src/codegen/codegen_x86_64.cpp
  src/lsp/completion.cpp
  src/lsp/lsp_server.cpp
  src/interp/interpreter.cpp
  src/cli/cli.cpp)
target_include_directories(tilt_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(tilt_core PUBLIC ${CMAKE_DL_LIBS})

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
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/tilt/docs)

set(CPACK_PACKAGE_NAME tilt)
set(CPACK_PACKAGE_VENDOR "ailake-io")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "ailake-io")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Linguagem Tilt: compilador/runtime C++20 para ML, DL, engenharia de dados, LLMs e agentes")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_HOMEPAGE "https://github.com/ailake-io/tilt")

# Geradores de pacote por plataforma. TGZ em todas; DEB no Linux;
# DragNDrop (.dmg) no macOS. Windows (.msi via WIX) fica para quando o
# runtime ganhar camada de portabilidade (epoll/fork/dlopen sao POSIX).
if(APPLE)
  set(CPACK_GENERATOR "TGZ;DragNDrop")
else()
  set(CPACK_GENERATOR "TGZ;DEB;RPM")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, curl")
  set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
  set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE}")
  set(CPACK_RPM_PACKAGE_REQUIRES "glibc, curl")
  set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE}")
endif()
if(CMAKE_SYSTEM_PROCESSOR)
  set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
include(CPack)
