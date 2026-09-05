#include "runtime/iceberg.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/json.hpp"
#include "runtime/parquet.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("iceberg: " + m); }

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

std::string new_uuid() {
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

std::int64_t new_snapshot_id() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  return static_cast<std::int64_t>(gen() & 0x7FFFFFFFFFFFFFFFULL);
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) die("nao foi possivel abrir '" + path + "'");
  std::string out;
  in.seekg(0, std::ios::end);
  out.resize(static_cast<std::size_t>(in.tellg()));
  in.seekg(0, std::ios::beg);
  if (!out.empty()) in.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (!in && !out.empty()) die("falha ao ler '" + path + "'");
  return out;
}

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

// ---------------------------------------------------------------------------
// Avro OCF (Object Container File) — writer e reader de 1a passada.
// Codec "null" apenas; tipos: null/boolean/int/long/float/double/string/bytes
// /record/array/map (map com chaves string) e uniao ["null", T] com null
// sempre na posicao 0. O schema vem como JSON no header (avro.schema) e e
// decodificado com o json_parse do projeto.
// ---------------------------------------------------------------------------

void put_u32(std::string& out, std::uint32_t v) {
  for (int k = 0; k < 4; ++k) out += static_cast<char>((v >> (8 * k)) & 0xFF);
}

void put_u64(std::string& out, std::uint64_t v) {
  for (int k = 0; k < 8; ++k) out += static_cast<char>((v >> (8 * k)) & 0xFF);
}

void put_varint(std::string& out, std::uint64_t v) {
  while (v & ~0x7FULL) {
    out += static_cast<char>((v & 0x7F) | 0x80);
    v >>= 7;
  }
  out += static_cast<char>(v);
}

void put_long(std::string& out, std::int64_t v) {
  put_varint(out, (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63));
}

class AvroDecoder {
 public:
  AvroDecoder(const std::string& data, std::string ctx)
      : d_(data), ctx_(std::move(ctx)) {}

  [[noreturn]] void die(const std::string& m) const {
    throw std::runtime_error("iceberg: avro invalido em '" + ctx_ + "': " + m);
  }

  std::uint8_t u8() {
    ensure(1);
    return static_cast<std::uint8_t>(d_[pos_++]);
  }

  void bytes(void* dst, std::size_t n) {
    ensure(n);
    std::memcpy(dst, d_.data() + pos_, n);
    pos_ += n;
  }

  std::uint64_t varint() {
    std::uint64_t v = 0;
    int shift = 0;
    while (true) {
      const std::uint8_t b = u8();
      v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
      shift += 7;
      if (shift > 63) die("varint longo demais");
    }
  }

  std::int64_t zig() {
    const std::uint64_t v = varint();
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1);
  }

  std::string str() {
    const std::int64_t n = zig();
    if (n < 0) die("comprimento negativo");
    ensure(static_cast<std::size_t>(n));
    std::string out = d_.substr(pos_, static_cast<std::size_t>(n));
    pos_ += static_cast<std::size_t>(n);
    return out;
  }

  bool done() const { return pos_ == d_.size(); }

 private:
  void ensure(std::size_t n) const {
    if (pos_ + n > d_.size()) throw std::runtime_error("iceberg: avro invalido em '" + ctx_ +
                                                        "': arquivo truncado");
  }
  const std::string& d_;
  std::size_t pos_ = 0;
  std::string ctx_;
};

const Value* map_find(const Value& m, const char* key) {
  return m.kind == ValueKind::Mapa && m.map ? m.map->find(key) : nullptr;
}

bool is_nullable_union(const Value& schema) {
  if (schema.kind != ValueKind::Lista || !schema.list || schema.list->empty()) return false;
  const Value& first = schema.list->front();
  return first.kind == ValueKind::Texto && first.s == "null";
}

// branch efetiva de uma uniao: indice 0 (null) ou 1 ("null" sempre em 0)
const Value& union_branch(const Value& schema, std::int64_t idx) {
  if (schema.kind != ValueKind::Lista || !schema.list || idx < 0 ||
      static_cast<std::size_t>(idx) >= schema.list->size()) {
    throw std::runtime_error("iceberg: avro: indice de uniao invalido");
  }
  return (*schema.list)[static_cast<std::size_t>(idx)];
}

