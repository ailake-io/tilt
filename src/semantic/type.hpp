#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tilt::sema {

enum class TypeKind {
  Unknown,  // not yet inferred / intentionally unconstrained
  Nulo,
  Texto,
  Inteiro,
  Decimal,
  Logico,
  Lista,          // elem
  Mapa,           // key, elem
  Opcional,       // elem
  Tensor,         // name = dtype, dims (-1 == symbolic '_')
  Tabela,
  Fluxo,          // elem
  Registro,       // name, fields
  Funcao,         // params, ret
  UniaoLiteral,   // literals
  Entidade,       // name, entity_kind ("modelo", "agente", "llm", ...)
};

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
  TypeKind kind = TypeKind::Unknown;

  std::string name;                 // Registro / Entidade name; Tensor dtype
  std::string entity_kind;          // Entidade: the declaring keyword
  std::vector<std::int64_t> dims;   // Tensor dimensions (-1 == symbolic)
  std::vector<std::string> literals;                  // UniaoLiteral members
  std::vector<std::pair<std::string, TypePtr>> fields;  // Registro fields
  std::vector<TypePtr> params;      // Funcao parameters
  TypePtr key;                      // Mapa key
  TypePtr elem;                     // Lista / Mapa value / Opcional / Fluxo
  TypePtr ret;                      // Funcao return

  static Type scalar(TypeKind k) {
    Type t;
    t.kind = k;
    return t;
  }
};

std::string type_to_string(const Type& t);

// Lenient assignability: Unknown is compatible with anything; Inteiro widens
// to Decimal; a Texto literal is compatible with a matching UniaoLiteral.
bool assignable(const Type& target, const Type& value);

}  // namespace tilt::sema
