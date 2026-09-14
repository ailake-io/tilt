#pragma once

// Parametros `?` posicionais para SQL (Marco 3 / D1): conversao de Value,
// reescrita de placeholders com scanner que pula literais e comentarios,
// interpolacao segura (MySQL) e transacao como lista de passos.
// Header-only (sem mudanca no CMake).
//
// Uso:
//   executar_sql url, "insert into t values (?, ?)", [1, "ana"]
//   transacao url, [
//     { sql: "insert into t values (?, ?)", params: [1, "ana"] },
//     { sql: "update t set n = n + 1" },
//   ]

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Valor de parametro ja convertido do Value tilt.
struct SqlParam {
  enum class Tipo { Nulo, Inteiro, Decimal, Texto, Logico } tipo = Tipo::Nulo;
  std::int64_t i = 0;
  double d = 0.0;
  std::string s;
  bool b = false;
};

// Converte um valor tilt em parametro SQL. Listas/mapas/tabelas/tensores
// nao sao parametros validos (erro claro no chamador).
inline SqlParam param_de_valor(const Value& v, const std::string& ctx) {
  SqlParam p;
  switch (v.kind) {
    case ValueKind::Nulo: p.tipo = SqlParam::Tipo::Nulo; return p;
    case ValueKind::Inteiro: p.tipo = SqlParam::Tipo::Inteiro; p.i = v.i; return p;
    case ValueKind::Decimal: p.tipo = SqlParam::Tipo::Decimal; p.d = v.d; return p;
    case ValueKind::Texto: p.tipo = SqlParam::Tipo::Texto; p.s = v.s; return p;
    case ValueKind::Logico: p.tipo = SqlParam::Tipo::Logico; p.b = v.b; return p;
    default: break;
  }
  throw std::runtime_error(ctx + ": parametro deve ser inteiro, decimal, texto, logico ou nulo");
}

// Passo do scanner compartilhado: copia literais ('...', "..." com escapes)
// e comentarios (--, /* */) e sinaliza cada `?` fora deles. `emite()` e
// chamado para cada placeholder encontrado.
template <typename F>
inline void varrer_sql(const std::string& sql, F emite, std::string& out) {
  out.reserve(sql.size() + 8);
  std::size_t i = 0;
  while (i < sql.size()) {
    const char c = sql[i];
    if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') out += sql[i++];
      continue;
    }
    if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
      out += sql[i++];
      out += sql[i++];
      while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) out += sql[i++];
      if (i + 1 < sql.size()) {
        out += sql[i++];
        out += sql[i++];
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      out += sql[i++];
      while (i < sql.size()) {
        if (sql[i] == '\\' && i + 1 < sql.size()) {
          out += sql[i++];
          out += sql[i++];
          continue;
        }
        out += sql[i];
        if (sql[i] == c) {
          ++i;
          if (i < sql.size() && sql[i] == c) {
            out += sql[i++];
            continue;
          }
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '?') {
      emite(out);
      ++i;
      continue;
    }
    out += c;
    ++i;
  }
}

// Reescreve `?` fora de literais/comentarios. Estilos:
//   dolar  -> $1, $2, ... (postgres)
//   chave  -> {p0}, {p1}, ... (clickhouse; tipos resolvidos pelo chamador)
//   nenhum -> mantem `?` (sqlite/mysql/duckdb), so conta.
// Devolve {sql_reescrito, n_placeholders}.
inline std::pair<std::string, std::size_t> rewrite_qmarks(const std::string& sql,
                                                          const std::string& estilo) {
  std::string out;
  std::size_t n = 0;
  varrer_sql(sql, [&](std::string& o) {
    ++n;
    if (estilo == "dolar") {
      o += "$" + std::to_string(n);
    } else if (estilo == "chave") {
      o += "{p" + std::to_string(n - 1) + "}";
    } else {
      o += "?";
    }
  }, out);
  return {out, n};
}

// Interpola `?` (fora de literais/comentarios) chamando `formata(i)` para o
// i-esimo placeholder (0-based, texto ja seguro). Usado pelo MySQL (escape
// da conexao). Erro claro em contagem divergente.
template <typename F>
inline std::string interpolar_qmarks(const std::string& sql, std::size_t n_params, F formata,
                                     const std::string& passo) {
  std::string out;
  std::size_t nq = 0;
  varrer_sql(sql, [&](std::string& o) {
    if (nq >= n_params) {
      throw std::runtime_error(passo + "faltam parametros: o SQL tem mais '?' que valores");
    }
    o += formata(nq++);
  }, out);
  if (nq != n_params) {
    throw std::runtime_error(passo + "esperava " + std::to_string(n_params) +
                             " parametro(s), mas o SQL tem " + std::to_string(nq) + " '?'");
  }
  return out;
}

}  // namespace tilt::rt