void avro_encode(std::string& out, const Value& schema, const Value& v) {
  if (schema.kind == ValueKind::Lista) {  // uniao
    if (!schema.list || schema.list->empty()) {
      throw std::runtime_error("iceberg: avro: uniao vazia no schema");
    }
    if (v.kind == ValueKind::Nulo) {
      if (!is_nullable_union(schema)) {
        throw std::runtime_error("iceberg: avro: null fora de uniao anulavel");
      }
      put_varint(out, 0);
      return;
    }
    put_varint(out, 1);
    avro_encode(out, union_branch(schema, 1), v);
    return;
  }
  if (schema.kind != ValueKind::Mapa && schema.kind != ValueKind::Texto) {
    throw std::runtime_error("iceberg: avro: tipo de schema invalido");
  }
  const std::string ty = schema.kind == ValueKind::Texto
                             ? schema.s
                             : ([&]() {
                                 if (const Value* t = map_find(schema, "type");
                                     t && t->kind == ValueKind::Texto) {
                                   return t->s;
                                 }
                                 return std::string();
                               })();
  if (ty.empty()) throw std::runtime_error("iceberg: avro: tipo de schema invalido");
  if (ty == "null") {
    return;
  }
  if (ty == "boolean") {
    if (v.kind != ValueKind::Logico) throw std::runtime_error("iceberg: avro: esperado boolean");
    out += v.b ? '\1' : '\0';
    return;
  }
  if (ty == "int" || ty == "long") {
    if (v.kind != ValueKind::Inteiro) throw std::runtime_error("iceberg: avro: esperado " + ty);
    put_long(out, v.i);
    return;
  }
  if (ty == "float") {
    const float f = v.kind == ValueKind::Inteiro ? static_cast<float>(v.i)
                                                 : static_cast<float>(v.as_number());
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
    return;
  }
  if (ty == "double") {
    const double dbl = v.as_number();
    std::uint64_t bits;
    std::memcpy(&bits, &dbl, 8);
    put_u64(out, bits);
    return;
  }
  if (ty == "string" || ty == "bytes") {
    if (v.kind != ValueKind::Texto) throw std::runtime_error("iceberg: avro: esperado " + ty);
    put_long(out, static_cast<std::int64_t>(v.s.size()));
    out += v.s;
    return;
  }
  if (ty == "record") {
    const Value* fields = map_find(schema, "fields");
    if (!fields || fields->kind != ValueKind::Lista) {
      throw std::runtime_error("iceberg: avro: record sem fields");
    }
    if (v.kind != ValueKind::Mapa || !v.map) throw std::runtime_error("iceberg: avro: esperado record");
    for (const Value& f : *fields->list) {
      const Value* name = map_find(f, "name");
      const Value* fschema = map_find(f, "type");
      if (!name || !fschema) throw std::runtime_error("iceberg: avro: field sem nome/tipo");
      const Value* fv = v.map->find(name->s);
      avro_encode(out, *fschema, fv ? *fv : Value::nulo());
    }
    return;
  }
  if (ty == "array") {
    const Value* items = map_find(schema, "items");
    if (!items) throw std::runtime_error("iceberg: avro: array sem items");
    if (v.kind != ValueKind::Lista) throw std::runtime_error("iceberg: avro: esperado array");
    if (!v.list->empty()) {
      put_long(out, static_cast<std::int64_t>(v.list->size()));
      for (const Value& e : *v.list) avro_encode(out, *items, e);
    }
    put_long(out, 0);
    return;
  }
  if (ty == "map") {
    const Value* values = map_find(schema, "values");
    if (!values) throw std::runtime_error("iceberg: avro: map sem values");
    if (v.kind != ValueKind::Mapa || !v.map) throw std::runtime_error("iceberg: avro: esperado map");
    if (!v.map->items.empty()) {
      put_long(out, static_cast<std::int64_t>(v.map->items.size()));
      for (const auto& kv : v.map->items) {
        put_long(out, static_cast<std::int64_t>(kv.first.size()));
        out += kv.first;
        avro_encode(out, *values, kv.second);
      }
    }
    put_long(out, 0);
    return;
  }
  throw std::runtime_error("iceberg: avro: tipo nao suportado '" + ty + "'");
}

Value avro_decode(AvroDecoder& dec, const Value& schema) {
  if (schema.kind == ValueKind::Lista) {  // uniao
    const std::int64_t idx = static_cast<std::int64_t>(dec.varint());
    if (idx == 0) return Value::nulo();
    return avro_decode(dec, union_branch(schema, idx));
  }
  if (schema.kind != ValueKind::Mapa && schema.kind != ValueKind::Texto) {
    throw std::runtime_error("iceberg: avro: tipo de schema invalido");
  }
  const std::string ty = schema.kind == ValueKind::Texto
                             ? schema.s
                             : ([&]() {
                                 if (const Value* t = map_find(schema, "type");
                                     t && t->kind == ValueKind::Texto) {
                                   return t->s;
                                 }
                                 return std::string();
                               })();
  if (ty.empty()) throw std::runtime_error("iceberg: avro: tipo de schema invalido");
  if (ty == "null") return Value::nulo();
  if (ty == "boolean") return Value::logico(dec.u8() != 0);
  if (ty == "int" || ty == "long") return Value::inteiro(dec.zig());
  if (ty == "float") {
    std::uint32_t bits;
    dec.bytes(&bits, 4);
    float f;
    std::memcpy(&f, &bits, 4);
    return Value::decimal(static_cast<double>(f));
  }
  if (ty == "double") {
    std::uint64_t bits;
    dec.bytes(&bits, 8);
    double dbl;
    std::memcpy(&dbl, &bits, 8);
    return Value::decimal(dbl);
  }
  if (ty == "string" || ty == "bytes") return Value::texto(dec.str());
  if (ty == "record") {
    const Value* fields = map_find(schema, "fields");
    if (!fields || fields->kind != ValueKind::Lista) {
      throw std::runtime_error("iceberg: avro: record sem fields");
    }
    Value out = Value::mapa();
    for (const Value& f : *fields->list) {
      const Value* name = map_find(f, "name");
      const Value* fschema = map_find(f, "type");
      if (!name || !fschema) throw std::runtime_error("iceberg: avro: field sem nome/tipo");
      out.map->set(name->s, avro_decode(dec, *fschema));
    }
    return out;
  }
  if (ty == "array") {
    const Value* items = map_find(schema, "items");
    if (!items) throw std::runtime_error("iceberg: avro: array sem items");
    Value out = Value::lista();
    while (true) {
      const std::int64_t n = dec.zig();
      if (n == 0) break;
      if (n < 0) {
        const std::int64_t blocks = dec.zig();  // tamanho em bytes; nao usado
        (void)blocks;
        for (std::int64_t k = 0; k < -n; ++k) out.list->push_back(avro_decode(dec, *items));
      } else {
        for (std::int64_t k = 0; k < n; ++k) out.list->push_back(avro_decode(dec, *items));
      }
    }
    return out;
  }
  if (ty == "map") {
    const Value* values = map_find(schema, "values");
    if (!values) throw std::runtime_error("iceberg: avro: map sem values");
    Value out = Value::mapa();
    while (true) {
      const std::int64_t n = dec.zig();
      if (n == 0) break;
      std::int64_t count = n;
      if (n < 0) {
        dec.zig();
        count = -n;
      }
      for (std::int64_t k = 0; k < count; ++k) {
        const std::string key = dec.str();
        out.map->set(key, avro_decode(dec, *values));
      }
    }
    return out;
  }
  throw std::runtime_error("iceberg: avro: tipo nao suportado '" + ty + "'");
}

