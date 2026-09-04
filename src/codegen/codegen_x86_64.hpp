#pragma once

#include <string>

#include "parser/ast.hpp"

namespace tilt::codegen {

struct Result {
  bool ok = false;
  std::string asm_text;  // AT&T x86-64
  std::string error;
};

// Emite assembly x86-64 para o subconjunto da VM: `funcao`s e `pipeline`s
// cujo bytecode usa literais (inteiro/decimal/texto/logico/nulo/lista),
// aritmetica/comparacao/logica, indice, `para cada`, `imprimir`/`tamanho` e
// chamadas entre funcoes do programa. O ponto de entrada espelha o
// interpretador: pipelines (nao vazios) rodam em ordem; senao `funcao
// principal`. Qualquer coisa fora do subconjunto -> Result{ok=false}.
Result emit_program(const ast::Program& program);

// C fonte do runtime linkado junto com o assembly (representacao de valor,
// operacoes e impressao espelhando runtime/value.cpp).
const char* runtime_source();

}  // namespace tilt::codegen
