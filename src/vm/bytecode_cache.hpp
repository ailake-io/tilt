#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "vm/bytecode.hpp"

namespace tilt::vm {

// Cache de bytecode em disco (`.tiltc`, Fase 6): guarda Chunks compilados
// para nao recompilar a cada `executar`. Chaveado pelo SHA-256 do fonte —
// fonte diferente = cache ignorado. Fail-closed: qualquer byte fora do
// esperado (magia, versao, truncamento, const nao serializavel) invalida
// a entrada (ou o arquivo todo), nunca executa lixo.
//
// Formato (little-endian): "TILTC2" + u32 versao(=1) + sha256(32B do fonte)
// + u32 nentries + entries. Entry: u8 kind (0=pipeline, 1=funcao) + str nome
// + i32 nparams + i32 num_locals + code[] (u8 op, i32 a, i32 b) + consts[]
// (u8 tag + payload) + op_names[] + names[]. str = u32 len + bytes.
// Consts: 0=nulo, 1=logico(u8), 2=inteiro(i64), 3=decimal(bits u64),
// 4=texto(str). Lista/mapa/tabela/tensor nao serializam (chunk pulado).
inline constexpr std::uint32_t kTiltcVersion = 1;

struct CachedChunk {
  bool is_pipeline = false;
  int nparams = 0;
  Chunk chunk;
};

// Chave: "pipeline <nome>" / "funcao <nome>".
struct CachedProgram {
  std::array<std::uint8_t, 32> source_sha{};
  std::unordered_map<std::string, CachedChunk> entries;
};

std::array<std::uint8_t, 32> tiltc_sha(const std::string& bytes);

// "<fonte>.tilt" -> "<fonte>.tiltc" (irmao do fonte).
std::string tiltc_path_for(const std::string& source_path);

// Grava (melhor esforco: false em qualquer falha de IO). Chunks com const
// nao serializavel sao pulados.
bool tiltc_save(const std::string& path, const std::string& source_bytes,
                const CachedProgram& prog);

// Carrega validando magia/versao/sha. false = ausente/corrompido/de outra
// versao ou fonte (chamar recompila; nunca lanca).
bool tiltc_load(const std::string& path, const std::string& source_bytes, CachedProgram& out);

}  // namespace tilt::vm