// OCF writer: um unico bloco com todos os registros, sync fixo.
std::string ocf_write(const Value& schema, const std::string& schema_json,
                      const std::vector<Value>& records,
                      const std::vector<std::pair<std::string, std::string>>& extra_meta) {
  static const char kSync[] = "\x77\xB6\xD2\xD1\x6E\xA6\x86\x79"
                              "\x98\x72\x8A\x66\x88\xA8\x44\x56";
  std::string body;
  for (const Value& r : records) avro_encode(body, schema, r);

  std::string out = "Obj\x01";
  // metadata map<string, bytes>: um bloco so
  std::vector<std::pair<std::string, std::string>> meta;
  meta.emplace_back("avro.schema", schema_json);
  meta.emplace_back("avro.codec", "null");
  meta.insert(meta.end(), extra_meta.begin(), extra_meta.end());
  put_long(out, static_cast<std::int64_t>(meta.size()));
  for (const auto& kv : meta) {
    put_long(out, static_cast<std::int64_t>(kv.first.size()));
    out += kv.first;
    put_long(out, static_cast<std::int64_t>(kv.second.size()));
    out += kv.second;
  }
  put_long(out, 0);
  out.append(kSync, 16);
  if (!records.empty()) {
    put_long(out, static_cast<std::int64_t>(records.size()));
    put_long(out, static_cast<std::int64_t>(body.size()));
    out += body;
    out.append(kSync, 16);
  }
  return out;
}

// OCF reader: le header + blocos e devolve (schema, registros).
std::pair<Value, std::vector<Value>> ocf_read(const std::string& path) {
  const std::string data = read_file(path);
  AvroDecoder dec(data, path);
  char magic[4];
  dec.bytes(magic, 4);
  if (std::memcmp(magic, "Obj\x01", 4) != 0) die("'" + path + "' nao e um Avro OCF");

  // metadata map<string, bytes>
  std::string schema_json;
  std::string codec = "null";
  while (true) {
    const std::int64_t n = dec.zig();
    if (n == 0) break;
    std::int64_t count = n;
    if (n < 0) {
      dec.zig();
      count = -n;
    }
    for (std::int64_t k = 0; k < count; ++k) {
      const std::string key = dec.str();
      const std::string val = dec.str();
      if (key == "avro.schema") schema_json = val;
      if (key == "avro.codec") codec = val;
    }
  }
  if (schema_json.empty()) die("'" + path + "' sem avro.schema no header");
  if (codec != "null") die("codec avro '" + codec + "' nao suportado (somente null)");

  char sync[16];
  dec.bytes(sync, 16);

  Value schema;
  try {
    schema = json_parse(schema_json);
  } catch (const std::exception& e) {
    die("avro.schema invalido em '" + path + "': " + e.what());
  }

  std::vector<Value> records;
  while (!dec.done()) {
    const std::int64_t count = dec.zig();
    const std::int64_t size = dec.zig();
    if (count < 0 || size < 0 || count > (1LL << 40) || size > (1LL << 40)) {
      die("bloco avro invalido em '" + path + "'");
    }
    std::string raw;
    raw.reserve(static_cast<std::size_t>(size));
    for (std::int64_t k = 0; k < size; ++k) raw += static_cast<char>(dec.u8());
    AvroDecoder bdec(raw, path + " (bloco)");
    for (std::int64_t k = 0; k < count; ++k) records.push_back(avro_decode(bdec, schema));
    if (!bdec.done()) die("registros avro maiores que o bloco em '" + path + "'");
    char tail[16];
    dec.bytes(tail, 16);
    if (std::memcmp(tail, sync, 16) != 0) die("sync marker invalido em '" + path + "'");
  }
  return {std::move(schema), std::move(records)};
}

// ---------------------------------------------------------------------------
// Schemas Avro do Iceberg (entrada v2 do manifest, manifest list v2)
// ---------------------------------------------------------------------------

