// Palavras reservadas em ingles: `if`, `for each`, `steps`, `print`, `filter`...
//
// A Tilt e bilingue: cada palavra-chave, funcao embutida e metodo tem uma forma em
// portugues (a canonica) e uma em ingles. A reescrita acontece nos tokens, logo
// depois do lexer, entao parser, checker, interpretador, VM e LSP so veem o
// portugues. Palavras dos dois idiomas podem ser misturadas no mesmo arquivo.
//
// A reescrita e conservadora para nao tocar em dados do usuario:
//   - nomes que o proprio arquivo define (variavel, funcao, parametro, ...) nunca
//     sao traduzidos;
//   - depois de `.` so se traduz nome de metodo chamado (com argumentos);
//   - chaves de `{ mapa: ... }` e campos de `type`/`input`/`output`/`data` (nomes
//     do usuario) ficam como foram escritos.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "lexer/token.hpp"

namespace tilt {

// Idioma de um arquivo: `Auto` decide pelo que predomina nas palavras estruturais
// (chaves de bloco, controle de fluxo) no inicio das linhas; um arquivo em
// portugues nunca e alterado. O primeiro comentario `# idioma: en|pt` (ou
// `# language: en|pt`) forca a escolha.
enum class Idioma { Auto, Ingles, Portugues };

// Le a diretiva `# idioma:` nas primeiras linhas do fonte (Auto se nao houver).
Idioma idioma_do_fonte(std::string_view fonte);

// Reescreve, no lugar, o lexema das palavras em ingles para a forma canonica.
void aplicar_aliases_en(std::vector<Token>& tokens, Idioma idioma = Idioma::Auto);

// Forma canonica (portugues) de uma palavra em ingles em qualquer papel, ou "" se
// a palavra nao e um alias. Usado por docs e testes.
std::string_view alias_en_para_pt(std::string_view en);

// Todos os pares (ingles, portugues, papeis) — papeis: C controle, K chave de
// bloco, N nome/funcao embutida, M metodo, Z metodo sem argumentos, A argumento
// nomeado. Para gerar a documentacao.
struct AliasEn {
  const char* en;
  const char* pt;
  const char* papeis;
};
const std::vector<AliasEn>& tabela_aliases_en();

}  // namespace tilt
