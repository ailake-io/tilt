#include "runtime/delta.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/compat.hpp"
#include "runtime/json.hpp"
#include "runtime/parquet.hpp"

namespace tilt::rt {

namespace {

constexpr const char* kHiveNullPartition = "__HIVE_DEFAULT_PARTITION__";

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("delta: " + m); }

// definido mais abaixo; usado pelo schemaString e pelo log
std::string json_compact(const Value& v);

void mkdir_if_missing(const std::string& path) {
  if (tilt_mkdir(path) != 0 && errno != EEXIST) {
    die("nao foi possivel criar o diretorio '" + path + "'");
  }
}

// Cria o caminho inteiro (pais inclusos), componente a componente. Usado
// para diretorios de particao aninhados (<c1>=<v1>/<c2>=<v2>).
void mkdir_p(const std::string& path) {
  std::string cur;
  cur.reserve(path.size());
  for (std::size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] != '/' && i + 1 != path.size()) continue;
    if (cur.empty() || cur == "/") continue;
    if (tilt_mkdir(cur) != 0 && errno != EEXIST) {
      die("nao foi possivel criar o diretorio '" + cur + "'");
    }
  }
}

std::int64_t file_size(const std::string& path) {
  return tilt_file_size(path);
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

// Colunas (nome + ordem) da 1a linha da tabela — criterio de schema usado
// tambem pela leitura ao concatenar os parquet das versoes.
std::vector<std::string> table_columns(const Value& tabela, const char* ctx) {
  if (tabela.kind != ValueKind::Lista && tabela.kind != ValueKind::Tabela) {
    die(std::string(ctx) + " espera uma tabela (lista de mapas)");
  }
  if (tabela.list->empty()) die(std::string(ctx) + ": tabela vazia (sem schema deduzivel)");
  const Value& first = tabela.list->front();
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");
  std::vector<std::string> cols;
  for (const auto& kv : first.map->items) cols.push_back(kv.first);
  return cols;
}

// Colunas (nome + ordem da 1a linha) da tabela com o tipo tilt deduzido do
// 1o valor nao nulo de cada coluna (tipo vazio = so nulos — o parquet ja
// falha com erro claro ao gravar). Resolucao por nome, nao por posicao.
std::vector<std::pair<std::string, std::string>> deduced_column_types(const Value& tabela,
                                                                      const char* ctx) {
  const std::vector<std::string> cols = table_columns(tabela, ctx);
  std::vector<std::pair<std::string, std::string>> out;
  for (const std::string& col : cols) {
    std::string ty;
    for (const Value& row : *tabela.list) {
      if (row.kind != ValueKind::Mapa || !row.map) break;
      const Value* cell = row.map->find(col);
      if (cell && cell->kind != ValueKind::Nulo) {
        ty = delta_type_name(*cell);
        break;
      }
    }
    out.emplace_back(col, ty);
  }
  return out;
}

bool column_has_null(const Value& tabela, const std::string& col) {
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) return true;
    const Value* cell = row.map->find(col);
    if (!cell || cell->kind == ValueKind::Nulo) return true;
  }
  return false;
}

bool schema_column_nullable(const std::string& schema_json, const std::string& col) {
  Value schema;
  try {
    schema = json_parse(schema_json);
  } catch (const std::exception&) {
    return false;
  }
  const Value* fields = schema.map ? schema.map->find("fields") : nullptr;
  if (!fields || fields->kind != ValueKind::Lista || !fields->list) return false;
  for (const Value& field : *fields->list) {
    const Value* name = field.map ? field.map->find("name") : nullptr;
    if (!name || name->kind != ValueKind::Texto || name->s != col) continue;
    const Value* nullable = field.map->find("nullable");
    return nullable && nullable->kind == ValueKind::Logico && nullable->b;
  }
  return false;
}

bool contains_col(const std::vector<std::pair<std::string, std::string>>& cols,
                  const std::string& name) {
  for (const auto& c : cols) {
    if (c.first == name) return true;
  }
  return false;
}

std::string delta_schema_string(const Value& tabela, const char* ctx) {
  if (tabela.kind != ValueKind::Lista && tabela.kind != ValueKind::Tabela) {
    die(std::string(ctx) + " espera uma tabela (lista de mapas)");
  }
  if (tabela.list->empty()) die(std::string(ctx) + ": tabela vazia (sem schema deduzivel)");
  const Value& first = tabela.list->front();
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");
  const auto types = deduced_column_types(tabela, ctx);
  Value fields = Value::lista();
  for (const auto& kv : first.map->items) {
    Value f = Value::mapa();
    f.map->set("name", Value::texto(kv.first));
    const auto type = std::find_if(types.begin(), types.end(),
                                   [&](const auto& item) { return item.first == kv.first; });
    f.map->set("type", Value::texto(type != types.end() && !type->second.empty()
                                         ? type->second
                                         : "string"));
    f.map->set("nullable", Value::logico(column_has_null(tabela, kv.first)));
    f.map->set("metadata", Value::mapa());
    fields.list->push_back(std::move(f));
  }
  Value schema = Value::mapa();
  schema.map->set("type", Value::texto("struct"));
  schema.map->set("fields", std::move(fields));
  return json_compact(schema);
}

std::string join_cols(const std::vector<std::string>& cols) {
  std::string out = "[";
  for (std::size_t i = 0; i < cols.size(); ++i) {
    if (i) out += ", ";
    out += cols[i];
  }
  out += ']';
  return out;
}

// schemaString do metaData mais recente do log (acoes em ordem de versao).
std::string current_schema_string(const std::vector<std::string>& versions) {
  std::string schema;
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
      if (const Value* md = row.map->find("metaData");
          md && md->kind == ValueKind::Mapa && md->map) {
        if (const Value* s = md->map->find("schemaString");
            s && s->kind == ValueKind::Texto) {
          schema = s->s;
        }
      }
    }
  }
  return schema;
}

// Valor do ultimo metaData do log (acoes em ordem de versao); Mapa vazio se
// nenhum. Usado no append para estender o schemaString preservando o id da
// tabela e as colunas de particao.
Value last_metadata_value(const std::vector<std::string>& versions) {
  Value meta = Value::mapa();
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
      if (const Value* md = row.map->find("metaData");
          md && md->kind == ValueKind::Mapa && md->map) {
        meta = *md;
      }
    }
  }
  return meta;
}

// Pares (nome, tipo delta) das colunas de uma schemaString no formato do
// Delta — usados na leitura para converter partitionValues (sempre texto no
// log) de volta ao tipo declarado.
std::vector<std::pair<std::string, std::string>> schema_string_fields(const std::string& schema_json) {
  Value v;
  try {
    v = json_parse(schema_json);
  } catch (const std::exception& e) {
    die("schemaString invalido no log: " + std::string(e.what()));
  }
  std::vector<std::pair<std::string, std::string>> fields;
  if (v.kind != ValueKind::Mapa || !v.map) return fields;
  const Value* fl = v.map->find("fields");
  if (!fl || fl->kind != ValueKind::Lista || !fl->list) return fields;
  for (const Value& f : *fl->list) {
    if (f.kind == ValueKind::Mapa && f.map) {
      const Value* n = f.map->find("name");
      const Value* t = f.map->find("type");
      fields.emplace_back(n && n->kind == ValueKind::Texto ? n->s : "",
                          t && t->kind == ValueKind::Texto ? t->s : "string");
    }
  }
  return fields;
}

// Nomes das colunas de uma schemaString no formato do Delta.
// (removida na fase 27: a validacao passou a ser por nome+tipo)

bool fields_contains(const std::vector<std::pair<std::string, std::string>>& fields,
                     const std::string& col) {
  for (const auto& f : fields) {
    if (f.first == col) return true;
  }
  return false;
}

// Promocao de tipo sem perda (widening, como Spark/mergeSchema): integer->long,
// float->double. Todo o resto divergente continua erro.
bool is_widening(const std::string& stored, const std::string& deduced) {
  return ((stored == "int" || stored == "integer") && deduced == "long") ||
         (stored == "float" && deduced == "double");
}

// partitionColumns do metaData mais recente do log (ordem de versao).
std::vector<std::string> current_partition_columns(const std::vector<std::string>& versions) {
  std::vector<std::string> cols;
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
      if (const Value* md = row.map->find("metaData");
          md && md->kind == ValueKind::Mapa && md->map) {
        if (const Value* pc = md->map->find("partitionColumns");
            pc && pc->kind == ValueKind::Lista && pc->list) {
          cols.clear();
          for (const Value& c : *pc->list) {
            if (c.kind == ValueKind::Texto) cols.push_back(c.s);
          }
        }
      }
    }
  }
  return cols;
}