const char* kManifestEntrySchema = R"AVRO({"type":"record","name":"manifest_entry","fields":[
{"name":"status","type":"int"},
{"name":"snapshot_id","type":["null","long"],"default":null},
{"name":"sequence_number","type":["null","long"],"default":null},
{"name":"file_sequence_number","type":["null","long"],"default":null},
{"name":"data_file","type":{"type":"record","name":"data_file","fields":[
{"name":"content","type":"int"},
{"name":"file_path","type":"string"},
{"name":"file_format","type":"string"},
{"name":"partition","type":{"type":"record","name":"partition","fields":[]}},
{"name":"record_count","type":"long"},
{"name":"file_size_in_bytes","type":"long"},
{"name":"column_sizes","type":["null",{"type":"map","values":"long"}],"default":null},
{"name":"value_counts","type":["null",{"type":"map","values":"long"}],"default":null},
{"name":"null_value_counts","type":["null",{"type":"map","values":"long"}],"default":null},
{"name":"lower_bounds","type":["null",{"type":"map","values":"bytes"}],"default":null},
{"name":"upper_bounds","type":["null",{"type":"map","values":"bytes"}],"default":null},
{"name":"key_metadata","type":["null","bytes"],"default":null},
{"name":"split_offsets","type":["null",{"type":"array","items":"long"}],"default":null},
{"name":"equality_ids","type":["null",{"type":"array","items":"int"}],"default":null},
{"name":"sort_order_id","type":["null","int"],"default":null}
]}}]})AVRO";

const char* kManifestListSchema = R"AVRO({"type":"record","name":"manifest_file","fields":[
{"name":"manifest_path","type":"string"},
{"name":"manifest_length","type":"long"},
{"name":"partition_spec_id","type":"int"},
{"name":"added_snapshot_id","type":"long"},
{"name":"added_files_count","type":["null","int"],"default":null},
{"name":"existing_files_count","type":["null","int"],"default":null},
{"name":"deleted_files_count","type":["null","int"],"default":null},
{"name":"partitions","type":["null",{"type":"array","items":{"type":"record","name":"partition_field_summary","fields":[
{"name":"contains_null","type":"boolean"},
{"name":"contains_nan","type":["null","boolean"],"default":null},
{"name":"lower_bound","type":["null","bytes"],"default":null},
{"name":"upper_bound","type":["null","bytes"],"default":null}
]}}],"default":null},
{"name":"has_added_files","type":["null","boolean"],"default":null},
{"name":"has_existing_files","type":["null","boolean"],"default":null},
{"name":"has_deleted_files","type":["null","boolean"],"default":null}
]})AVRO";

// ---------------------------------------------------------------------------
// Tabela / schema tilt -> Iceberg
// ---------------------------------------------------------------------------

std::string iceberg_type_name(const Value& v) {
  switch (v.kind) {
    case ValueKind::Logico: return "boolean";
    case ValueKind::Inteiro: return "long";
    case ValueKind::Decimal: return "double";
    case ValueKind::Texto: return "string";
    default: return "string";
  }
}

struct Column {
  std::string name;
  std::string type;  // iceberg
  Value sample;      // valor da 1a linha (deduzir tipo)
};

std::vector<Column> table_columns(const Value& tabela, const char* ctx) {
  if (tabela.kind != ValueKind::Lista && tabela.kind != ValueKind::Tabela) {
    die(std::string(ctx) + " espera uma tabela (lista de mapas)");
  }
  if (tabela.list->empty()) die(std::string(ctx) + ": tabela vazia (sem schema deduzivel)");
  const Value& first = tabela.list->front();
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");
  std::vector<Column> cols;
  for (const auto& kv : first.map->items) {
    cols.push_back({kv.first, iceberg_type_name(kv.second), kv.second});
  }
  return cols;
}

std::string schema_json_fields(const std::vector<Column>& cols) {
  std::string out = "[";
  for (std::size_t k = 0; k < cols.size(); ++k) {
    if (k) out += ',';
    out += "{\"id\":" + std::to_string(k + 1) + ",\"name\":\"" + json_escape(cols[k].name) +
           "\",\"required\":true,\"type\":\"" + cols[k].type + "\"}";
  }
  out += ']';
  return out;
}

// ---------------------------------------------------------------------------
// Metadata (v<N>-<uuid>.metadata.json)
// ---------------------------------------------------------------------------

std::vector<std::string> list_metadata_files(const std::string& meta_dir) {
  DIR* d = ::opendir(meta_dir.c_str());
  if (!d) return {};
  std::vector<std::pair<std::string, std::string>> found;
  while (dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > 14 && name.compare(name.size() - 14, 14, ".metadata.json") == 0) {
      found.emplace_back(name, meta_dir + "/" + name);
    }
  }
  ::closedir(d);
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  out.reserve(found.size());
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

struct Snapshot {
  std::int64_t id = 0;
  std::int64_t ts = 0;
  std::string operation;
  std::string manifest_list;
  std::int64_t parent = -1;
  bool has_parent = false;
};

struct TableMeta {
  std::int64_t last_updated = 0;
  std::int64_t current_snapshot = -1;
  std::vector<Column> schema_cols;  // do current-schema-id
  std::vector<Snapshot> snapshots;
  std::vector<std::pair<std::int64_t, std::int64_t>> snapshot_log;  // (ts, id)
  std::int64_t version = -1;
  std::string location;
};

