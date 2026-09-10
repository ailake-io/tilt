#pragma once

#include "codegen/codegen_x86_64.hpp"  // reutiliza Result e runtime_source()

namespace tilt::codegen::arm64 {

// Emite assembly AArch64 (GNU as, ELF) para o mesmo subconjunto da VM
// descrito em codegen_x86_64.hpp: `funcao`s e `pipeline`s com literais
// (inteiro/decimal/texto/logico/nulo/lista), aritmetica/comparacao/logica,
// indice, `para cada`, `imprimir`/`tamanho` e chamadas entre funcoes.
// Convencoes AAPCS: argumentos x0-x7, retorno x0, callee-saved x19-x28,
// sp 16-alinhado em todo `bl`. O ponto de entrada espelha o interpretador.
// Qualquer coisa fora do subconjunto -> Result{ok=false}.
Result emit_program(const ast::Program& program);

}  // namespace tilt::codegen::arm64
