#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "parser/ast.hpp"
#include "semantic/type.hpp"

namespace tilt {

// First semantic pass: builds the global symbol table and runs the checks
// that do not need full type inference — duplicate declarations, unknown type
// names, malformed tensor annotations, hard-coded secrets, invalid devices,
// agent tool references, statement-level name resolution (T030), the
// shape solver (T012: cadeia densa/linear nos `modelo`s e propagacao de
// formas literais conv2d/norma_lote/reformar/transposta/matmul) e a
// inferencia de tipos conservadora (T011: operadores, builtins, metodos,
// retorno de `funcao` por anotacao ou corpo, campos de mapas, indice em
// listas homogeneas e agregacoes) nos corpos de
// `funcao`/`pipeline`/`servico`.
class SemanticChecker {
 public:
  struct Symbol {
    std::string name;
    std::string kind;  // declaring keyword, or "var" / "modulo"
    sema::Type type;
    Span span;
  };

  SemanticChecker(const ast::Program& program, DiagnosticEngine& diag);
  void run();

  // Para o LSP: simbolos globais com tipos resolvidos (apos run()) e
  // anotacoes de tipo por expressao (params/retorno de `funcao`, campos de
  // `entrada:`/`saida:`). Retorna nullptr quando sem anotacao.
  const std::unordered_map<std::string, Symbol>& globals() const { return globals_; }
  const sema::Type* annotation_of(const ast::Expr* e) const {
    auto it = annotation_cache_.find(e);
    return it == annotation_cache_.end() ? nullptr : &it->second;
  }
  // Para o LSP: tipo/forma inferidos de uma expressao (apos run()).
  // nullptr = desconhecido (hover cai no texto atual).
  const sema::TypeKind* hover_type(const ast::Expr* e) const {
    auto it = hover_types_.find(e);
    return it == hover_types_.end() ? nullptr : &it->second;
  }
  const std::vector<std::int64_t>* hover_shape(const ast::Expr* e) const {
    auto it = hover_shapes_.find(e);
    return it == hover_shapes_.end() ? nullptr : &it->second;
  }

 private:

  void collect();
  void resolve_types();
  void infer_funcao_returns();  // C1: retornos anotados + inferidos do corpo
  void audit_blocks();
  void check_bodies();
  void check_model_shapes();

  using Scope = std::unordered_set<std::string>;
  using TensorShape = std::vector<std::int64_t>;
  using ShapeEnv = std::unordered_map<std::string, TensorShape>;
  using TypeEnv = std::unordered_map<std::string, sema::TypeKind>;
  // Formas de mapas/listas por variavel (Marco 2 / C1): campos de mapas
  // literais e elemento de listas homogeneas, para `m.campo`, `l[i]` e
  // agregacoes. So para literais em linha reta (fluxo-insensivel como o
  // TypeEnv); ramos condicionais restauram o estado anterior.
  using MapShapes = std::unordered_map<std::string, std::unordered_map<std::string, sema::TypeKind>>;
  using ListElems = std::unordered_map<std::string, sema::TypeKind>;

  void scan_for_bodies(const ast::Block& block, Scope scope, ShapeEnv shapes, TypeEnv types);
  void walk_stmt_block(const ast::Block& block, Scope scope, ShapeEnv shapes, TypeEnv types);
  void walk_stmt(const ast::Stmt& stmt, Scope& scope, ShapeEnv& shapes, TypeEnv& types);
  void check_expr(const ast::Expr& expr, const Scope& scope);

  // Shape solver: infere a forma de uma expressao tensora a partir de
  // literais, construtores (uns/zeros/aleatorio) e anotacoes
  // conhecidas em `shapes`, validando dimensoes (T012) como conv2d,
  // reformar, transposta e matmul. `std::nullopt` = forma desconhecida
  // (solver nao infere atraves de chamadas de funcao, condicionais etc.).
  std::optional<TensorShape> infer_shape(const ast::Expr& expr, const ShapeEnv& shapes);
  std::optional<TensorShape> check_conv2d(const ast::Expr& call, const ShapeEnv& shapes);
  std::optional<TensorShape> check_atencao(const ast::Expr& call, const ShapeEnv& shapes);
  std::optional<TensorShape> check_reshape(Span span, std::optional<TensorShape> in,
                                           const TensorShape& to);

  // Type inference (T011): infere o tipo de uma expressao a partir de
  // literais, builtins, metodos e anotacoes conhecidas em `types`, validando
  // operadores, chamadas de builtins e metodos quando o tipo esta evidente.
  // (Wrapper registra conhecidos em hover_types_/hover_shapes_ para o LSP;
  // a logica mora em infer_type_impl/infer_shape_impl.)
  // Desconhecido = sem verificacao (sem falsos positivos).
  // Tipos conhecidos sao registrados em hover_types_ para o LSP.
  sema::TypeKind infer_type(const ast::Expr& expr, const TypeEnv& types);
  sema::TypeKind infer_type_impl(const ast::Expr& expr, const TypeEnv& types);
  std::optional<TensorShape> infer_shape_impl(const ast::Expr& expr, const ShapeEnv& shapes);
  void check_return(const ast::Expr* value, Span span, const TypeEnv& types,
                    const ShapeEnv& shapes);
  // Aridade de chamada de `funcao` do usuario (T011). Faltantes sempre
  // acusam (runtime preenche com nulo em silencio); sobrantes so na forma
  // com parenteses — bare-call (`f x, y`) e guloso por desenho e o runtime
  // ignora o excedente. So chamadas 100% posicionais; o resto pula.
  void check_funcao_arity(const std::string& name, const std::vector<ast::Arg>& args, bool paren,
                          Span span);

  // Anotacoes de tipo ja resolvidas na passada 2 (params/retorno de
  // `funcao`, campos de `entrada:`/`saida:`), por expressao de tipo.
  std::unordered_map<const ast::Expr*, sema::Type> annotation_cache_;
  const sema::Type* current_ret_ = nullptr;  // retorno esperado (dentro de `funcao`)
  // Coleta de retornos (Marco 2 / C1): com silencioso_, check_return registra
  // os tipos inferidos em vez de validar (pre-passagem sem diagnostico duplo).
  bool silencioso_ = false;
  std::string funcao_coleta_;
  std::vector<sema::TypeKind> retornos_coletados_;
  // Formas de mapas/listas por variavel (ver MapShapes/ListElems acima).
  MapShapes formas_mapa_;
  ListElems elem_lista_;

  void define(const std::string& name, std::string kind, sema::Type type, Span span);
  const Symbol* lookup(std::string_view name) const;

  sema::Type resolve_type_expr(const ast::Expr& expr);
  void resolve_type_annotations(const ast::Item& decl);

  void walk_block(const ast::Block& block, std::string_view entity_kw);
  void visit_item(const ast::Item& item, std::string_view entity_kw);
  void check_device(const ast::Expr& value);
  void check_tool_list(const ast::Item& field);

  void report(DiagCode code, Span span, std::string message,
              std::vector<std::string> notes = {});

  const ast::Program& program_;
  DiagnosticEngine& diag_;
  std::unordered_map<std::string, Symbol> globals_;
  // Tipos/formas por expressao para o hover do LSP (so conhecidos).
  std::unordered_map<const ast::Expr*, sema::TypeKind> hover_types_;
  std::unordered_map<const ast::Expr*, std::vector<std::int64_t>> hover_shapes_;
};

// Convenience wrapper used by the CLI.
void check_program(const ast::Program& program, DiagnosticEngine& diag);

}  // namespace tilt