Value parse_metadata(const std::string& path, TableMeta& out) {
  Value md;
  try {
    md = json_parse(read_file(path));
  } catch (const std::exception& e) {
    die("metadata invalido em '" + path + "': " + e.what());
  }
  if (md.kind != ValueKind::Mapa || !md.map) die("'" + path + "' nao e um metadata json");
  if (const Value* fv = map_find(md, "format-version"); !fv || fv->kind != ValueKind::Inteiro) {
    die("'" + path + "' sem format-version");
  }
  if (const Value* loc = map_find(md, "location"); loc && loc->kind == ValueKind::Texto) {
    out.location = loc->s;
  }
  if (const Value* lu = map_find(md, "last-updated-ms"); lu && lu->kind == ValueKind::Inteiro) {
    out.last_updated = lu->i;
  }
  if (const Value* cs = map_find(md, "current-snapshot-id"); cs && cs->kind == ValueKind::Inteiro) {
    out.current_snapshot = cs->i;
  }

  // schema corrente
  std::int64_t current_schema = 0;
  if (const Value* c = map_find(md, "current-schema-id"); c && c->kind == ValueKind::Inteiro) {
    current_schema = c->i;
  }
  if (const Value* schemas = map_find(md, "schemas"); schemas && schemas->kind == ValueKind::Lista) {
    for (const Value& s : *schemas->list) {
      const Value* sid = map_find(s, "schema-id");
      if (!sid || sid->kind != ValueKind::Inteiro || sid->i != current_schema) continue;
      if (const Value* fields = map_find(s, "fields"); fields && fields->kind == ValueKind::Lista) {
        for (const Value& f : *fields->list) {
          const Value* name = map_find(f, "name");
          const Value* type = map_find(f, "type");
          Column c;
          c.name = name && name->kind == ValueKind::Texto ? name->s : "";
          c.type = type && type->kind == ValueKind::Texto ? type->s : "string";
          out.schema_cols.push_back(std::move(c));
        }
      }
    }
  }

  if (const Value* snaps = map_find(md, "snapshots"); snaps && snaps->kind == ValueKind::Lista) {
    for (const Value& s : *snaps->list) {
      Snapshot snap;
      const Value* id = map_find(s, "snapshot-id");
      const Value* ts = map_find(s, "timestamp-ms");
      const Value* ml = map_find(s, "manifest-list");
      const Value* sum = map_find(s, "summary");
      const Value* parent = map_find(s, "parent-snapshot-id");
      if (!id || id->kind != ValueKind::Inteiro || !ml || ml->kind != ValueKind::Texto) {
        die("snapshot incompleto em '" + path + "'");
      }
      snap.id = id->i;
      snap.ts = ts && ts->kind == ValueKind::Inteiro ? ts->i : 0;
      snap.manifest_list = ml->s;
      if (sum && sum->kind == ValueKind::Mapa) {
        if (const Value* op = map_find(*sum, "operation"); op && op->kind == ValueKind::Texto) {
          snap.operation = op->s;
        }
      }
      if (parent && parent->kind == ValueKind::Inteiro) {
        snap.parent = parent->i;
        snap.has_parent = true;
      }
      out.snapshots.push_back(std::move(snap));
    }
  }

  if (const Value* log = map_find(md, "snapshot-log"); log && log->kind == ValueKind::Lista) {
    for (const Value& e : *log->list) {
      const Value* ts = map_find(e, "timestamp-ms");
      const Value* id = map_find(e, "snapshot-id");
      if (ts && ts->kind == ValueKind::Inteiro && id && id->kind == ValueKind::Inteiro) {
        out.snapshot_log.emplace_back(ts->i, id->i);
      }
    }
  }

  // versao pelo nome do arquivo v<N>-<uuid>.metadata.json
  const std::string base = path.substr(path.find_last_of('/') + 1);
  if (base.size() > 1 && base[0] == 'v') {
    try {
      out.version = std::stoll(base.substr(1));
    } catch (const std::exception&) {
      out.version = -1;
    }
  }
  return md;
}

std::string columns_desc(const std::vector<Column>& cols) {
  std::string out = "[";
  for (std::size_t k = 0; k < cols.size(); ++k) {
    if (k) out += ", ";
    out += cols[k].name + ": " + cols[k].type;
  }
  out += ']';
  return out;
}

// le o metadata mais recente (maior versao)
std::string latest_metadata_path(const std::string& dir, TableMeta& meta) {
  const std::string meta_dir = dir + "/metadata";
  const std::vector<std::string> files = list_metadata_files(meta_dir);
  if (files.empty()) {
    die("tabela nao existe em '" + dir + "' (use escrever_iceberg para criar)");
  }
  parse_metadata(files.back(), meta);
  return files.back();
}

// ---------------------------------------------------------------------------
// Manifests
// ---------------------------------------------------------------------------

Value make_manifest_entry(int status, std::int64_t snapshot_id, const std::string& file_path,
                          std::int64_t record_count, std::int64_t file_size) {
  Value e = Value::mapa();
  e.map->set("status", Value::inteiro(status));
  e.map->set("snapshot_id", Value::inteiro(snapshot_id));
  e.map->set("sequence_number", Value::nulo());
  e.map->set("file_sequence_number", Value::nulo());
  Value df = Value::mapa();
  df.map->set("content", Value::inteiro(0));
  df.map->set("file_path", Value::texto(file_path));
  df.map->set("file_format", Value::texto("PARQUET"));
  df.map->set("partition", Value::mapa());
  df.map->set("record_count", Value::inteiro(record_count));
  df.map->set("file_size_in_bytes", Value::inteiro(file_size));
  df.map->set("column_sizes", Value::nulo());
  df.map->set("value_counts", Value::nulo());
  df.map->set("null_value_counts", Value::nulo());
  df.map->set("lower_bounds", Value::nulo());
  df.map->set("upper_bounds", Value::nulo());
  df.map->set("key_metadata", Value::nulo());
  df.map->set("split_offsets", Value::nulo());
  df.map->set("equality_ids", Value::nulo());
  df.map->set("sort_order_id", Value::nulo());
  e.map->set("data_file", std::move(df));
  return e;
}