// Valor de particao no caminho hive-style. Null usa o marcador padrao Hive;
// esse marcador e reservado, pois leitores Hive o interpretam como NULL.
std::string partition_value_string(const Value& v, const std::string& col) {
  switch (v.kind) {
    case ValueKind::Nulo: return kHiveNullPartition;
    case ValueKind::Logico: return v.b ? "true" : "false";
    case ValueKind::Inteiro: return std::to_string(v.i);
    case ValueKind::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%g", v.d);
      return buf;
    }
    case ValueKind::Texto:
      if (v.s.find('/') != std::string::npos) {
        die("valor da coluna de particao '" + col +
            "' contem '/' (caracteres especiais nao suportados)");
      }
      if (v.s == kHiveNullPartition) {
        die("valor da coluna de particao '" + col +
            "' e reservado para representar nulo no layout Hive");
      }
      return v.s;
    default:
      die("coluna de particao '" + col + "' deve ser texto, inteiro, decimal ou logico");
  }
}

// O protocolo Delta armazena `path` como URI relativa. Spark/delta-rs podem
// percent-encode espacos e caracteres reservados; `file://` absoluto tambem
// aparece em alguns writers. Normaliza os dois casos antes do acesso local.
std::string decode_delta_path(std::string path) {
  if (path.rfind("file://", 0) == 0) path.erase(0, 7);
  std::string out;
  out.reserve(path.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size()) {
      const int hi = hex(path[i + 1]);
      const int lo = hex(path[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(path[i]);
  }
  return out;
}

std::string delta_data_path(const std::string& dir, const std::string& path) {
  const std::string decoded = decode_delta_path(path);
  if (!decoded.empty() && decoded.front() == '/') return decoded;
  return dir + "/" + decoded;
}

// partitionValues do log sao strings; converte para o tipo declarado no
// schema (inteiro/decimal/logico). Conversao impossivel mantem texto.
Value partition_rehydrate(const std::string& s, const std::string& delta_type) {
  try {
    if (delta_type == "long" || delta_type == "integer" || delta_type == "short" ||
        delta_type == "byte") {
      return Value::inteiro(std::stoll(s));
    }
    if (delta_type == "double" || delta_type == "float") {
      return Value::decimal(std::stod(s));
    }
    if (delta_type == "boolean") {
      if (s == "true") return Value::logico(true);
      if (s == "false") return Value::logico(false);
      return Value::texto(s);
    }
  } catch (const std::exception&) {
    // fora de formato/alcance: mantem texto (decisao da fase 25)
  }
  return Value::texto(s);
}

bool partition_values_match(const Value& stored, const Value& predicate) {
  if (stored.kind == ValueKind::Nulo || predicate.kind == ValueKind::Nulo) {
    return stored.kind == ValueKind::Nulo && predicate.kind == ValueKind::Nulo;
  }
  return stored.kind == ValueKind::Texto && predicate.kind == ValueKind::Texto &&
         stored.s == predicate.s;
}

// Garante que todas as colunas de particao existem no schema da tabela (1a
// linha) e que nao ha repeticao.
void ensure_partition_columns(const Value& tabela, const std::vector<std::string>& cols,
                              const char* ctx) {
  const std::vector<std::string> schema_cols = table_columns(tabela, ctx);
  for (const std::string& col : cols) {
    if (std::count(cols.begin(), cols.end(), col) > 1) {
      die(std::string(ctx) + ": coluna de particao '" + col + "' repetida");
    }
    if (std::find(schema_cols.begin(), schema_cols.end(), col) == schema_cols.end()) {
      die(std::string(ctx) + ": coluna de particao '" + col +
          "' nao existe na tabela (colunas: " + join_cols(schema_cols) + ")");
    }
  }
}

// Lista os arquivos de <dir>/_delta_log/NNN.json em ordem crescente de versao.
std::vector<std::string> list_delta_versions(const std::string& log_dir) {
  std::vector<std::string> entries;
  if (!tilt_listdir(log_dir, entries)) {
    die("diretorio '" + log_dir + "' nao encontrado (nao e uma tabela delta?)");
  }
  std::vector<std::pair<std::string, std::string>> found;  // (nome, caminho)
  for (const std::string& name : entries) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
      found.emplace_back(name, log_dir + "/" + name);
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  out.reserve(found.size());
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

// Proximo indice livre part-NNNNN.parquet dentro de um diretorio de
// particao: conta os part-*.parquet ja presentes (reescrita e append nao
// colidem com arquivos de versoes anteriores, que permanecem no disco).
int next_part_index(const std::string& dir) {
  std::vector<std::string> entries;
  if (!tilt_listdir(dir, entries)) return 0;
  int n = 0;
  for (const std::string& name : entries) {
    if (name.rfind("part-", 0) == 0 && name.size() > 8 &&
        name.compare(name.size() - 8, 8, ".parquet") == 0) {
      ++n;
    }
  }
  return n;
}

// Linha sem as colunas de particao: o parquet do Delta nao armazena as
// colunas de particao (o valor vive no diretorio/partitionValues).
Value strip_partition_columns(const Value& row, const std::vector<std::string>& cols) {
  Value m = Value::mapa();
  for (const auto& kv : row.map->items) {
    if (std::find(cols.begin(), cols.end(), kv.first) == cols.end()) m.map->set(kv.first, kv.second);
  }
  return m;
}

struct PartitionGroup {
  std::string key;  // caminho relativo do diretorio: "c1=v1/c2=v2" ("" = raiz)
  std::vector<std::pair<std::string, Value>> partvals;  // valor tipado; null permanece null
  Value rows;         // tabela sem as colunas de particao
};

// Agrupa as linhas pela combinacao dos valores das colunas de particao, na
// ordem de 1a aparicao das chaves (define a ordem dos adds no log).
std::vector<PartitionGroup> partition_rows(const Value& tabela,
                                           const std::vector<std::string>& cols) {
  std::vector<PartitionGroup> grupos;
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
    PartitionGroup candidato;
    for (const std::string& col : cols) {
      const Value* cell = row.map->find(col);
      const Value part_value = cell ? *cell : Value::nulo();
      const std::string valor = partition_value_string(part_value, col);
      candidato.partvals.emplace_back(col, part_value);
      candidato.key += (candidato.key.empty() ? "" : "/") + col + "=" + valor;
    }
    auto it = std::find_if(grupos.begin(), grupos.end(),
                           [&](const PartitionGroup& g) { return g.key == candidato.key; });
    if (it == grupos.end()) {
      candidato.rows = Value::tabela();
      it = grupos.insert(grupos.end(), std::move(candidato));
    }
    it->rows.list->push_back(strip_partition_columns(row, cols));
  }
  return grupos;
}

Value make_add(const std::string& rel_path,
               const std::vector<std::pair<std::string, Value>>& partvals,
               std::int64_t size, std::int64_t ts) {
  Value add = Value::mapa();
  add.map->set("path", Value::texto(rel_path));
  Value pv = Value::mapa();
  for (const auto& kv : partvals) {
    pv.map->set(kv.first, kv.second.kind == ValueKind::Nulo
                              ? Value::nulo()
                              : Value::texto(partition_value_string(kv.second, kv.first)));
  }
  add.map->set("partitionValues", std::move(pv));
  add.map->set("size", Value::inteiro(size));
  add.map->set("modificationTime", Value::inteiro(ts));
  add.map->set("dataChange", Value::logico(true));
  return add;
}

// Grava os parquet de uma escrita/anexo. Com cols vazio, um unico arquivo na
// raiz (comportamento original); com cols, um arquivo por combinacao de
// valores em <dir>/<c1>=<v1>/<c2>=<v2>/part-NNNNN.parquet. Devolve os adds
// para o log.
std::vector<Value> write_partitions(const std::string& dir, const Value& tabela,
                                    const std::vector<std::string>& cols) {
  std::vector<Value> adds;
  std::vector<PartitionGroup> grupos;
  if (cols.empty()) {
    PartitionGroup g;
    g.rows = tabela;
    grupos.push_back(std::move(g));
  } else {
    grupos = partition_rows(tabela, cols);
  }
  for (const PartitionGroup& g : grupos) {
    const std::string subdir = g.key.empty() ? dir : dir + "/" + g.key;
    if (g.key.empty()) {
      mkdir_if_missing(subdir);
    } else {
      mkdir_p(subdir);
    }
    std::string nome;
    if (g.key.empty()) {
      // nomenclatura historica (uuid) preservada para tabelas sem particao
      nome = "part-00000000-0000-4000-8000-" + new_table_id().substr(0, 12) + ".parquet";
    } else {
      char buf[32];
      std::snprintf(buf, sizeof buf, "part-%05d.parquet", next_part_index(subdir));
      nome = buf;
    }
    const std::string fpath = subdir + "/" + nome;
    parquet_write(fpath, g.rows);
    const std::int64_t size = file_size(fpath);
    if (size <= 0) die("falha ao gravar '" + fpath + "'");
    const std::string rel = g.key.empty() ? nome : g.key + "/" + nome;
    adds.push_back(make_add(rel, g.partvals, size, now_ms()));
  }
  return adds;
}

// Checkpoint tilt-native (Fase 12-5a): sidecar que acelera a leitura sem
// quebrar interop — arquivos `<versao>.checkpoint.parquet` +
// `<versao>.checkpoint.meta.json` dentro de _delta_log, ignorados por
// leitores externos (delta-rs/Spark seguem replayando os JSONs). O nosso
// delta_read usa o checkpoint mais recente como base e so repassa os JSONs
// maiores que ele. Materializado a cada 10 versoes no append. O checkpoint
// padrao `_last_checkpoint` fica para a Fase 12-5b.
long long versao_de_json(const std::string& path) {
  const std::string base = path.substr(path.find_last_of('/') + 1);
  if (base.size() < 6) return -1;
  try {
    return std::stoll(base.substr(0, base.size() - 5));
  } catch (const std::exception&) {
    return -1;
  }
}

long long checkpoint_versao(const std::string& log_dir, long long limite = -1) {
  std::vector<std::string> entries;
  if (!tilt_listdir(log_dir, entries)) return -1;
  long long melhor = -1;
  for (const std::string& name : entries) {
    const std::string suf = ".checkpoint.parquet";
    if (name.size() <= suf.size() || name.compare(name.size() - suf.size(), suf.size(), suf) != 0) {
      continue;
    }
    try {
      const long long v = std::stoll(name.substr(0, 20));
      if (limite < 0 || v <= limite) melhor = std::max(melhor, v);
    } catch (const std::exception&) {
    }
  }
  return melhor;
}

std::string checkpoint_nome(long long v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%020lld.checkpoint.parquet", v);
  return buf;
}

std::string checkpoint_meta_nome(long long v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%020lld.checkpoint.meta.json", v);
  return buf;
}

// ---------------------------------------------------------------- Deletion Vectors
// Delta envolve um RoaringBitmap portable de 64 bits com magic little-endian
// 0x6439d3d1. O decoder abaixo cobre array, bitmap e run containers do
// formato portable, sem depender de libroaring no sistema.
constexpr std::uint32_t DV_MAGIC = 1681511377u;
constexpr std::uint32_t DV_NATIVE_MAGIC = 1681511376u;

std::uint16_t dv_u16(const std::string& b, std::size_t& p) {
  if (p + 2 > b.size()) die("Deletion Vector truncado (u16)");
  std::uint16_t v = static_cast<std::uint16_t>(static_cast<unsigned char>(b[p])) |
                    static_cast<std::uint16_t>(static_cast<unsigned char>(b[p + 1]) << 8);
  p += 2;
  return v;
}
std::uint32_t dv_u32(const std::string& b, std::size_t& p) {
  if (p + 4 > b.size()) die("Deletion Vector truncado (u32)");
  std::uint32_t v = static_cast<std::uint32_t>(static_cast<unsigned char>(b[p])) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 1])) << 8) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 2])) << 16) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 3])) << 24);
  p += 4;
  return v;
}
std::uint64_t dv_u64(const std::string& b, std::size_t& p) {
  std::uint64_t lo = dv_u32(b, p);
  std::uint64_t hi = dv_u32(b, p);
  return lo | (hi << 32);
}
std::uint32_t dv_be32(const std::string& b, std::size_t p) {
  if (p + 4 > b.size()) die("Deletion Vector truncado (be32)");
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 3]));
}

