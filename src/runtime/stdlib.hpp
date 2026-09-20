// Funcoes puras da biblioteca padrao da Tilt: matematica, texto, conversao de
// tipos, listas/mapas, datas, arquivos de texto, hash/base64 e JSON.
//
// Nao dependem do interpretador: recebem Values ja avaliados e devolvem um
// Value. Erros de uso (aridade, tipo, dominio) viram std::runtime_error com
// mensagem em portugues; o interpretador os converte em T901 com a linha.
#pragma once

#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// true se `nome` e uma funcao da biblioteca padrao (ex.: "raiz", "substituir").
bool stdlib_existe(const std::string& nome);

// Chama a funcao `nome`. Requer stdlib_existe(nome). Lanca std::runtime_error.
Value stdlib_chamar(const std::string& nome, const std::vector<Value>& args);

}  // namespace tilt::rt
