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

// Divide `texto` em pedacos de ate `tamanho` bytes (nunca no meio de um
// caractere UTF-8), com `sobreposicao` bytes repetidos entre pedacos vizinhos.
// modo: "tamanho" (janela fixa), "sentenca" (junta sentencas inteiras, termina
// em . ! ? ou linha em branco), "paragrafo" (blocos separados por linha em
// branco) ou "linha" (linhas inteiras — bom para codigo). Nos modos por unidade,
// a unidade maior que `tamanho` cai na janela fixa. Lanca std::runtime_error
// para modo desconhecido.
std::vector<std::string> dividir_texto_em_pedacos(const std::string& texto, std::size_t tamanho,
                                                  std::size_t sobreposicao,
                                                  const std::string& modo);

// true se `nome` e uma funcao da biblioteca padrao (ex.: "raiz", "substituir").
bool stdlib_existe(const std::string& nome);

// Chama a funcao `nome`. Requer stdlib_existe(nome). Lanca std::runtime_error.
Value stdlib_chamar(const std::string& nome, const std::vector<Value>& args);

}  // namespace tilt::rt