std::uint32_t dv_crc32(const std::string& b, std::size_t begin, std::size_t end) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = begin; i < end; ++i) {
    crc ^= static_cast<std::uint8_t>(b[i]);
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
  }
  return ~crc;
}

std::size_t decode_roaring32(const std::string& b, std::size_t start,
                             std::vector<std::uint32_t>& out) {
  std::size_t p = start;
  if (p + 4 > b.size()) die("Deletion Vector sem cookie Roaring");
  const std::uint32_t cookie = dv_u32(b, p);
  const bool no_run = cookie == 12346u;
  const bool has_run = (cookie & 0xFFFFu) == 12347u;
  if (!no_run && !has_run) die("Deletion Vector com cookie Roaring desconhecido");
  std::uint32_t n = no_run ? dv_u32(b, p) : ((cookie >> 16) + 1u);
  if (n > 65536u) die("Deletion Vector com numero de containers invalido");
  std::string run_flags;
  if (has_run) {
    const std::size_t bytes = (n + 7u) / 8u;
    if (p + bytes > b.size()) die("Deletion Vector sem run bitmap");
    run_flags.assign(b.data() + p, bytes);
    p += bytes;
  }
  std::vector<std::uint16_t> keys(n), cards(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    keys[i] = dv_u16(b, p);
    cards[i] = dv_u16(b, p);
  }
  const bool offsets = no_run || n >= 4;
  if (offsets) {
    for (std::uint32_t i = 0; i < n; ++i) (void)dv_u32(b, p);
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    const bool is_run = has_run && ((static_cast<unsigned char>(run_flags[i / 8]) >> (i % 8)) & 1u);
    const std::uint32_t card = static_cast<std::uint32_t>(cards[i]) + 1u;
    const std::uint32_t base = static_cast<std::uint32_t>(keys[i]) << 16;
    if (is_run) {
      const std::uint16_t runs = dv_u16(b, p);
      for (std::uint16_t r = 0; r < runs; ++r) {
        const std::uint16_t first = dv_u16(b, p);
        const std::uint16_t len = dv_u16(b, p);
        for (std::uint32_t v = 0; v <= len; ++v) out.push_back(base + first + v);
      }
    } else if (card <= 4096u) {
      for (std::uint32_t v = 0; v < card; ++v) out.push_back(base + dv_u16(b, p));
    } else {
      if (p + 8192 > b.size()) die("Deletion Vector bitset truncado");
      for (std::uint32_t word = 0; word < 1024; ++word) {
        std::uint64_t bits = dv_u64(b, p);
        while (bits) {
          unsigned bit = 0;
          while ((bits & 1u) == 0) {
            bits >>= 1;
            ++bit;
          }
          out.push_back(base + word * 64u + bit);
          bits &= bits - 1;
        }
      }
    }
  }
  return p;
}

std::set<std::uint64_t> decode_roaring64(const std::string& payload) {
  std::size_t p = 0;
  const std::uint64_t buckets = dv_u64(payload, p);
  if (buckets > 1000000) die("Deletion Vector com numero de buckets invalido");
  std::set<std::uint64_t> rows;
  for (std::uint64_t b = 0; b < buckets; ++b) {
    const std::uint32_t high = dv_u32(payload, p);
    std::vector<std::uint32_t> lows;
    const std::size_t end = decode_roaring32(payload, p, lows);
    p = end;
    for (std::uint32_t low : lows) rows.insert((static_cast<std::uint64_t>(high) << 32) | low);
  }
  return rows;
}

std::string dv_z85_decode(const std::string& encoded) {
  static const std::string alphabet =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#";
  if (encoded.size() % 5 != 0) die("Deletion Vector base85 com tamanho invalido");
  std::string out;
  out.reserve(encoded.size() / 5 * 4);
  for (std::size_t i = 0; i < encoded.size(); i += 5) {
    std::uint64_t value = 0;
    for (std::size_t k = 0; k < 5; ++k) {
      const std::size_t digit = alphabet.find(encoded[i + k]);
      if (digit == std::string::npos) die("Deletion Vector com caractere base85 invalido");
      value = value * 85 + digit;
    }
    if (value > 0xFFFFFFFFull) die("Deletion Vector base85 fora do intervalo");
    for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((value >> (k * 8)) & 0xFF));
  }
  return out;
}

