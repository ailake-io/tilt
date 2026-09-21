// Operacoes de limpeza e preparacao de dados sobre `tabela` (lista de mapas).
//
// Todas sao puras: recebem tabelas e devolvem uma tabela NOVA (as linhas de entrada
// nao sao alteradas), sem depender do interpretador. Uma celula "nula" e o valor
// nulo, a chave ausente ou um texto vazio/so de espacos (o que `ler_csv` devolve para
// celula vazia). Erros de uso lancam std::runtime_error em portugues.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Celula ausente ou nula (nulo, chave faltando, texto vazio/so espacos).
bool celula_nula(const Value* v);

// Remove as linhas com nulo em alguma das colunas (todas, se `colunas` vazio).
Value tabela_remover_nulos(const Value& t, const std::vector<std::string>& colunas);

// Preenche celulas nulas: `preenchimento` e um mapa { coluna: valor } (cria a coluna
// se faltar) ou um valor unico para todas as colunas existentes.
Value tabela_preencher_nulos(const Value& t, const Value& preenchimento);

// { antigo: "novo" }: mantem a ordem das colunas. Coluna inexistente e erro.
Value tabela_renomear(const Value& t, const Value& mapa);

// Remove colunas por nome (nome inexistente e erro: pega typo).
Value tabela_remover_colunas(const Value& t, const std::vector<std::string>& nomes);

// { coluna: tipo } com tipo = inteiro | decimal | texto | logico | data. Valor que
// nao converte vira nulo (fracao em `inteiro` tambem: arredonde antes).
Value tabela_converter(const Value& t, const Value& tipos);

// Mantem a primeira ocorrencia de cada chave (linha inteira se `colunas` vazio).
Value tabela_deduplicar(const Value& t, const std::vector<std::string>& colunas);

// Junta duas tabelas. `por`: nome, lista de nomes ou mapa { esquerda: direita };
// `tipo`: interna (padrao), esquerda, direita, completa. Colunas repetidas da
// direita ganham o sufixo `_direita`. Chave nula nunca casa (como no SQL).
Value tabela_juntar(const Value& esq, const Value& dir, const Value& por, const std::string& tipo);

// Concatena tabelas; o esquema vira a uniao das colunas (faltantes = nulo).
Value tabela_empilhar(const std::vector<Value>& tabelas);

// Perfil por coluna: coluna, tipo, total, nulos, distintos, minimo, maximo, media.
Value tabela_descrever(const Value& t);

// `n` linhas (n >= 1) ou a fracao `n` (0 < n < 1) de linhas, sem reposicao,
// deterministica para a mesma semente; preserva a ordem original.
Value tabela_amostra(const Value& t, double n, std::uint64_t semente);

// { valor, contagem } por valor distinto da coluna, do mais ao menos frequente.
Value tabela_contar_valores(const Value& t, const std::string& coluna);

// Tira espacos das pontas e comprime os repetidos; `caixa`: "", "minusculas" ou
// "maiusculas". Colunas vazias = todas as colunas de texto.
Value tabela_limpar_texto(const Value& t, const std::vector<std::string>& colunas,
                          const std::string& caixa);

// Ordena por uma ou mais colunas (estavel; numero contra numero, senao texto).
Value tabela_ordenar(const Value& t, const std::vector<std::string>& colunas, bool decrescente);

// Data em varios formatos (2024-01-31, 31/01/2024, 2024/01/31, com hora opcional) para
// ISO `AAAA-MM-DD[THH:MM[:SS]]`; vazio se invalida.
std::string normalizar_data(const std::string& texto);

}  // namespace tilt::rt
