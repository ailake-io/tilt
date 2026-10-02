// Chamada de funcoes Python a partir da Tilt (`chamar_python`).
//
// Cada chamada roda `python3` em um subprocesso: argumentos e resultado viajam
// como JSON por arquivos temporarios (nunca pelo argv nem por uma shell), e o
// stdout do modulo Python vai para o stderr para nao misturar com a saida do
// Tilt. Erros do Python (com a linha do traceback) viram std::runtime_error.
#pragma once

#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Importa `modulo` (nome importavel como `math` ou caminho de um arquivo `.py`) e
// chama `funcao` (aceita pontos: `linalg.norm`) com os argumentos posicionais e
// nomeados. `python` vazio usa $TILT_PYTHON e depois python3 (python no Windows).
// Uma lista de objetos no resultado vira `tabela`.
Value chamar_python(const std::string& modulo, const std::string& funcao,
                    const std::vector<Value>& args, const ValueMap& nomeados,
                    const std::string& python = "");

}  // namespace tilt::rt
