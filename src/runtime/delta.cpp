#include "runtime/delta.hpp"

#include "runtime/compat.hpp"

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
  if (tilt_mkdir(path) != 0 && errno != EEXIST) {
    die("nao foi possivel criar o diretorio '" + path + "'");
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

// Valor de particao como string (nome do diretorio hive-style). Erro claro
// em nulo e em texto com '/' (fase 25: sem __HIVE_DEFAULT_PARTITION__ nem
// escaping de caracteres especiais).
std::string partition_value_string(const Value& v, const std::string& col) {
  switch (v.kind) {
    case ValueKind::Nulo:
      die("valor nulo em coluna de particao '" + col +
          "' (fase 25: particao com nulo nao e suportada)");
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
            "' contem '/' (fase 25: caracteres especiais nao suportados)");
      }
      return v.s;
    default:
      die("coluna de particao '" + col + "' deve ser texto, inteiro, decimal ou logico");
  }
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

// Garante que a coluna de particao existe no schema da tabela (1a linha).
void ensure_partition_column(const Value& tabela, const std::string& col, const char* ctx) {
  const std::vector<std::string> cols = table_columns(tabela, ctx);
  if (std::find(cols.begin(), cols.end(), col) == cols.end()) {
    die(std::string(ctx) + ": coluna de particao '" + col + "' nao existe na tabela (colunas: " +
        join_cols(cols) + ")");
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

// Linha sem a coluna de particao: o parquet do Delta nao armazena as colunas
// de particao (o valor vive no diretorio/partitionValues).
Value strip_partition_column(const Value& row, const std::string& col) {
  Value m = Value::mapa();
  for (const auto& kv : row.map->items) {
    if (kv.first != col) m.map->set(kv.first, kv.second);
  }
  return m;
}

struct PartitionGroup {
  std::string valor;  // valor da coluna de particao como string
  Value rows;         // tabela sem a coluna de particao
};

// Agrupa as linhas pelo valor da coluna de particao, na ordem de 1a
// aparicao dos valores (define a ordem dos adds no log).
std::vector<PartitionGroup> partition_rows(const Value& tabela, const std::string& col) {
  std::vector<PartitionGroup> grupos;
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
    const Value* cell = row.map->find(col);
    const std::string valor = partition_value_string(cell ? *cell : Value::nulo(), col);
    auto it = std::find_if(grupos.begin(), grupos.end(),
                           [&](const PartitionGroup& g) { return g.valor == valor; });
    if (it == grupos.end()) {
      PartitionGroup g;
      g.valor = valor;
      g.rows = Value::tabela();
      it = grupos.insert(grupos.end(), std::move(g));
    }
    it->rows.list->push_back(strip_partition_column(row, col));
  }
  return grupos;
}

Value make_add(const std::string& rel_path,
               const std::vector<std::pair<std::string, std::string>>& partvals,
               std::int64_t size, std::int64_t ts) {
  Value add = Value::mapa();
  add.map->set("path", Value::texto(rel_path));
  Value pv = Value::mapa();
  for (const auto& kv : partvals) pv.map->set(kv.first, Value::texto(kv.second));
  add.map->set("partitionValues", std::move(pv));
  add.map->set("size", Value::inteiro(size));
  add.map->set("modificationTime", Value::inteiro(ts));
  add.map->set("dataChange", Value::logico(true));
  return add;
}

// Grava os parquet de uma escrita/anexo. Com col vazio, um unico arquivo na
// raiz (comportamento original); com col, um arquivo por valor de particao
// em <dir>/<col>=<valor>/part-NNNNN.parquet. Devolve os adds para o log.
std::vector<Value> write_partitions(const std::string& dir, const Value& tabela,
                                    const std::string& col) {
  std::vector<Value> adds;
  std::vector<PartitionGroup> grupos;
  if (col.empty()) {
    PartitionGroup g;
    g.rows = tabela;
    grupos.push_back(std::move(g));
  } else {
    grupos = partition_rows(tabela, col);
  }
  for (const PartitionGroup& g : grupos) {
    const std::string subdir = col.empty() ? dir : dir + "/" + col + "=" + g.valor;
    mkdir_if_missing(subdir);
    std::string nome;
    if (col.empty()) {
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
    const std::string rel = col.empty() ? nome : col + "=" + g.valor + "/" + nome;
    std::vector<std::pair<std::string, std::string>> pv;
    if (!col.empty()) pv.emplace_back(col, g.valor);
    adds.push_back(make_add(rel, pv, size, now_ms()));
  }
  return adds;
}

}  // namespace

void delta_write(const std::string& dir, const Value& tabela, const std::string& part_col) {
  const std::string schema = delta_schema_string(tabela, "escrever_delta");
  if (!part_col.empty()) ensure_partition_column(tabela, part_col, "escrever_delta");
  mkdir_if_missing(dir);
  const std::string log_dir = dir + "/_delta_log";
  mkdir_if_missing(log_dir);

  // 1a passada: tabela nova por escrita. Remove log anterior para nao
  // misturar versoes (sem merge/ACID concorrente).
  for (const std::string& old : list_delta_versions(log_dir)) {
    if (std::remove(old.c_str()) != 0) die("nao foi possivel limpar '" + old + "'");
  }

  const std::int64_t ts = now_ms();
  std::vector<Value> adds = write_partitions(dir, tabela, part_col);

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
  if (!part_col.empty()) part_cols.list->push_back(Value::texto(part_col));
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

void delta_append(const std::string& dir, const Value& tabela, const std::string& part_col_req) {
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
  for (const auto& old : cur_fields) {
    const std::string* ty = nullptr;
    for (const auto& nt : new_types) {
      if (nt.first == old.first) ty = &nt.second;
    }
    if (!ty) {
      die("anexar_delta: coluna '" + old.first +
          "' ausente na tabela anexada (evolucao de schema suporta apenas adicao de colunas)");
    }
    if (!ty->empty() && *ty != old.second) {
      die("anexar_delta: coluna '" + old.first + "' com tipo divergente (esperado " + old.second +
          "; recebido " + *ty +
          ") (evolucao de schema suporta apenas adicao de colunas)");
    }
  }
  for (const auto& nt : new_types) {
    if (!contains_col(cur_fields, nt.first)) added_cols.push_back(nt);
  }

  // schemaString estendido (colunas novas nullable no fim) para o commit.
  std::string new_schema;
  if (!added_cols.empty()) {
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

  // Particao: herda a da tabela existente; erro se explicita e divergente.
  const std::vector<std::string> existing = current_partition_columns(versions);
  std::string part_col;
  if (!existing.empty()) {
    part_col = existing.front();
    if (!part_col_req.empty() && part_col_req != part_col) {
      die("anexar_delta: tabela em '" + dir + "' ja e particionada por '" + part_col +
          "' (recebido particionar_por: '" + part_col_req + "')");
    }
    ensure_partition_column(tabela, part_col, "anexar_delta");
  } else if (!part_col_req.empty()) {
    die("anexar_delta: tabela em '" + dir + "' nao e particionada — recrie-a com escrever_delta " +
        "tabela, \"" + dir + "\", particionar_por: \"" + part_col_req + "\"");
  }

  // Grava os parquet ANTES de commitar; se der crash antes do rename, sobra
  // um parquet orfao que a leitura ignora (nao esta no log).
  const std::int64_t ts = now_ms();
  std::vector<Value> adds = write_partitions(dir, tabela, part_col);

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
  const std::string tmp_path = log_dir + "/.commit-" + std::to_string(::getpid()) + ".tmp";
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
}

Value delta_read(const std::string& dir) {
  const std::string log_dir = dir + "/_delta_log";
  const std::vector<std::string> versions = list_delta_versions(log_dir);

  // Arquivos ativos + seus partitionValues (Texto; Nulo = particao default
  // de tabelas externas, reidratada como nulo).
  struct ActiveFile {
    std::string path;
    std::vector<std::pair<std::string, Value>> partvals;
  };
  std::vector<ActiveFile> active;  // em ordem de add
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
        if (p && p->kind == ValueKind::Texto) {
          ActiveFile f;
          f.path = p->s;
          if (const Value* pv = add->map->find("partitionValues");
              pv && pv->kind == ValueKind::Mapa && pv->map) {
            for (const auto& kv : pv->map->items) {
              if (kv.second.kind == ValueKind::Texto || kv.second.kind == ValueKind::Nulo) {
                f.partvals.emplace_back(kv.first, kv.second);
              }
            }
          }
          active.push_back(std::move(f));
        }
      }
      if (const Value* rem = row.map->find("remove");
          rem && rem->kind == ValueKind::Mapa && rem->map) {
        const Value* p = rem->map->find("path");
        if (p && p->kind == ValueKind::Texto) {
          active.erase(std::remove_if(active.begin(), active.end(),
                                      [&](const ActiveFile& f) { return f.path == p->s; }),
                       active.end());
        }
      }
    }
  }
  if (active.empty()) die("tabela em '" + dir + "' esta vazia (nenhum arquivo ativo no log)");

  // Schema declarado no metaData (com as colunas de particao); sem metaData,
  // cai no legado de deduzir as colunas do 1o arquivo parquet.
  const std::vector<std::pair<std::string, std::string>> fields =
      schema_string_fields(current_schema_string(versions));

  Value out = Value::tabela();
  std::vector<std::string> schema_cols;
  for (const ActiveFile& f : active) {
    Value chunk = parquet_read(dir + "/" + f.path);
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
      die("arquivo '" + f.path + "' nao e uma tabela parquet");
    }
    for (Value& row : *chunk.list) {
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
        out.list->push_back(std::move(m));
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
        out.list->push_back(std::move(row));
      }
    }
  }
  return out;
}

}  // namespace tilt::rt
