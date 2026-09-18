#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/tensor.hpp"

namespace tilt::rt {

// Camada na ordem do `modelo` (densa + ativacoes/softmax/norma/dropout).
// Espelha Interpreter::Layer sem acoplar o runtime ao interpretador.
struct OnnxLayer {
  enum Kind {
    Dense,
    Recorrente,
    Activation,
    Softmax,
    Dropout,
    LayerNorm,
    Conv2d,
    NormaLote,
    Flatten,
    MaxPool
  } kind = Dense;
  Tensor w;  // valido quando kind == Dense/Recorrente
  Tensor b;
  Tensor u;                     // peso recorrente [oculta, portas*oculta]
  std::string recorrente_tipo;  // valido quando kind == Dense ([saida])
  std::string act;              // valido quando kind == Activation (relu|gelu|silu|sigmoide|tanh)
  std::int64_t passo = 1;       // valido quando kind == Conv2d/MaxPool
  std::int64_t padding = 0;     // valido quando kind == Conv2d
  std::int64_t dilatacao = 1;  // valido quando kind == Conv2d
  std::int64_t janela = 0;     // valido quando kind == MaxPool
  std::int64_t plano = 0;      // valido quando kind == Flatten (largura apos achatar)
  Tensor media_running;        // valido quando kind == NormaLote
  Tensor var_running;          // valido quando kind == NormaLote
};

// Serializa o modelo como ONNX (protobuf binario, opset 20) e devolve os
// bytes. Lanca std::runtime_error se o modelo nao for exportavel
// (sem densa, ativacao desconhecida).
// forma_entrada: forma sem lote (de `entrada: tensor[...]`).
std::string onnx_export_bytes(const std::vector<OnnxLayer>& layers,
                              const std::vector<std::int64_t>& forma_entrada,
                              const std::string& model_name);

// Grava o ONNX em `path`. Em erro devolve false e preenche `err`.
bool onnx_salvar(const std::string& path, const std::vector<OnnxLayer>& layers,
                 const std::vector<std::int64_t>& forma_entrada, const std::string& model_name,
                 std::string& err);

}  // namespace tilt::rt
