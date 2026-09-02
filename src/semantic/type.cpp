#include "semantic/type.hpp"

namespace tilt::sema {

std::string type_to_string(const Type& t) {
  switch (t.kind) {
    case TypeKind::Unknown:
      return "?";
    case TypeKind::Nulo:
      return "nulo";
    case TypeKind::Texto:
      return "texto";
    case TypeKind::Inteiro:
      return "inteiro";
    case TypeKind::Decimal:
      return "decimal";
    case TypeKind::Logico:
      return "logico";
    case TypeKind::Tabela:
      return "tabela";
    case TypeKind::Lista:
      return "lista[" + (t.elem ? type_to_string(*t.elem) : "?") + "]";
    case TypeKind::Mapa:
      return "mapa[" + (t.key ? type_to_string(*t.key) : "?") + ", " +
             (t.elem ? type_to_string(*t.elem) : "?") + "]";
    case TypeKind::Opcional:
      return "opcional[" + (t.elem ? type_to_string(*t.elem) : "?") + "]";
    case TypeKind::Fluxo:
      return "fluxo[" + (t.elem ? type_to_string(*t.elem) : "?") + "]";
    case TypeKind::Tensor: {
      std::string s = "tensor[" + (t.name.empty() ? "?" : t.name);
      for (std::int64_t d : t.dims) s += ", " + (d < 0 ? std::string("_") : std::to_string(d));
      return s + "]";
    }
    case TypeKind::Registro:
      return t.name.empty() ? "registro" : t.name;
    case TypeKind::Funcao:
      return "funcao";
    case TypeKind::UniaoLiteral: {
      std::string s;
      for (std::size_t i = 0; i < t.literals.size(); ++i) {
        if (i) s += " | ";
        s += "\"" + t.literals[i] + "\"";
      }
      return s;
    }
    case TypeKind::Entidade:
      return t.entity_kind + " " + t.name;
  }
  return "?";
}

bool assignable(const Type& target, const Type& value) {
  if (target.kind == TypeKind::Unknown || value.kind == TypeKind::Unknown) return true;
  if (target.kind == TypeKind::Decimal && value.kind == TypeKind::Inteiro) return true;
  if (target.kind == TypeKind::UniaoLiteral && value.kind == TypeKind::Texto) return true;
  if (target.kind == TypeKind::Opcional) {
    if (value.kind == TypeKind::Nulo) return true;
    if (target.elem) return assignable(*target.elem, value);
  }
  if (target.kind != value.kind) return false;
  if (target.kind == TypeKind::Registro) return target.name == value.name || value.name.empty();
  if (target.kind == TypeKind::Tensor) {
    if (!target.name.empty() && !value.name.empty() && target.name != value.name) return false;
    if (target.dims.size() != value.dims.size()) return false;
    for (std::size_t i = 0; i < target.dims.size(); ++i) {
      if (target.dims[i] >= 0 && value.dims[i] >= 0 && target.dims[i] != value.dims[i]) return false;
    }
  }
  return true;
}

}  // namespace tilt::sema
