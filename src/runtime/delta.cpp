#include "runtime/delta.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/json.hpp"
#include "runtime/parquet.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("delta: " + m); }

// definido mais abaixo; usado pelo schemaString e pelo log
std::string json_compact(const Value& v);

void mkdir_if_missing(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    die("nao foi possivel criar o diretorio '" + path + "'");
  }
}

std::int64_t file_size(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<std::int64_t>(st.st_size);
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Serializador JSONL compacto para o log do Delta (uma acao por linha).
// O json_dump do projeto pretty-printa; o protocolo Delta exige 1 linha.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string json_compact(const Value& v) {
  switch (v.kind) {
    case ValueKind::Nulo: return "null";
    case ValueKind::Logico: return v.b ? "true" : "false";
    case ValueKind::Inteiro: return std::to_string(v.i);
    case ValueKind::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%g", v.d);
      return buf;
    }
    case ValueKind::Texto: return "\"" + json_escape(v.s) + "\"";
    case ValueKind::Lista: {
      std::string out = "[";
      bool first = true;
      for (const Value& e : *v.list) {
        if (!first) out += ',';
        first = false;
        out += json_compact(e);
      }
      out += ']';
      return out;
    }
    case ValueKind::Mapa: {
      std::string out = "{";
      bool first = true;
      for (const auto& kv : v.map->items) {
        if (!first) out += ',';
        first = false;
        out += "\"" + json_escape(kv.first) + "\":" + json_compact(kv.second);
      }
      out += '}';
      return out;
    }
    default: return "null";
  }
}

std::string new_table_id() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  char buf[37];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(gen() & 0xFFFFFFFFu),
                static_cast<unsigned>(gen() & 0xFFFFu),
                static_cast<unsigned>(0x4000u | (gen() & 0x0FFFu)),
                static_cast<unsigned>(0x8000u | (gen() & 0x3FFFu)),
                static_cast<unsigned long long>(gen() & 0xFFFFFFFFFFFFull));
  return buf;
}

// schemaString no formato JSON do Delta/Spark:
//   {"type":"struct","fields":[{"name":"c","type":"long","nullable":false,...}]}
// Na leitura o schema real vem dos arquivos parquet; o schemaString fica
// registrado para ferramentas externas (delta-rs, Spark).
std::string delta_type_name(const Value& v) {
  switch (v.kind) {
    case ValueKind::Logico: return "boolean";
    case ValueKind::Inteiro: return "long";
    case ValueKind::Decimal: return "double";
    case ValueKind::Texto: return "string";
    default: return "string";
  }
}

std::string delta_schema_string(const Value& tabela) {
  if (tabela.kind != ValueKind::Lista && tabela.kind != ValueKind::Tabela) {
    die("escrever_delta espera uma tabela (lista de mapas)");
  }
  if (tabela.list->empty()) die("escrever_delta: tabela vazia (sem schema deduzivel)");
  const Value& first = tabela.list->front();
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");
  Value fields = Value::lista();
  for (const auto& kv : first.map->items) {
    Value f = Value::mapa();
    f.map->set("name", Value::texto(kv.first));
    f.map->set("type", Value::texto(delta_type_name(kv.second)));
    f.map->set("nullable", Value::logico(false));
    f.map->set("metadata", Value::mapa());
    fields.list->push_back(std::move(f));
  }
  Value schema = Value::mapa();
  schema.map->set("type", Value::texto("struct"));
  schema.map->set("fields", std::move(fields));
  return json_compact(schema);
}

// Lista os arquivos de <dir>/_delta_log/NNN.json em ordem crescente de versao.
std::vector<std::string> list_delta_versions(const std::string& log_dir) {
  DIR* d = ::opendir(log_dir.c_str());
  if (!d) die("diretorio '" + log_dir + "' nao encontrado (nao e uma tabela delta?)");
  std::vector<std::pair<std::string, std::string>> found;  // (nome, caminho)
  while (dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
      found.emplace_back(name, log_dir + "/" + name);
    }
  }
  ::closedir(d);
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  out.reserve(found.size());
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

}  // namespace