std::string dv_unique_id(const Value& action) {
  const Value* dv = action.map ? action.map->find("deletionVector") : nullptr;
  if (!dv || dv->kind != ValueKind::Mapa || !dv->map) return {};
  const Value* st = dv->map->find("storageType");
  const Value* pi = dv->map->find("pathOrInlineDv");
  if (!st || st->kind != ValueKind::Texto || !pi || pi->kind != ValueKind::Texto) {
    die("Deletion Vector sem storageType/pathOrInlineDv");
  }
  std::string id = st->s + pi->s;
  if (const Value* off = dv->map->find("offset"); off && off->is_number()) {
    id += "@" + std::to_string(static_cast<long long>(off->as_number()));
  }
  return id;
}

std::set<std::uint64_t> read_deletion_vector(const Value& action, const std::string& dir) {
  const Value* dv = action.map ? action.map->find("deletionVector") : nullptr;
  if (!dv || dv->kind != ValueKind::Mapa || !dv->map) return {};
  const Value* st = dv->map->find("storageType");
  const Value* pi = dv->map->find("pathOrInlineDv");
  if (!st || st->kind != ValueKind::Texto || !pi || pi->kind != ValueKind::Texto) {
    die("Deletion Vector sem storageType/pathOrInlineDv");
  }
  std::string payload;
  if (st->s == "i") {
    payload = dv_z85_decode(pi->s);
  } else {
    std::string path;
    if (st->s == "p") {
      path = decode_delta_path(pi->s);
    } else if (st->s == "u") {
      if (pi->s.size() < 20) die("Deletion Vector relativo sem UUID base85");
      const std::string uuid = dv_z85_decode(pi->s.substr(pi->s.size() - 20));
      if (uuid.size() != 16) die("UUID de Deletion Vector invalido");
      char us[37];
      std::snprintf(us, sizeof us,
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    static_cast<unsigned char>(uuid[0]), static_cast<unsigned char>(uuid[1]),
                    static_cast<unsigned char>(uuid[2]), static_cast<unsigned char>(uuid[3]),
                    static_cast<unsigned char>(uuid[4]), static_cast<unsigned char>(uuid[5]),
                    static_cast<unsigned char>(uuid[6]), static_cast<unsigned char>(uuid[7]),
                    static_cast<unsigned char>(uuid[8]), static_cast<unsigned char>(uuid[9]),
                    static_cast<unsigned char>(uuid[10]), static_cast<unsigned char>(uuid[11]),
                    static_cast<unsigned char>(uuid[12]), static_cast<unsigned char>(uuid[13]),
                    static_cast<unsigned char>(uuid[14]), static_cast<unsigned char>(uuid[15]));
      const std::string prefix = pi->s.substr(0, pi->s.size() - 20);
      path = dir + (prefix.empty() ? "/" : "/" + prefix + "/") + "deletion_vector_" + us + ".bin";
    } else {
      die("Deletion Vector storageType '" + st->s + "' nao suportado");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) die("nao foi possivel abrir Deletion Vector '" + path + "'");
    payload.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    std::size_t offset = 1;
    if (const Value* off = dv->map->find("offset"); off && off->is_number()) {
      offset = static_cast<std::size_t>(off->as_number());
    }
    const Value* sz = dv->map->find("sizeInBytes");
    if (!sz || !sz->is_number()) die("Deletion Vector sem sizeInBytes");
    const std::size_t size = static_cast<std::size_t>(sz->as_number());
    if (offset + 4 + size + 4 > payload.size()) die("arquivo Deletion Vector truncado");
    const std::uint32_t stored = dv_be32(payload, offset);
    if (stored != size) die("sizeInBytes do Deletion Vector nao confere com o arquivo");
    const std::size_t magic_at = offset + 4;
    const std::size_t crc_at = magic_at + size;
    const std::uint32_t expected = dv_crc32(payload, magic_at, crc_at);
    const std::uint32_t got = dv_be32(payload, crc_at);
    if (expected != got) die("CRC32 do Deletion Vector nao confere");
    payload = payload.substr(magic_at, size);
  }
  if (payload.size() < 4) die("Deletion Vector sem magic");
  std::size_t mp = 0;
  const std::uint32_t magic = dv_u32(payload, mp);
  if (magic == DV_NATIVE_MAGIC) die("Deletion Vector native serialization nao suportada");
  if (magic != DV_MAGIC) die("magic de Deletion Vector invalido");
  std::set<std::uint64_t> rows = decode_roaring64(payload.substr(mp));
  if (const Value* card = dv->map->find("cardinality");
      card && card->is_number() && static_cast<std::size_t>(card->as_number()) != rows.size()) {
    die("cardinality do Deletion Vector nao confere");
  }
  return rows;
}

bool logs_have_deletion_vectors(const std::vector<std::string>& versions) {
  for (const std::string& path : versions) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("\"deletionVector\"") != std::string::npos) return true;
    }
  }
  return false;
}

// Reconstroi a lista de adds ativos repassando os JSONs (sem pruning) —
// usado para materializar o checkpoint apos o commit.
struct CpAdd {
  std::string path;
  std::string part_json;
  std::string dv_id;
  std::set<std::uint64_t> deleted_rows;
  std::int64_t size = 0;
  std::int64_t mtime = 0;
};

std::vector<CpAdd> coletar_ativos(const std::vector<std::string>& versions) {
  std::vector<CpAdd> ativos;
  for (const std::string& path : versions) {
    std::ifstream in(path);
    if (!in) die("nao foi possivel abrir '" + path + "'");
    std::string line_text;
    while (std::getline(in, line_text)) {
      if (line_text.empty()) continue;
      Value row = json_parse(line_text);
      if (row.kind != ValueKind::Mapa || !row.map) continue;
      if (const Value* add = row.map->find("add"); add && add->kind == ValueKind::Mapa && add->map) {
        const Value* p = add->map->find("path");
        if (!p || p->kind != ValueKind::Texto) continue;
        CpAdd a;
        a.path = decode_delta_path(p->s);
        a.dv_id = dv_unique_id(*add);
        if (const Value* pv = add->map->find("partitionValues");
            pv && pv->kind == ValueKind::Mapa && pv->map) {
          Value cpi = Value::mapa();
          for (const auto& kv : pv->map->items) {
            if (kv.second.kind == ValueKind::Texto || kv.second.kind == ValueKind::Nulo) {
              cpi.map->set(kv.first, kv.second);
            }
          }
          a.part_json = json_compact(cpi);
        } else {
          a.part_json = "{}";
        }
        if (const Value* s = add->map->find("size"); s && s->is_number()) {
          a.size = static_cast<std::int64_t>(s->as_number());
        }
        if (const Value* m = add->map->find("modificationTime"); m && m->is_number()) {
          a.mtime = static_cast<std::int64_t>(m->as_number());
        }
        ativos.push_back(std::move(a));
      }
      if (const Value* rem = row.map->find("remove");
          rem && rem->kind == ValueKind::Mapa && rem->map) {
        const Value* p = rem->map->find("path");
        if (p && p->kind == ValueKind::Texto) {
          ativos.erase(std::remove_if(ativos.begin(), ativos.end(),
                                      [&](const CpAdd& a) {
                                        return a.path == decode_delta_path(p->s) &&
                                               (dv_unique_id(*rem).empty() ||
                                                a.dv_id == dv_unique_id(*rem));
                                      }),
                       ativos.end());
        }
      }
    }
  }
  return ativos;
}

void delta_maybe_checkpoint(const std::string& dir, const std::string& log_dir, long long versao,
                            const std::string& schema_string,
                            const std::vector<std::string>& part_cols, const std::string& table_id) {
  (void)dir;
  if (versao < 0 || versao % 10 != 0) return;
  const std::vector<std::string> versions = list_delta_versions(log_dir);
  const std::vector<CpAdd> ativos = coletar_ativos(versions);
  Value tabela = Value::tabela();
  for (const CpAdd& a : ativos) {
    Value r = Value::mapa();
    r.map->set("caminho", Value::texto(a.path));
    r.map->set("particao_json", Value::texto(a.part_json));
    r.map->set("tamanho", Value::inteiro(a.size));
    r.map->set("mtime", Value::inteiro(a.mtime));
    tabela.list->push_back(std::move(r));
  }
  const std::string final_pq = log_dir + "/" + checkpoint_nome(versao);
  const std::string tmp_pq = final_pq + ".tmp";
  parquet_write(tmp_pq, tabela);
  if (::rename(tmp_pq.c_str(), final_pq.c_str()) != 0) {
    std::remove(tmp_pq.c_str());
    return;  // checkpoint e best-effort: o log JSON continua autoritativo
  }
  Value meta = Value::mapa();
  meta.map->set("version", Value::inteiro(versao));
  meta.map->set("schemaString", Value::texto(schema_string));
  Value pcs = Value::lista();
  for (const auto& c : part_cols) pcs.list->push_back(Value::texto(c));
  meta.map->set("partitionColumns", std::move(pcs));
  meta.map->set("table_id", Value::texto(table_id));
  const std::string final_meta = log_dir + "/" + checkpoint_meta_nome(versao);
  const std::string tmp_meta = final_meta + ".tmp";
  {
    std::ofstream out(tmp_meta, std::ios::trunc);
    if (out) out << json_compact(meta) << "\n";
  }
  if (::rename(tmp_meta.c_str(), final_meta.c_str()) != 0) std::remove(tmp_meta.c_str());
}

