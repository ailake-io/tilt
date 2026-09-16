#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tilt::rt {

// Exportacao GGUF (formato do llama.cpp): tensor F32 com forma row-major
// (serializada invertida, por convenção do GGUF) + metadados minimos.
// Sem dependencias externas; so escrita.
struct GgufTensor {
  std::string nome;
  std::vector<std::int64_t> forma;  // row-major, como em Tensor
  std::vector<float> dados;
};

// Grava `path` no formato GGUF v3. Em erro devolve false e preenche `err`.
bool gguf_salvar(const std::string& path, const std::vector<GgufTensor>& tensores,
                 const std::string& nome_modelo, std::string& err);

}  // namespace tilt::rt
