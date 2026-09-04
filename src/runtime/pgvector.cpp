#include "runtime/pgvector.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

#include "runtime/postgres.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("pgvector: " + m); }

// Identificador de tabela seguro: [a-z0-9_], para nao permitir injecao SQL
// via nome de colecao vindo do programa tilt.
std::string sanitize_table(const std::string& name) {
  if (name.empty()) die("nome de tabela vazio");
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) die("nome de tabela/colecao '" + name + "' invalido (use [a-z0-9_])");
  }
  return name;
}

std::string sql_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  return out;
}

// Literal pgvector: '[1.5,2,...]'
std::string vec_literal(const std::vector<float>& vec) {
  std::string out = "'[";
  char num[32];
  for (std::size_t k = 0; k < vec.size(); ++k) {
    if (k) out += ',';
    std::snprintf(num, sizeof num, "%.9g", static_cast<double>(vec[k]));
    out += num;
  }
  out += "]'";
  return out;
}

void ensure_table(const std::string& url, const std::string& tabela, std::size_t dims) {
  // CREATE EXTENSION exige privilegio de superusuario/banco; se ja existir,
  // o erro e ignorado (o CREATE TABLE seguinte falha com mensagem clara se
  // a extensao nao estiver disponivel).
  try {
    postgres_exec(url, "CREATE EXTENSION IF NOT EXISTS vector");
  } catch (const std::exception&) {
    // sem privilegio: presume que a extensao ja esta instalada
  }
  postgres_exec(url, "CREATE TABLE IF NOT EXISTS " + tabela + " (id TEXT PRIMARY KEY, texto TEXT, " +
                         "embedding vector(" + std::to_string(dims) + "))");
}

}  // namespace

void pgvector_upsert(const std::string& url, const std::string& tabela_raw, const std::string& id,
                     const std::string& text, const std::vector<float>& vec) {
  if (vec.empty()) die("vetor vazio para o id '" + id + "'");
  const std::string tabela = sanitize_table(tabela_raw);
  ensure_table(url, tabela, vec.size());
  const std::string sql = "INSERT INTO " + tabela + " (id, texto, embedding) VALUES ('" +
                          sql_escape(id) + "', '" + sql_escape(text) + "', " + vec_literal(vec) +
                          ") ON CONFLICT (id) DO UPDATE SET texto = EXCLUDED.texto, " +
                          "embedding = EXCLUDED.embedding";
  postgres_exec(url, sql);
}

std::vector<std::pair<std::string, double>> pgvector_search(const std::string& url,
                                                            const std::string& tabela_raw,
                                                            const std::vector<float>& vec,
                                                            std::size_t k) {
  const std::string tabela = sanitize_table(tabela_raw);
  const std::string v = vec_literal(vec);
  const std::string sql = "SELECT id, 1 - (embedding <=> " + v +
                          ") AS score FROM " + tabela + " ORDER BY embedding <=> " + v +
                          " ASC LIMIT " + std::to_string(k);
  Value result = postgres_query(url, sql);
  std::vector<std::pair<std::string, double>> out;
  if (!result.list) return out;
  for (const Value& row : *result.list) {
    if (row.kind != ValueKind::Mapa || !row.map) continue;
    const Value* id = row.map->find("id");
    const Value* score = row.map->find("score");
    out.emplace_back(id && id->kind == ValueKind::Texto ? id->s : "?",
                     score ? score->as_number() : 0.0);
  }
  return out;
}

}  // namespace tilt::rt
