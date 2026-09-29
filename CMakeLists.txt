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

option(TILT_COVERAGE "Instrument for gcov/gcovr line coverage (GCC/Clang)" OFF)
if(TILT_COVERAGE AND NOT MSVC)
  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endif()

option(TILT_SANITIZE "Enable AddressSanitizer + UBSan" OFF)
if(TILT_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Build optimizations are opt-in: default binaries remain portable and do not
# require a training profile. GCC profile files are tied to object paths, so
# GENERATE and USE must reconfigure the same build tree.
option(TILT_LTO "Enable link-time optimization in Release builds" OFF)
set(TILT_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GENERATE or USE")
set_property(CACHE TILT_PGO PROPERTY STRINGS OFF GENERATE USE)
set(TILT_PGO_PROFILE_DIR "" CACHE PATH "Directory shared by PGO training and use builds")
if(TILT_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT tilt_ipo_ok OUTPUT tilt_ipo_error)
  if(NOT tilt_ipo_ok)
    message(FATAL_ERROR "TILT_LTO is unavailable: ${tilt_ipo_error}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
if(NOT TILT_PGO STREQUAL "OFF")
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "TILT_PGO currently supports GCC only")
  endif()
  if(NOT TILT_PGO STREQUAL "GENERATE" AND NOT TILT_PGO STREQUAL "USE")
    message(FATAL_ERROR "TILT_PGO must be OFF, GENERATE or USE")
  endif()
  if(TILT_PGO_PROFILE_DIR STREQUAL "")
    message(FATAL_ERROR "TILT_PGO_PROFILE_DIR must point to a shared profile directory")
  endif()
  if(TILT_PGO STREQUAL "GENERATE")
    add_compile_options(-fprofile-generate=${TILT_PGO_PROFILE_DIR})
    add_link_options(-fprofile-generate=${TILT_PGO_PROFILE_DIR})
  else()
    # Some optional connector translation units may not run during training.
    add_compile_options(-fprofile-use=${TILT_PGO_PROFILE_DIR} -fprofile-correction
                        -Wno-error=missing-profile)
    add_link_options(-fprofile-use=${TILT_PGO_PROFILE_DIR})
  endif()
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/tilt/version.hpp @ONLY)

# Metal é opcional e só entra no binário macOS. Nas outras plataformas o stub
# mantém o runtime compilável sem SDK ou frameworks Apple.
if(APPLE)
  enable_language(OBJCXX)
  set(TILT_METAL_SOURCE src/runtime/metal_runtime.mm)
else()
  set(TILT_METAL_SOURCE src/runtime/metal_runtime.cpp)
endif()

add_library(tilt_core STATIC
  src/common/source.cpp
  src/diagnostics/codes.cpp
  src/diagnostics/diagnostic.cpp
  src/lexer/token.cpp
  src/lexer/aliases_en.cpp
  src/lexer/lexer.cpp
  src/parser/parser.cpp
  src/parser/ast_dump.cpp
  src/semantic/type.cpp
  src/semantic/checker.cpp
  src/runtime/compat.cpp
  src/runtime/value.cpp
  src/runtime/columnar.cpp
  src/runtime/json.cpp
  src/runtime/python_interop.cpp
  src/runtime/sql_tabelas.cpp
  src/runtime/tabela_ops.cpp
  src/runtime/fuso.cpp
  src/runtime/tensor.cpp
  src/runtime/safetensors.cpp
  src/runtime/onnx.cpp
  src/runtime/gguf.cpp
  src/runtime/llm.cpp
  src/runtime/vectorstore.cpp
  src/runtime/http_server.cpp
  src/runtime/http_client.cpp
  src/runtime/gpu_runtime.cpp
  ${TILT_METAL_SOURCE}
  src/runtime/parquet.cpp
  src/runtime/parquet_key_provider.cpp
  # Avro OCF/Confluent vive no mesmo TU do Iceberg para reutilizar o codec
  # binario e os codecs de bloco sem duplicar a implementacao.
  src/runtime/delta.cpp
  src/runtime/iceberg.cpp
  src/runtime/iceberg_catalog_server.cpp
  src/runtime/qdrant.cpp
  src/runtime/redis.cpp
  src/runtime/kafka.cpp
  src/runtime/leader.cpp
  src/runtime/cluster.cpp
  src/runtime/checkpoint.cpp
  src/runtime/mongo.cpp
  src/runtime/tls.cpp
  src/runtime/aws_sigv4.cpp
  src/runtime/aws_kms.cpp
  src/runtime/s3.cpp
  src/runtime/sha256.cpp
  src/runtime/stdlib.cpp
  src/runtime/sqlite.cpp
  src/runtime/sql_pool.cpp
  src/runtime/postgres.cpp
  src/runtime/duckdb.cpp
  src/runtime/mysql.cpp
  src/runtime/clickhouse.cpp
  src/runtime/elasticsearch.cpp
  src/runtime/livy.cpp
  src/runtime/pgvector.cpp
  src/runtime/weaviate.cpp
  src/runtime/pinecone.cpp
  src/runtime/chroma.cpp
  src/vm/compiler.cpp
  src/vm/vm.cpp
  src/vm/jit.cpp
  src/vm/bytecode_cache.cpp
  src/codegen/codegen_x86_64.cpp
  src/codegen/codegen_arm64.cpp
  src/lsp/completion.cpp
  src/lsp/workspace_index.cpp
  src/lsp/lsp_server.cpp
  src/interp/interpreter.cpp
  src/cli/cli.cpp
  src/cli/dev_cmds.cpp
  src/cli/rpc.cpp)
target_include_directories(tilt_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(tilt_core PUBLIC ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(tilt_core PUBLIC "-framework Metal" "-framework Foundation")
endif()
# O cliente HTTP nativo usa o header de libcurl quando disponivel, mas carrega
# a biblioteca em runtime; instalacoes sem libcurl continuam com o curl CLI.
find_path(TILT_CURL_INCLUDE_DIR curl/curl.h HINTS "$ENV{CONDA_PREFIX}/include")
if(TILT_CURL_INCLUDE_DIR)
  target_include_directories(tilt_core PRIVATE ${TILT_CURL_INCLUDE_DIR})
  target_compile_definitions(tilt_core PRIVATE TILT_HAS_CURL_HEADER=1)
endif()
if(WIN32)
  # Camada de compat: sockets (Winsock2) no runtime kafka/mongo/redis/http.
  target_link_libraries(tilt_core PUBLIC ws2_32)
endif()

add_executable(tilt src/main.cpp)
target_link_libraries(tilt PRIVATE tilt_core)
# Windows: a pilha padrao do executavel e 1 MB (Linux/macOS: 8 MB); o parser e o
# checker recursam na estrutura do programa e o MSVC usa frames grandes. 16 MB
# da a mesma folga das outras plataformas (os limites de profundidade do parser
# continuam valendo como protecao).
if(MSVC)
  target_link_options(tilt PRIVATE /STACK:16777216)
elseif(WIN32)
  target_link_options(tilt PRIVATE -Wl,--stack,16777216)
endif()
set_target_properties(tilt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# stdlib ao lado do binario no build tree: copie tambem em builds incrementais
# quando apenas um modulo .tilt mudar.
add_custom_target(tilt_stdlib ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/stdlib
          ${CMAKE_BINARY_DIR}/share/tilt/stdlib)
add_dependencies(tilt tilt_stdlib)

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
if(NOT WIN32)
  install(PROGRAMS scripts/update.sh DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME tilt-atualizar)
endif()

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
