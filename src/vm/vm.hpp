#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <unordered_map>
#include <vector>

#include "runtime/value.hpp"
#include "vm/bytecode.hpp"

namespace tilt::vm {

// Executes a Chunk. Function calls are delegated to `call_hook`, which sets
// *handled = false when it does not know the name.
class Vm {
 public:
  using CallHook =
      std::function<rt::Value(const std::string&, std::vector<rt::Value>&, bool* handled)>;

  // Resolve o nome de uma funcao de usuario para o Chunk ja compilado dela (nullptr
  // = nao esta no subconjunto da VM: a chamada vai pelo `call_hook`). O ponteiro
  // precisa viver mais que o Vm.
  using ChunkResolver = std::function<const Chunk*(const std::string&)>;

  Vm(std::ostream& out, CallHook call_hook, ChunkResolver resolver = {})
      : out_(out), call_(std::move(call_hook)), resolver_(std::move(resolver)) {}

  rt::Value run(const Chunk& chunk, std::vector<rt::Value> args);

 private:
  // Quadro = [base, base + num_locals) de `pilha_` (locals) seguido da pilha de
  // operandos. Chamada VM -> VM reaproveita a mesma pilha: os argumentos ja empilhados
  // viram os locals do chamado, sem alocar vetores por chamada.
  rt::Value executar(const Chunk& chunk, std::size_t base);
  const Chunk* alvo_da_chamada(const Chunk& chunk, std::size_t idx_nome);

  std::ostream& out_;
  CallHook call_;
  ChunkResolver resolver_;
  std::vector<rt::Value> pilha_;
  std::int64_t orcamento_ = 0;
  int profundidade_ = 0;
  // Por chunk, o resultado da resolucao de cada nome de CallFunc (1 = ja resolvido).
  struct Chamadas {
    std::vector<const Chunk*> alvos;
    std::vector<char> resolvido;
  };
  std::unordered_map<const Chunk*, Chamadas> chamadas_;
};

}  // namespace tilt::vm