struct FileInfo {
  std::string path;  // com file://
  std::int64_t records = 0;
  std::int64_t size = 0;
};

std::string write_manifest(const std::string& meta_dir,
                           const std::vector<std::pair<int, std::string>>& changes,
                           std::int64_t snapshot_id, const std::vector<FileInfo>& files) {
  Value schema;
  try {
    schema = json_parse(kManifestEntrySchema);
  } catch (const std::exception& e) {
    die("schema interno do manifest invalido: " + std::string(e.what()));
  }
  std::vector<Value> records;
  for (const auto& [status, path] : changes) {
    const FileInfo* info = nullptr;
    for (const FileInfo& f : files) {
      if (f.path == path) info = &f;
    }
    records.push_back(make_manifest_entry(status, snapshot_id, path, info ? info->records : 0,
                                          info ? info->size : 0));
  }
  const std::string name = new_uuid() + "-m0.avro";
  const std::string path = meta_dir + "/" + name;
  const std::string bytes = ocf_write(schema, kManifestEntrySchema, records, {});
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) die("nao foi possivel gravar '" + path + "'");
  out << bytes;
  if (!out) die("falha ao gravar '" + path + "'");
  return name;
}

std::string write_manifest_list(const std::string& meta_dir, const std::string& manifest_name,
                                std::int64_t snapshot_id, int added, int existing, int deleted) {
  Value schema;
  try {
    schema = json_parse(kManifestListSchema);
  } catch (const std::exception& e) {
    die("schema interno do manifest list invalido: " + std::string(e.what()));
  }
  const std::string manifest_path = meta_dir + "/" + manifest_name;
  const std::int64_t manifest_length = file_size(manifest_path);
  if (manifest_length < 0) die("manifest ausente em '" + manifest_path + "'");

  Value rec = Value::mapa();
  rec.map->set("manifest_path", Value::texto("file://" + manifest_path));
  rec.map->set("manifest_length", Value::inteiro(manifest_length));
  rec.map->set("partition_spec_id", Value::inteiro(0));
  rec.map->set("added_snapshot_id", Value::inteiro(snapshot_id));
  rec.map->set("added_files_count", Value::inteiro(added));
  rec.map->set("existing_files_count", Value::inteiro(existing));
  rec.map->set("deleted_files_count", Value::inteiro(deleted));
  rec.map->set("partitions", Value::lista());
  rec.map->set("has_added_files", Value::logico(added > 0));
  rec.map->set("has_existing_files", Value::logico(existing > 0));
  rec.map->set("has_deleted_files", Value::logico(deleted > 0));

  const std::string name = "snap-" + std::to_string(snapshot_id) + "-0-" + new_uuid() + ".avro";
  const std::string path = meta_dir + "/" + name;
  std::vector<std::pair<std::string, std::string>> meta = {{"format-version", "2"}};
  const std::string bytes = ocf_write(schema, kManifestListSchema, {rec}, meta);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) die("nao foi possivel gravar '" + path + "'");
  out << bytes;
  if (!out) die("falha ao gravar '" + path + "'");
  return "file://" + path;
}

// ---------------------------------------------------------------------------
// Metadata JSON (format-version 2)
// ---------------------------------------------------------------------------

std::string build_metadata_json(const std::string& dir, const std::vector<Column>& cols,
                                const std::vector<Snapshot>& snapshots,
                                std::vector<std::pair<std::int64_t, std::int64_t>> snapshot_log,
                                std::int64_t current_snapshot, std::int64_t last_updated) {
  std::string out = "{\n";
  out += "  \"format-version\": 2,\n";
  out += "  \"location\": \"" + json_escape(dir) + "\",\n";
  out += "  \"last-updated-ms\": " + std::to_string(last_updated) + ",\n";
  out += "  \"last-column-id\": " + std::to_string(cols.size()) + ",\n";
  out += "  \"schemas\": [\n    {\n      \"schema-id\": 0,\n      \"type\": \"struct\",\n";
  out += "      \"fields\": " + schema_json_fields(cols) + "\n    }\n  ],\n";
  out += "  \"current-schema-id\": 0,\n";
  out += "  \"partition-specs\": [\n    { \"spec-id\": 0, \"fields\": [] }\n  ],\n";
  out += "  \"default-spec-id\": 0,\n";
  out += "  \"current-snapshot-id\": " + std::to_string(current_snapshot) + ",\n";
  out += "  \"snapshots\": [";
  for (std::size_t k = 0; k < snapshots.size(); ++k) {
    const Snapshot& s = snapshots[k];
    out += (k ? ",\n" : "\n");
    out += "    {\n";
    out += "      \"snapshot-id\": " + std::to_string(s.id) + ",\n";
    out += "      \"timestamp-ms\": " + std::to_string(s.ts) + ",\n";
    out += "      \"summary\": { \"operation\": \"" + s.operation + "\" },\n";
    if (s.has_parent) {
      out += "      \"parent-snapshot-id\": " + std::to_string(s.parent) + ",\n";
    }
    out += "      \"manifest-list\": \"" + json_escape(s.manifest_list) + "\"\n";
    out += "    }";
  }
  out += "\n  ],\n";
  out += "  \"snapshot-log\": [";
  for (std::size_t k = 0; k < snapshot_log.size(); ++k) {
    out += (k ? ",\n" : "\n");
    out += "    { \"timestamp-ms\": " + std::to_string(snapshot_log[k].first) +
           ", \"snapshot-id\": " + std::to_string(snapshot_log[k].second) + " }";
  }
  out += "\n  ],\n";
  out += "  \"metadata-log\": []\n";
  out += "}\n";
  return out;
}