// Checkpoint padrao Delta (Marco 1 / A3): `_delta_log/_last_checkpoint`
// {"version": N} + `<20-digit>.checkpoint*.parquet` no schema oficial
// (colunas add/remove/metaData/protocol como structs; partitionValues como
// MAP<STRING,STRING>). Lido como base (adds menos removes + metaData) com
// replay apenas dos JSONs maiores que N — tabelas escritas por Spark/
// delta-rs com checkpoint passam a ler rapido. Coexiste com o sidecar
// tilt-native (usa-se o de maior versao; empate: o padrao).
long long last_checkpoint_version(const std::string& log_dir, long long limite = -1) {
  std::ifstream in(log_dir + "/_last_checkpoint");
  if (!in) return -1;
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    Value v = json_parse(ss.str());
    if (v.kind == ValueKind::Mapa && v.map) {
      if (const Value* n = v.map->find("version"); n && n->is_number()) {
        const long long v = static_cast<long long>(n->as_number());
        return limite < 0 || v <= limite ? v : -1;
      }
    }
  } catch (const std::exception&) {
  }
  return -1;
}

// Arquivos `<versao>.checkpoint*.parquet` do log (padrao ou tilt-native).
std::vector<std::string> checkpoint_files(const std::string& log_dir, long long versao) {
  char prefix[32];
  std::snprintf(prefix, sizeof prefix, "%020lld.checkpoint", versao);
  const std::string pre = prefix;
  std::vector<std::string> entries;
  std::vector<std::string> out;
  if (!tilt_listdir(log_dir, entries)) return out;
  for (const std::string& n : entries) {
    if (n.size() >= pre.size() + 8 && n.compare(0, pre.size(), pre) == 0 &&
        n.compare(n.size() - 8, 8, ".parquet") == 0) {
      out.push_back(log_dir + "/" + n);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

struct StdCheckpoint {
  long long version = -1;
  std::vector<CpAdd> adds;  // estado (adds menos removes) na versao
  std::string schema_string;
  std::vector<std::string> part_cols;
};

// Carrega o checkpoint padrao da versao V (nullopt se ilegivel/ausente).
// Reaproveita o parquet_read (structs + MAP) e interpreta add/remove/metaData.
std::optional<StdCheckpoint> load_standard_checkpoint(const std::string& log_dir, long long v) {
  const std::vector<std::string> files = checkpoint_files(log_dir, v);
  if (files.empty()) return std::nullopt;
  try {
    StdCheckpoint cp;
    cp.version = v;
    std::vector<CpAdd> ativos;
    for (const std::string& f : files) {
      Value t = parquet_read(f);
      if (t.kind != ValueKind::Tabela && t.kind != ValueKind::Lista) continue;
      // Tilt-native (coluna "caminho") nao e padrao: ignora aqui.
      bool tilt_native = false;
      if (!t.list->empty() && (*t.list)[0].kind == ValueKind::Mapa &&
          (*t.list)[0].map->find("caminho")) {
        tilt_native = true;
      }
      if (tilt_native) continue;
      for (const Value& row : *t.list) {
        if (row.kind != ValueKind::Mapa || !row.map) continue;
        if (const Value* add = row.map->find("add");
            add && add->kind == ValueKind::Mapa && add->map) {
          const Value* p = add->map->find("path");
          if (!p || p->kind != ValueKind::Texto) continue;
          CpAdd a;
          a.path = decode_delta_path(p->s);
          a.dv_id = dv_unique_id(*add);
          a.deleted_rows = read_deletion_vector(*add, log_dir.substr(0, log_dir.size() - 10));
          if (const Value* pv = add->map->find("partitionValues");
              pv && pv->kind == ValueKind::Mapa && pv->map) {
            Value cpi = Value::mapa();
            for (const auto& kv : pv->map->items) {
              if (kv.second.kind == ValueKind::Texto || kv.second.kind == ValueKind::Nulo) {
                cpi.map->set(kv.first, kv.second);
              }
            }
            a.part_json = json_compact(cpi);
          } else {
            a.part_json = "{}";
          }
          if (const Value* s = add->map->find("size"); s && s->is_number()) {
            a.size = static_cast<std::int64_t>(s->as_number());
          }
          if (const Value* m = add->map->find("modificationTime"); m && m->is_number()) {
            a.mtime = static_cast<std::int64_t>(m->as_number());
          }
          ativos.push_back(std::move(a));
        }
        if (const Value* rem = row.map->find("remove");
            rem && rem->kind == ValueKind::Mapa && rem->map) {
          const Value* p = rem->map->find("path");
          if (p && p->kind == ValueKind::Texto) {
            ativos.erase(std::remove_if(ativos.begin(), ativos.end(),
                                        [&](const CpAdd& a) { return a.path == p->s; }),
                         ativos.end());
          }
        }
        if (const Value* md = row.map->find("metaData");
            md && md->kind == ValueKind::Mapa && md->map) {
          if (const Value* s = md->map->find("schemaString");
              s && s->kind == ValueKind::Texto) {
            cp.schema_string = s->s;
          }
          if (const Value* pc = md->map->find("partitionColumns");
              pc && pc->kind == ValueKind::Lista && pc->list) {
            cp.part_cols.clear();
            for (const Value& c : *pc->list) {
              if (c.kind == ValueKind::Texto) cp.part_cols.push_back(c.s);
            }
          }
        }
      }
    }
    cp.adds = std::move(ativos);
    return cp;
  } catch (const std::exception&) {
    return std::nullopt;  // checkpoint ilegivel: replay completo dos JSONs
  }
}

void list_delta_parquets(const std::string& dir, const std::string& rel,
                         std::vector<std::string>& out) {
  const std::string current = rel.empty() ? dir : dir + "/" + rel;
  std::vector<std::string> entries;
  if (!tilt_listdir(current, entries)) return;
  for (const std::string& name : entries) {
    if (name == "_delta_log" && rel.empty()) continue;
    const std::string child_rel = rel.empty() ? name : rel + "/" + name;
    const std::string child = dir + "/" + child_rel;
    if (tilt_is_directory(child)) {
      list_delta_parquets(dir, child_rel, out);
    } else if (name.size() > 8 && name.compare(name.size() - 8, 8, ".parquet") == 0) {
      out.push_back(child_rel);
    }
  }
}

std::set<std::string> delta_logged_files(const std::vector<std::string>& versions) {
  std::set<std::string> referenced;
  for (const std::string& path : versions) {
    std::ifstream in(path);
    if (!in) die("nao foi possivel abrir '" + path + "'");
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      Value row = json_parse(line);
      if (row.kind != ValueKind::Mapa || !row.map) continue;
      const Value* add = row.map->find("add");
      if (!add || add->kind != ValueKind::Mapa || !add->map) continue;
      const Value* p = add->map->find("path");
      if (p && p->kind == ValueKind::Texto) referenced.insert(p->s);
    }
  }
  return referenced;
}

}  // namespace

void delta_write(const std::string& dir, const Value& tabela,
                 const std::vector<std::string>& part_cols_req) {
  const std::string schema = delta_schema_string(tabela, "escrever_delta");
  if (!part_cols_req.empty()) ensure_partition_columns(tabela, part_cols_req, "escrever_delta");
  mkdir_if_missing(dir);
  const std::string log_dir = dir + "/_delta_log";
  mkdir_if_missing(log_dir);

  // 1a passada: tabela nova por escrita. Remove log anterior para nao
  // misturar versoes (sem merge/ACID concorrente), incluindo checkpoints.
  for (const std::string& old : list_delta_versions(log_dir)) {
    if (std::remove(old.c_str()) != 0) die("nao foi possivel limpar '" + old + "'");
  }
  {
    std::vector<std::string> entries;
    if (tilt_listdir(log_dir, entries)) {
      for (const std::string& n : entries) {
        if (n.find(".checkpoint") != std::string::npos) {
          std::remove((log_dir + "/" + n).c_str());
        }
      }
    }
  }

  const std::int64_t ts = now_ms();
  std::vector<Value> adds = write_partitions(dir, tabela, part_cols_req);

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
  Value part_cols = Value::lista();
  for (const std::string& c : part_cols_req) part_cols.list->push_back(Value::texto(c));
  meta.map->set("partitionColumns", std::move(part_cols));
  meta.map->set("configuration", Value::mapa());
  meta.map->set("createdTime", Value::inteiro(ts));

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
  for (Value& add : adds) {
    log << line("add", add) << '\n';
  }
  if (!log) die("falha ao gravar '" + log_path + "'");
}

void delta_append(const std::string& dir, const Value& tabela,
                  const std::vector<std::string>& part_cols_req) {
  const std::string log_dir = dir + "/_delta_log";
  if (!tilt_is_directory(log_dir)) {
    die("tabela nao existe em '" + dir + "' (use escrever_delta para criar)");
  }
  const std::vector<std::string> versions = list_delta_versions(log_dir);
  if (versions.empty()) {
    die("tabela nao existe em '" + dir + "' (use escrever_delta para criar)");
  }

  // Validacao de schema por nome (evolucao de schema, fase 27): toda coluna
  // do schema atual precisa existir na tabela anexada (remover coluna ->
  // erro); coluna em comum precisa ter o mesmo tipo; colunas novas entram
  // como nullable no fim do schemaString.
  const Value cur_meta = last_metadata_value(versions);
  const Value* cur_schema_v = cur_meta.map ? cur_meta.map->find("schemaString") : nullptr;
  if (!cur_schema_v || cur_schema_v->kind != ValueKind::Texto) {
    die("tabela em '" + dir + "' nao tem schema no log (metaData ausente)");
  }
  const std::vector<std::pair<std::string, std::string>> cur_fields =
      schema_string_fields(cur_schema_v->s);
  if (cur_fields.empty()) {
    die("tabela em '" + dir + "' nao tem schema no log (metaData ausente)");
  }
  const std::vector<std::pair<std::string, std::string>> new_types =
      deduced_column_types(tabela, "anexar_delta");
  std::vector<std::pair<std::string, std::string>> added_cols;
  std::vector<std::string> nullable_cols;
  // Nome -> tipo promovido (widening int->long, float->double).
  std::vector<std::pair<std::string, std::string>> widened_cols;
  for (const auto& old : cur_fields) {
    const std::string* ty = nullptr;
    for (const auto& nt : new_types) {
      if (nt.first == old.first) ty = &nt.second;
    }
    if (!ty) {
      die("anexar_delta: coluna '" + old.first +
          "' ausente na tabela anexada (evolucao de schema suporta apenas adicao de colunas)");
    }
    if (column_has_null(tabela, old.first) &&
        !schema_column_nullable(cur_schema_v->s, old.first)) {
      nullable_cols.push_back(old.first);
    }
    if (!ty->empty() && *ty != old.second) {
      if (is_widening(old.second, *ty)) {
        widened_cols.emplace_back(old.first, *ty);
      } else {
          die("anexar_delta: coluna '" + old.first + "' com tipo divergente (esperado " +
            old.second + "; recebido " + *ty +
            ") (evolucao de schema: apenas adicao de colunas e widening integer->long, "
            "float->double)");
      }
    }
  }
  for (const auto& nt : new_types) {
    if (!contains_col(cur_fields, nt.first)) added_cols.push_back(nt);
  }

  // schemaString estendido (colunas novas nullable no fim; tipos promovidos
  // reescritos) para o commit.
  std::string new_schema;
  if (!added_cols.empty() || !widened_cols.empty() || !nullable_cols.empty()) {
    Value schema;
    try {
      schema = json_parse(cur_schema_v->s);
    } catch (const std::exception& e) {
      die("schemaString invalido no log: " + std::string(e.what()));
    }
    Value* fields = schema.map ? schema.map->find("fields") : nullptr;
    if (!fields || fields->kind != ValueKind::Lista || !fields->list) {
      die("schemaString invalido no log (sem fields)");
    }
    for (Value& f : *fields->list) {
      if (f.kind != ValueKind::Mapa || !f.map) continue;
      const Value* nm = f.map->find("name");
      if (!nm || nm->kind != ValueKind::Texto) continue;
      if (std::find(nullable_cols.begin(), nullable_cols.end(), nm->s) !=
          nullable_cols.end()) {
        f.map->set("nullable", Value::logico(true));
      }
      for (const auto& [wname, wty] : widened_cols) {
        if (nm->s == wname) f.map->set("type", Value::texto(wty));
      }
    }
    for (const auto& [name, ty] : added_cols) {
      Value f = Value::mapa();
      f.map->set("name", Value::texto(name));
      f.map->set("type", Value::texto(ty.empty() ? "string" : ty));
      f.map->set("nullable", Value::logico(true));
      f.map->set("metadata", Value::mapa());
      fields->list->push_back(std::move(f));
    }
    new_schema = json_compact(schema);
  }

  // Particao: herda a(s coluna)s da tabela existente; erro se explicita e
  // divergente (a ordem das colunas importa).
  const std::vector<std::string> existing = current_partition_columns(versions);
  std::vector<std::string> part_cols = existing;
  if (!existing.empty()) {
    if (!part_cols_req.empty() && part_cols_req != existing) {
      die("anexar_delta: tabela em '" + dir + "' ja e particionada por " + join_cols(existing) +
          " (recebido particionar_por: " + join_cols(part_cols_req) + ")");
    }
    ensure_partition_columns(tabela, existing, "anexar_delta");
  } else if (!part_cols_req.empty()) {
    die("anexar_delta: tabela em '" + dir + "' nao e particionada — recrie-a com escrever_delta " +
        "tabela, \"" + dir + "\", particionar_por: \"" + part_cols_req.front() + "\"");
  }

  // Grava os parquet ANTES de commitar; se der crash antes do rename, sobra
  // um parquet orfao que a leitura ignora (nao esta no log).
  const std::int64_t ts = now_ms();
  std::vector<Value> adds = write_partitions(dir, tabela, part_cols);

  // Proxima versao = maior numero de <log_dir>/*.json + 1. Os nomes sao
  // zero-padded de 20 digitos, entao a ordem lexicografica == numerica.
  long long next = -1;
  for (const std::string& p : versions) {
    std::string base = p.substr(p.find_last_of('/') + 1);
    base = base.substr(0, base.size() - 5);
    try {
      next = std::max(next, std::stoll(base));
    } catch (const std::exception&) {
      // nome nao numerico no log: ignora no calculo da versao
    }
  }
  next += 1;
  char num[32];
  std::snprintf(num, sizeof num, "%020lld", next);
  const std::string final_path = log_dir + "/" + num + ".json";

  Value commit = Value::mapa();
  commit.map->set("timestamp", Value::inteiro(ts));
  commit.map->set("operation", Value::texto("APPEND"));

  auto line = [](const char* key, Value& payload) {
    Value row = Value::mapa();
    row.map->set(key, std::move(payload));
    return json_compact(row);
  };

  // Commit atomico: JSONL num temporario do mesmo diretorio, fecha e rename()
  // para o nome final (rename atomico no mesmo filesystem).
  const std::string tmp_path = log_dir + "/.commit-" + std::to_string(tilt::rt::tilt_getpid()) + ".tmp";
  {
    std::ofstream log(tmp_path, std::ios::trunc);
    if (!log) die("nao foi possivel gravar '" + tmp_path + "'");
    log << line("commitInfo", commit) << '\n';
    if (!new_schema.empty()) {
      // evolucao de schema: novo metaData com o schemaString estendido
      // (id da tabela e colunas de particao preservados do metaData atual)
      Value meta = Value::mapa();
      const Value* id = cur_meta.map ? cur_meta.map->find("id") : nullptr;
      meta.map->set("id", id && id->kind == ValueKind::Texto ? *id : Value::texto(new_table_id()));
      Value format = Value::mapa();
      format.map->set("provider", Value::texto("parquet"));
      format.map->set("options", Value::mapa());
      meta.map->set("format", std::move(format));
      meta.map->set("schemaString", Value::texto(new_schema));
      Value part_cols = Value::lista();
      for (const std::string& c : existing) part_cols.list->push_back(Value::texto(c));
      meta.map->set("partitionColumns", std::move(part_cols));
      meta.map->set("configuration", Value::mapa());
      log << line("metaData", meta) << '\n';
    }
    for (Value& add : adds) {
      log << line("add", add) << '\n';
    }
    log.flush();
    if (!log) die("falha ao gravar '" + tmp_path + "'");
  }
  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    die("falha ao commitar a versao em '" + final_path + "'");
  }
  // Checkpoint tilt-native a cada 10 versoes (best-effort; o JSON continua
  // autoritativo).
  try {
    const std::string schema_final = new_schema.empty() && cur_schema_v ? cur_schema_v->s : new_schema;
    const Value* tid = cur_meta.map ? cur_meta.map->find("id") : nullptr;
    delta_maybe_checkpoint(dir, log_dir, next, schema_final, existing,
                           tid && tid->kind == ValueKind::Texto ? tid->s : "");
  } catch (const std::exception&) {
  }
}

