cmake_minimum_required(VERSION 3.20)
project(tilt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(TILT_WERROR "Treat warnings as errors" OFF)
if(TILT_WERROR)
  if(MSVC)
    add_compile_options(/WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()

option(TILT_SANITIZE "Enable AddressSanitizer + UBSan" OFF)
if(TILT_SANITIZE AND NOT MSVC)
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
  src/runtime/compat.cpp
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
  src/codegen/codegen_arm64.cpp
  src/lsp/completion.cpp
  src/lsp/lsp_server.cpp
  src/interp/interpreter.cpp
  src/cli/cli.cpp)
target_include_directories(tilt_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(tilt_core PUBLIC ${CMAKE_DL_LIBS})
if(WIN32)
  # Camada de compat: sockets (Winsock2) no runtime kafka/mongo/redis/http.
  target_link_libraries(tilt_core PUBLIC ws2_32)
endif()

add_executable(tilt src/main.cpp)
target_link_libraries(tilt PRIVATE tilt_core)
set_target_properties(tilt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# stdlib ao lado do binario no build tree: o interpretador resolve modulos em
# <exe>/../share/tilt/stdlib, mesmo layout da instalacao (install/CPack).
add_custom_command(TARGET tilt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/stdlib
          ${CMAKE_BINARY_DIR}/share/tilt/stdlib)

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
install(DIRECTORY stdlib/ DESTINATION ${CMAKE_INSTALL_DATADIR}/tilt/stdlib)
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

# Geradores de pacote por plataforma. TGZ em todas; DEB+RPM no Linux;
# DragNDrop (.dmg) no macOS; WIX (.msi) + ZIP no Windows (port fase 30).
if(APPLE)
  set(CPACK_GENERATOR "TGZ;DragNDrop")
elseif(WIN32)
  set(CPACK_GENERATOR "ZIP;WIX")
  # GUID fixo: identidade do produto para upgrades do .msi entre versoes.
  # NAO trocar entre releases — trocar quebra o upgrade path do instalador.
  set(CPACK_WIX_UPGRADE_GUID "8F4C1A2B-6D3E-4A5F-9B7C-2E1D0A8F6C43")
  set(CPACK_WIX_PROGRAM_MENU_FOLDER "Tilt")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "Tilt")
  set(CPACK_WIX_LICENSE_RTF ${CMAKE_CURRENT_SOURCE_DIR}/packaging/wix/license.rtf)
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