void commit_metadata(const std::string& dir, std::int64_t version, const std::string& content) {
  const std::string meta_dir = dir + "/metadata";
  const std::string final_path =
      meta_dir + "/v" + std::to_string(version) + "-" + new_uuid() + ".metadata.json";
  const std::string tmp_path =
      meta_dir + "/.commit-" + std::to_string(::getpid()) + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out) die("nao foi possivel gravar '" + tmp_path + "'");
    out << content;
    out.flush();
    if (!out) die("falha ao gravar '" + tmp_path + "'");
  }
  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    ::unlink(tmp_path.c_str());
    die("falha ao commitar o metadata em '" + final_path + "'");
  }
}

// ---------------------------------------------------------------------------
// Leitura: resolve o snapshot atual percorrendo a cadeia de pais
// ---------------------------------------------------------------------------

const Snapshot* find_snapshot(const TableMeta& meta, std::int64_t id) {
  for (const Snapshot& s : meta.snapshots) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

// coleta (status, file_path) dos manifests de um snapshot
void collect_manifest_entries(const Snapshot& snap, std::vector<std::pair<int, std::string>>& out) {
  std::string list_path = snap.manifest_list;
  if (list_path.rfind("file://", 0) == 0) list_path = list_path.substr(7);
  auto [list_schema, manifests] = ocf_read(list_path);
  (void)list_schema;
  for (const Value& m : manifests) {
    const Value* mp = map_find(m, "manifest_path");
    if (!mp || mp->kind != ValueKind::Texto) die("manifest_path ausente no manifest list");
    std::string manifest_path = mp->s;
    if (manifest_path.rfind("file://", 0) == 0) manifest_path = manifest_path.substr(7);
    auto [m_schema, entries] = ocf_read(manifest_path);
    (void)m_schema;
    for (const Value& e : entries) {
      const Value* status = map_find(e, "status");
      const Value* df = map_find(e, "data_file");
      if (!status || status->kind != ValueKind::Inteiro || !df || df->kind != ValueKind::Mapa) {
        die("entrada de manifest invalida em '" + manifest_path + "'");
      }
      const Value* fp = map_find(*df, "file_path");
      if (!fp || fp->kind != ValueKind::Texto) die("data_file sem file_path em '" + manifest_path + "'");
      out.emplace_back(static_cast<int>(status->i), fp->s);
    }
  }
}

// arquivos ativos no snapshot corrente: percorre a cadeia de pais do mais
// antigo para o mais novo aplicando adds menos removes (remove = status 2).
std::vector<std::string> resolve_active_files(const TableMeta& meta) {
  if (meta.current_snapshot < 0) die("tabela em '" + meta.location + "' esta vazia (sem snapshots)");

  // cadeia do atual ate a raiz
  std::vector<const Snapshot*> chain;
  std::int64_t id = meta.current_snapshot;
  for (int guard = 0; guard < 100000; ++guard) {
    const Snapshot* s = find_snapshot(meta, id);
    if (!s) die("snapshot " + std::to_string(id) + " nao encontrado no metadata");
    chain.push_back(s);
    if (!s->has_parent || s->parent < 0) break;
    id = s->parent;
  }
  if (chain.empty() || (chain.back()->has_parent && chain.back()->parent >= 0)) {
    die("cadeia de snapshots quebrada em '" + meta.location + "'");
  }

  std::vector<std::string> active;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    std::vector<std::pair<int, std::string>> entries;
    collect_manifest_entries(**it, entries);
    for (const auto& [status, path] : entries) {
      if (status == 2) {  // DELETED
        active.erase(std::remove(active.begin(), active.end(), path), active.end());
      } else {  // ADDED (1) ou EXISTING (0)
        if (std::find(active.begin(), active.end(), path) == active.end()) active.push_back(path);
      }
    }
  }
  return active;
}

std::string strip_scheme(const std::string& path) {
  return path.rfind("file://", 0) == 0 ? path.substr(7) : path;
}

// grava um data file e devolve suas informacoes (caminho absoluto com file://)
FileInfo write_data_file(const std::string& dir, const Value& tabela) {
  const std::string name = new_uuid() + ".parquet";
  const std::string path = dir + "/data/" + name;
  parquet_write(path, tabela);
  const std::int64_t size = file_size(path);
  if (size <= 0) die("falha ao gravar '" + path + "'");
  return {"file://" + path, static_cast<std::int64_t>(tabela.list->size()), size};
}