// Igualdade entre o valor de uma celula e um predicado de `onde`.
// Inteiro/decimal comparam numericamente entre si; o resto exige o mesmo tipo.
bool pred_eq(const Value& cell, const Value& pred) {
  const bool cell_num = cell.kind == ValueKind::Inteiro || cell.kind == ValueKind::Decimal;
  const bool pred_num = pred.kind == ValueKind::Inteiro || pred.kind == ValueKind::Decimal;
  if (cell_num && pred_num) return cell.as_number() == pred.as_number();
  if (cell.kind != pred.kind) return false;
  switch (pred.kind) {
    case ValueKind::Nulo: return true;
    case ValueKind::Logico: return cell.b == pred.b;
    case ValueKind::Texto: return cell.s == pred.s;
    default: return false;
  }
}

Value delta_read(const std::string& dir, const Value* onde, long long versao) {
  const std::string log_dir = dir + "/_delta_log";
  std::vector<std::string> versions = list_delta_versions(log_dir);
  if (versao >= 0) {
    std::vector<std::string> ate;
    for (const std::string& path : versions) {
      if (versao_de_json(path) <= versao) ate.push_back(path);
    }
    versions = std::move(ate);
    if (versions.empty() || versao_de_json(versions.back()) != versao) {
      die("snapshot Delta versao " + std::to_string(versao) + " nao encontrado");
    }
  }

  // Predicados de `onde` separados em: (a) pruning — coluna de particao da
  // tabela, compara contra partitionValues do log e pula arquivo inteiro;
  // (b) residual — filtra linhas apos a reidratacao.
  std::vector<std::pair<std::string, Value>> prune_preds, residual_preds;
  if (onde && onde->kind == ValueKind::Mapa && onde->map) {
    const std::vector<std::string> part_cols = current_partition_columns(versions);
    for (const auto& kv : onde->map->items) {
      auto is_part = std::find(part_cols.begin(), part_cols.end(), kv.first);
      if (is_part != part_cols.end()) {
        // PartitionValues do Delta sao strings, exceto null, que permanece tipado.
        prune_preds.emplace_back(
            kv.first, kv.second.kind == ValueKind::Nulo
                          ? Value::nulo()
                          : Value::texto(partition_value_string(kv.second, kv.first)));
      } else {
        residual_preds.push_back(kv);
      }
    }
  }

  // Arquivos ativos + partitionValues: textos e null tipado para particoes Hive default.
  struct ActiveFile {
    std::string path;
    std::string dv_id;
    std::set<std::uint64_t> deleted_rows;
    std::vector<std::pair<std::string, Value>> partvals;
  };
  std::vector<ActiveFile> active;  // em ordem de add
  // Base de checkpoint (evita repassar JSONs antigos; a cauda e replayada
  // abaixo): padrao (`_last_checkpoint`) ou tilt-native, o de maior versao.
  const long long cp_ver = checkpoint_versao(log_dir, versao);
  const long long std_ver = last_checkpoint_version(log_dir, versao);
  auto passa_prune = [&](const std::vector<std::pair<std::string, Value>>& partvals) {
    for (const auto& [col, val] : prune_preds) {
      auto pv = std::find_if(partvals.begin(), partvals.end(),
                             [&](const auto& kv) { return kv.first == col; });
      if (pv == partvals.end() || !partition_values_match(pv->second, val)) {
        return false;
      }
    }
    return true;
  };
  auto semeia = [&](const std::vector<CpAdd>& adds) {
    for (const CpAdd& a : adds) {
      ActiveFile f;
      f.path = a.path;
      f.dv_id = a.dv_id;
      f.deleted_rows = a.deleted_rows;
      try {
        Value pv = json_parse(a.part_json);
        if (pv.kind == ValueKind::Mapa && pv.map) {
          for (const auto& kv : pv.map->items) {
            if (kv.second.kind == ValueKind::Texto || kv.second.kind == ValueKind::Nulo) {
              f.partvals.emplace_back(kv.first, kv.second);
            }
          }
        }
      } catch (const std::exception&) {
      }
      if (passa_prune(f.partvals)) active.push_back(std::move(f));
    }
  };
  const bool has_dvs = logs_have_deletion_vectors(versions);
  long long base_ver = -1;
  // O checkpoint tilt-native antigo não carrega o descriptor DV. Em uma
  // tabela com DVs o replay JSON é a fonte segura; checkpoint padrão segue
  // disponível somente quando não há DVs.
  if (!has_dvs && std_ver >= 0 && std_ver >= cp_ver) {
    if (const auto scp = load_standard_checkpoint(log_dir, std_ver)) {
      semeia(scp->adds);
      base_ver = std_ver;
    }
  }
  if (!has_dvs && base_ver < 0 && cp_ver >= 0) {
    try {
      Value base = parquet_read(log_dir + "/" + checkpoint_nome(cp_ver));
      if (base.kind == ValueKind::Tabela || base.kind == ValueKind::Lista) {
        std::vector<CpAdd> adds;
        for (const Value& row : *base.list) {
          if (row.kind != ValueKind::Mapa || !row.map) continue;
          const Value* c = row.map->find("caminho");
          const Value* pj = row.map->find("particao_json");
          if (!c || c->kind != ValueKind::Texto) continue;
          CpAdd a;
          a.path = c->s;
          a.part_json =
              (pj && pj->kind == ValueKind::Texto) ? pj->s : std::string("{}");
          adds.push_back(std::move(a));
        }
        semeia(adds);
        base_ver = cp_ver;
      }
    } catch (const std::exception&) {
      active.clear();  // checkpoint ilegivel: volta ao replay completo
    }
  }
  for (const std::string& path : versions) {
    if (base_ver >= 0 && versao_de_json(path) <= base_ver) continue;  // coberto pelo checkpoint
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
        if (p && p->kind == ValueKind::Texto) {
          ActiveFile f;
          f.path = decode_delta_path(p->s);
          f.dv_id = dv_unique_id(*add);
          f.deleted_rows = read_deletion_vector(*add, dir);
          if (const Value* pv = add->map->find("partitionValues");
              pv && pv->kind == ValueKind::Mapa && pv->map) {
            for (const auto& kv : pv->map->items) {
              if (kv.second.kind == ValueKind::Texto || kv.second.kind == ValueKind::Nulo) {
                f.partvals.emplace_back(kv.first, kv.second);
              }
            }
          }
          // Pruning: arquivo precisa bater com todos os predicados, inclusive null.
          bool passa = true;
          for (const auto& [col, val] : prune_preds) {
            auto pv = std::find_if(f.partvals.begin(), f.partvals.end(),
                                   [&](const auto& kv) { return kv.first == col; });
            if (pv == f.partvals.end() || !partition_values_match(pv->second, val)) {
              passa = false;
              break;
            }
          }
          if (passa) {
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](const ActiveFile& old) {
                                          return old.path == f.path && old.dv_id == f.dv_id;
                                        }),
                         active.end());
            active.push_back(std::move(f));
          }
        }
      }
      if (const Value* rem = row.map->find("remove");
          rem && rem->kind == ValueKind::Mapa && rem->map) {
        const Value* p = rem->map->find("path");
        if (p && p->kind == ValueKind::Texto) {
          active.erase(std::remove_if(active.begin(), active.end(),
                                      [&](const ActiveFile& f) {
                                        return f.path == decode_delta_path(p->s) &&
                                               (dv_unique_id(*rem).empty() ||
                                                f.dv_id == dv_unique_id(*rem));
                                      }),
                       active.end());
        }
      }
    }
  }
  if (active.empty() && prune_preds.empty()) {
    die("tabela em '" + dir + "' esta vazia (nenhum arquivo ativo no log)");
  }

  // Schema declarado no metaData (com as colunas de particao); sem metaData,
  // cai no legado de deduzir as colunas do 1o arquivo parquet.
  const std::vector<std::pair<std::string, std::string>> fields =
      schema_string_fields(current_schema_string(versions));

  // Filtro residual aplicado sobre a linha final (reidratada ou legado).
  auto passa_residual = [&](const Value& row) {
    for (const auto& [col, val] : residual_preds) {
      const Value* cell = row.map ? row.map->find(col) : nullptr;
      if (!pred_eq(cell ? *cell : Value::nulo(), val)) return false;
    }
    return true;
  };

  Value out = Value::tabela();
  std::vector<std::string> schema_cols;
  for (const ActiveFile& f : active) {
    Value chunk = parquet_read(delta_data_path(dir, f.path));
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
      die("arquivo '" + f.path + "' nao e uma tabela parquet");
    }
    std::uint64_t row_index = 0;
    for (Value& row : *chunk.list) {
      if (f.deleted_rows.find(row_index++) != f.deleted_rows.end()) continue;
      if (row.kind != ValueKind::Mapa || !row.map) die("linha de '" + f.path + "' nao e um mapa");
      if (!fields.empty()) {
        // Reidrata na ordem declarada: valor do parquet, senao partitionValues
        // convertido para o tipo do schema, senao nulo.
        Value m = Value::mapa();
        for (const auto& fld : fields) {
          if (const Value* cell = row.map->find(fld.first)) {
            m.map->set(fld.first, *cell);
            continue;
          }
          auto pv = std::find_if(f.partvals.begin(), f.partvals.end(),
                                 [&](const auto& kv) { return kv.first == fld.first; });
          if (pv == f.partvals.end()) {
            m.map->set(fld.first, Value::nulo());
          } else if (pv->second.kind == ValueKind::Nulo) {
            m.map->set(fld.first, Value::nulo());
          } else {
            m.map->set(fld.first, partition_rehydrate(pv->second.s, fld.second));
          }
        }
        for (const auto& kv : row.map->items) {
          if (!fields_contains(fields, kv.first)) {
            die("schema divergente em '" + f.path + "' (coluna '" + kv.first +
                "' fora do metaData)");
          }
        }
        if (passa_residual(m)) out.list->push_back(std::move(m));
      } else {
        if (schema_cols.empty()) {
          for (const auto& kv : row.map->items) schema_cols.push_back(kv.first);
        } else {
          if (row.map->items.size() != schema_cols.size()) {
            die("schema divergente em '" + f.path + "' (colunas diferentes da 1a versao)");
          }
          for (std::size_t k = 0; k < schema_cols.size(); ++k) {
            if (row.map->items[k].first != schema_cols[k]) {
              die("schema divergente em '" + f.path + "' (ordem/nome de colunas difere)");
            }
          }
        }
        if (passa_residual(row)) out.list->push_back(std::move(row));
      }
    }
  }
  return out;
}