void delta_write(const std::string& dir, const Value& tabela) {
  const std::string schema = delta_schema_string(tabela);
  mkdir_if_missing(dir);
  const std::string log_dir = dir + "/_delta_log";
  mkdir_if_missing(log_dir);

  // 1a passada: tabela nova por escrita. Remove log anterior para nao
  // misturar versoes (sem merge/ACID concorrente).
  for (const std::string& old : list_delta_versions(log_dir)) {
    if (::unlink(old.c_str()) != 0) die("nao foi possivel limpar '" + old + "'");
  }

  const std::string part = "part-00000000-0000-4000-8000-" + new_table_id().substr(0, 12) + ".parquet";
  const std::string part_path = dir + "/" + part;
  parquet_write(part_path, tabela);
  const std::int64_t part_size = file_size(part_path);
  if (part_size <= 0) die("falha ao gravar '" + part_path + "'");

  const std::int64_t ts = now_ms();
  Value protocol = Value::mapa();
  protocol.map->set("minReaderVersion", Value::inteiro(1));
  protocol.map->set("minWriterVersion", Value::inteiro(2));

  Value format = Value::mapa();
  format.map->set("provider", Value::texto("parquet"));
  format.map->set("options", Value::mapa());

  Value meta = Value::mapa();
  meta.map->set("id", Value::texto(new_table_id()));
  meta.map->set("format", std::move(format));
  meta.map->set("schemaString", Value::texto(schema));
  meta.map->set("partitionColumns", Value::lista());
  meta.map->set("configuration", Value::mapa());
  meta.map->set("createdTime", Value::inteiro(ts));

  Value add = Value::mapa();
  add.map->set("path", Value::texto(part));
  add.map->set("partitionValues", Value::mapa());
  add.map->set("size", Value::inteiro(part_size));
  add.map->set("modificationTime", Value::inteiro(ts));
  add.map->set("dataChange", Value::logico(true));

  auto line = [&](const char* key, Value& payload) {
    Value row = Value::mapa();
    row.map->set(key, std::move(payload));
    return json_compact(row);
  };

  const std::string log_path = log_dir + "/00000000000000000000.json";
  std::ofstream log(log_path, std::ios::trunc);
  if (!log) die("nao foi possivel gravar '" + log_path + "'");
  log << line("protocol", protocol) << '\n';
  log << line("metaData", meta) << '\n';
  log << line("add", add) << '\n';
  if (!log) die("falha ao gravar '" + log_path + "'");
}

Value delta_read(const std::string& dir) {
  const std::string log_dir = dir + "/_delta_log";
  const std::vector<std::string> versions = list_delta_versions(log_dir);

  std::vector<std::string> active;  // arquivos ativos, em ordem de add
  for (const std::string& path : versions) {
    std::ifstream in(path);
    if (!in) die("nao foi possivel abrir '" + path + "'");
    std::string line_text;
    while (std::getline(in, line_text)) {
      if (line_text.empty()) continue;
      Value row;
      try {
        row = json_parse(line_text);
      } catch (const std::exception& e) {
        die("linha invalida no log '" + path + "': " + e.what());
      }
      if (row.kind != ValueKind::Mapa || !row.map) continue;
      if (const Value* add = row.map->find("add"); add && add->kind == ValueKind::Mapa && add->map) {
        const Value* p = add->map->find("path");
        if (p && p->kind == ValueKind::Texto) active.push_back(p->s);
      }
      if (const Value* rem = row.map->find("remove");
          rem && rem->kind == ValueKind::Mapa && rem->map) {
        const Value* p = rem->map->find("path");
        if (p && p->kind == ValueKind::Texto) {
          active.erase(std::remove(active.begin(), active.end(), p->s), active.end());
        }
      }
    }
  }
  if (active.empty()) die("tabela em '" + dir + "' esta vazia (nenhum arquivo ativo no log)");

  Value out = Value::tabela();
  std::vector<std::string> schema_cols;
  for (const std::string& part : active) {
    Value chunk = parquet_read(dir + "/" + part);
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
      die("arquivo '" + part + "' nao e uma tabela parquet");
    }
    for (Value& row : *chunk.list) {
      if (row.kind != ValueKind::Mapa || !row.map) die("linha de '" + part + "' nao e um mapa");
      if (schema_cols.empty()) {
        for (const auto& kv : row.map->items) schema_cols.push_back(kv.first);
      } else {
        if (row.map->items.size() != schema_cols.size()) {
          die("schema divergente em '" + part + "' (colunas diferentes da 1a versao)");
        }
        for (std::size_t k = 0; k < schema_cols.size(); ++k) {
          if (row.map->items[k].first != schema_cols[k]) {
            die("schema divergente em '" + part + "' (ordem/nome de colunas difere)");
          }
        }
      }
      out.list->push_back(std::move(row));
    }
  }
  return out;
}

}  // namespace tilt::rt