void validate_schema_match(const std::vector<Column>& expected, const std::vector<Column>& got,
                           const char* ctx, const std::string& dir) {
  if (expected.size() != got.size()) {
    die(std::string(ctx) + ": schema divergente em '" + dir + "' (esperado " +
        columns_desc(expected) + "; recebido " + columns_desc(got) + ")");
  }
  for (std::size_t k = 0; k < expected.size(); ++k) {
    if (expected[k].name != got[k].name || expected[k].type != got[k].type) {
      die(std::string(ctx) + ": schema divergente em '" + dir + "' (esperado " +
          columns_desc(expected) + "; recebido " + columns_desc(got) + ")");
    }
  }
}

}  // namespace

void iceberg_write(const std::string& dir, const Value& tabela) {
  const std::vector<Column> cols = table_columns(tabela, "escrever_iceberg");
  mkdir_if_missing(dir);
  const std::string meta_dir = dir + "/metadata";
  mkdir_if_missing(meta_dir);
  mkdir_if_missing(dir + "/data");

  // sobrescreve: remove metadata anterior (data avro/parquet orfao fica para
  // tras, como no delta — a leitura so enxerga o que o metadata referencia).
  for (const std::string& old : list_metadata_files(meta_dir)) {
    if (::unlink(old.c_str()) != 0) die("nao foi possivel limpar '" + old + "'");
  }

  const FileInfo data = write_data_file(dir, tabela);

  const std::int64_t snapshot_id = new_snapshot_id();
  const std::int64_t ts = now_ms();

  const std::string manifest_name =
      write_manifest(meta_dir, {{1, data.path}}, snapshot_id, {data});
  const std::string list_path =
      write_manifest_list(meta_dir, manifest_name, snapshot_id, 1, 0, 0);

  Snapshot snap;
  snap.id = snapshot_id;
  snap.ts = ts;
  snap.operation = "overwrite";
  snap.manifest_list = list_path;
  snap.has_parent = false;
  snap.parent = -1;

  const std::string json = build_metadata_json(dir, cols, {snap}, {{ts, snapshot_id}}, snapshot_id, ts);
  commit_metadata(dir, 0, json);
}

void iceberg_append(const std::string& dir, const Value& tabela) {
  struct stat st {};
  if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    die("tabela nao existe em '" + dir + "' (use escrever_iceberg para criar)");
  }
  const std::string meta_dir = dir + "/metadata";
  if (::stat(meta_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    die("tabela nao existe em '" + dir + "' (use escrever_iceberg para criar)");
  }

  TableMeta meta;
  latest_metadata_path(dir, meta);
  const std::vector<Column> got = table_columns(tabela, "anexar_iceberg");
  validate_schema_match(meta.schema_cols, got, "anexar_iceberg", dir);

  const FileInfo data = write_data_file(dir, tabela);

  const std::int64_t snapshot_id = new_snapshot_id();
  const std::int64_t ts = now_ms();

  const std::string manifest_name = write_manifest(meta_dir, {{1, data.path}}, snapshot_id, {data});
  const std::string list_path = write_manifest_list(meta_dir, manifest_name, snapshot_id, 1, 0, 0);

  Snapshot snap;
  snap.id = snapshot_id;
  snap.ts = ts;
  snap.operation = "append";
  snap.manifest_list = list_path;
  snap.parent = meta.current_snapshot;
  snap.has_parent = meta.current_snapshot >= 0;

  std::vector<Snapshot> snapshots = meta.snapshots;
  snapshots.push_back(std::move(snap));
  std::vector<std::pair<std::int64_t, std::int64_t>> log = meta.snapshot_log;
  log.emplace_back(ts, snapshot_id);

  const std::int64_t version = meta.version < 0 ? 0 : meta.version + 1;
  const std::string json =
      build_metadata_json(dir, meta.schema_cols.empty() ? got : meta.schema_cols, snapshots, log,
                          snapshot_id, ts);
  commit_metadata(dir, version, json);
}

Value iceberg_read(const std::string& dir) {
  TableMeta meta;
  latest_metadata_path(dir, meta);
  const std::vector<std::string> active = resolve_active_files(meta);
  if (active.empty()) die("tabela em '" + dir + "' esta vazia (nenhum data file ativo)");

  Value out = Value::tabela();
  std::vector<Column> schema_cols;
  for (const std::string& raw : active) {
    const std::string path = strip_scheme(raw);
    Value chunk = parquet_read(path);
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
      die("arquivo '" + path + "' nao e uma tabela parquet");
    }
    for (Value& row : *chunk.list) {
      if (row.kind != ValueKind::Mapa || !row.map) die("linha de '" + path + "' nao e um mapa");
      if (schema_cols.empty()) {
        for (const auto& kv : row.map->items) {
          schema_cols.push_back({kv.first, iceberg_type_name(kv.second), kv.second});
        }
      } else {
        if (row.map->items.size() != schema_cols.size()) {
          die("schema divergente em '" + path + "' (colunas diferentes da 1a versao)");
        }
        for (std::size_t k = 0; k < schema_cols.size(); ++k) {
          if (row.map->items[k].first != schema_cols[k].name) {
            die("schema divergente em '" + path + "' (ordem/nome de colunas difere)");
          }
        }
      }
      out.list->push_back(std::move(row));
    }
  }
  return out;
}

}  // namespace tilt::rt