Value delta_read_changes(const std::string& dir, long long de, long long ate) {
  if (de < 0) die("ler_delta_mudancas: versao inicial deve ser >= 0");
  const std::vector<std::string> all = list_delta_versions(dir + "/_delta_log");
  if (all.empty()) die("tabela em '" + dir + "' nao possui transaction log");
  const long long ultimo = versao_de_json(all.back());
  if (ate < 0) ate = ultimo;
  if (ate < de) die("ler_delta_mudancas: 'ate' deve ser >= 'de'");

  Value out = Value::tabela();
  for (const std::string& path : all) {
    const long long versao = versao_de_json(path);
    if (versao < de || versao > ate) continue;
    std::ifstream in(path);
    if (!in) die("nao foi possivel abrir '" + path + "'");
    std::vector<Value> cdc, adds, removes;
    std::int64_t timestamp = 0;
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
      if (const Value* ci = row.map->find("commitInfo");
          ci && ci->kind == ValueKind::Mapa && ci->map) {
        if (const Value* ts = ci->map->find("timestamp"); ts && ts->is_number()) {
          timestamp = static_cast<std::int64_t>(ts->as_number());
        }
      }
      if (const Value* a = row.map->find("cdc"); a && a->kind == ValueKind::Mapa && a->map) {
        cdc.push_back(*a);
      } else if (const Value* a = row.map->find("add");
                 a && a->kind == ValueKind::Mapa && a->map) {
        adds.push_back(*a);
      } else if (const Value* r = row.map->find("remove");
                 r && r->kind == ValueKind::Mapa && r->map) {
        removes.push_back(*r);
      }
    }

    std::vector<std::string> prefix;
    for (const std::string& candidate : all) {
      if (versao_de_json(candidate) <= versao) prefix.push_back(candidate);
    }
    const auto fields = schema_string_fields(current_schema_string(prefix));
    const auto part_cols = current_partition_columns(prefix);
    auto append_file = [&](const Value& action, const char* change) {
      const Value* p = action.map ? action.map->find("path") : nullptr;
      if (!p || p->kind != ValueKind::Texto) die("acao Delta sem path em '" + path + "'");
      Value chunk = parquet_read(delta_data_path(dir, p->s));
      if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
        die("arquivo CDC '" + p->s + "' nao e uma tabela parquet");
      }
      const std::set<std::uint64_t> dv_rows = read_deletion_vector(action, dir);
      std::uint64_t row_index = 0;
      for (Value& row : *chunk.list) {
        const bool marked = dv_rows.find(row_index++) != dv_rows.end();
        if ((std::string(change) == "delete") ? !marked : marked) continue;
        if (row.kind != ValueKind::Mapa || !row.map) continue;
        if (!fields.empty()) {
          for (const auto& fld : fields) {
            if (row.map->find(fld.first)) continue;
            const Value* pv = action.map ? action.map->find("partitionValues") : nullptr;
            const Value* cell = nullptr;
            if (pv && pv->kind == ValueKind::Mapa && pv->map) cell = pv->map->find(fld.first);
            if (cell && cell->kind == ValueKind::Texto &&
                std::find(part_cols.begin(), part_cols.end(), fld.first) != part_cols.end()) {
              row.map->set(fld.first, partition_rehydrate(cell->s, fld.second));
            } else {
              row.map->set(fld.first, Value::nulo());
            }
          }
        }
        if (std::string(change) != "cdc" || !row.map->find("_change_type")) {
          row.map->set("_change_type", Value::texto(change));
        }
        row.map->set("_commit_version", Value::inteiro(versao));
        row.map->set("_commit_timestamp", Value::inteiro(timestamp));
        out.list->push_back(std::move(row));
      }
    };
    if (!cdc.empty()) {
      for (const Value& a : cdc) append_file(a, "cdc");
    } else {
      auto data_change = [](const Value& action) {
        const Value* v = action.map ? action.map->find("dataChange") : nullptr;
        return !v || v->kind != ValueKind::Logico || v->b;
      };
      for (const Value& a : adds) {
        if (data_change(a)) append_file(a, "insert");
      }
      for (const Value& a : removes) {
        if (data_change(a)) append_file(a, "delete");
      }
    }
  }
  return out;
}

void delta_optimize(const std::string& dir) {
  const std::string log_dir = dir + "/_delta_log";
  const std::vector<std::string> versions = list_delta_versions(log_dir);
  if (versions.empty()) die("tabela nao existe em '" + dir + "' (use escrever_delta para criar)");
  const Value tabela = delta_read(dir, nullptr);
  const std::vector<std::string> part_cols = current_partition_columns(versions);
  // Reescrita por particao: delta_write agrupa as linhas e cria um parquet
  // compacto por grupo. Os arquivos anteriores permanecem orfaos ate vacuum.
  delta_write(dir, tabela, part_cols);
}

std::int64_t delta_vacuum(const std::string& dir) {
  const std::string log_dir = dir + "/_delta_log";
  const std::vector<std::string> versions = list_delta_versions(log_dir);
  const std::set<std::string> referenced = delta_logged_files(versions);
  std::vector<std::string> parquet;
  list_delta_parquets(dir, "", parquet);
  std::int64_t removed = 0;
  for (const std::string& rel : parquet) {
    if (referenced.find(rel) != referenced.end()) continue;
    if (std::remove((dir + "/" + rel).c_str()) == 0) ++removed;
  }
  return removed;
}

}  // namespace tilt::rt
