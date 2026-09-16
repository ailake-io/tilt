#include "runtime/iceberg.hpp"

#include "runtime/compat.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/json.hpp"
#include "runtime/parquet.hpp"
#include "runtime/snappy_codec.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("iceberg: " + m); }

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
      die("nao foi possivel criar o diretorio '" + path + "'");
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
// Codecs de bloco: "null", "deflate" (deflate RAW, RFC1951, via zlib dlopen)
// e "snappy" (bloco snappy de literais + trailer CRC32, spec Avro); a escrita
// usa "deflate" por default e aceita override via env ICEBERG_AVRO_CODEC
// (null|deflate|snappy) — a leitura aceita os tres independente da env.
// Tipos: null/boolean/int/long/float/double/string/bytes
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

// Branch efetiva de uma uniao: indice 0 (null) ou 1 ("null" sempre em 0).
// O discriminant Avro e um long zigzag (indice 1 -> 0x02); escritas ate a
// fase 25 usavam varint cru (0x01) — ambos sao aceitos na leitura.
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
      put_long(out, 0);
      return;
    }
    put_long(out, 1);
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
    const std::int64_t raw = static_cast<std::int64_t>(dec.varint());
    // 0 = null; 1 = branch 1 legado (varint cru, fases <= 25);
    // 2 = branch 1 padrao (long zigzag). Unioes do tilt sao ["null", T].
    if (raw == 0) return Value::nulo();
    const std::int64_t idx = raw == 2 ? 1 : raw;
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

// ---------------------------------------------------------------------------
// Codecs dos blocos OCF — zlib via dlopen (mesmo padrao do parquet.cpp) +
// bloco snappy de literais (snappy_codec.hpp).
// ---------------------------------------------------------------------------

// zlib carregada via dlopen — mesmo padrao de sqlite.cpp/postgres.cpp.
struct ZStream {
  const std::uint8_t* next_in;
  unsigned int avail_in;
  unsigned long total_in;
  std::uint8_t* next_out;
  unsigned int avail_out;
  unsigned long total_out;
  const char* msg;
  void* state;
  void* (*zalloc)(void*, unsigned int, unsigned int);
  void (*zfree)(void*, void*);
  void* opaque;
  int data_type;
  unsigned long adler;
  unsigned long reserved;
};

struct ZlibApi {
  void* lib = nullptr;
  const char* (*version)() = nullptr;
  int (*inflate_init2)(ZStream*, int, const char*, int) = nullptr;
  int (*inflate)(ZStream*, int) = nullptr;
  int (*inflate_end)(ZStream*) = nullptr;
  int (*deflate_init2)(ZStream*, int, int, int, int, int, const char*, int) = nullptr;
  int (*deflate)(ZStream*, int) = nullptr;
  int (*deflate_end)(ZStream*) = nullptr;
  unsigned long (*crc32)(unsigned long, const unsigned char*, unsigned int) = nullptr;
};

template <typename F>
bool bind_zsym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(tilt_dlsym(lib, name));
  return fn != nullptr;
}

const ZlibApi& zlib() {
  static const ZlibApi instance = [] {
    ZlibApi a;
#if defined(_WIN32)
    a.lib = tilt_dlopen("zlib1.dll");
    if (!a.lib) a.lib = tilt_dlopen("zlib.dll");
#else
    a.lib = tilt_dlopen("libz.so.1");
    if (!a.lib) a.lib = tilt_dlopen("libz.so");
    if (!a.lib) a.lib = tilt_dlopen("libz.1.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libz.dylib");
#endif
    if (!a.lib) return a;
    const bool ok = bind_zsym(a.lib, a.version, "zlibVersion") &&
                    bind_zsym(a.lib, a.inflate_init2, "inflateInit2_") &&
                    bind_zsym(a.lib, a.inflate, "inflate") &&
                    bind_zsym(a.lib, a.inflate_end, "inflateEnd") &&
                    bind_zsym(a.lib, a.deflate_init2, "deflateInit2_") &&
                    bind_zsym(a.lib, a.deflate, "deflate") &&
                    bind_zsym(a.lib, a.deflate_end, "deflateEnd") &&
                    bind_zsym(a.lib, a.crc32, "crc32");
    if (!ok) {
      tilt_dlclose(a.lib);
      a = ZlibApi{};
    }
    return a;
  }();
  return instance;
}

constexpr int kZNoFlush = 0;
constexpr int kZFinish = 4;
constexpr int kZStreamEnd = 1;
constexpr int kZDefaultCompression = -1;
constexpr int kZDeflated = 8;
constexpr int kWindowBitsRaw = -15;  // deflate "cru" (RFC1951, sem header zlib/gzip)

std::string zlib_ausente_avro() {
  return "codec avro requer zlib (libz.so.1 no Linux, zlib1.dll no Windows), "
         "que nao foi encontrada; instale o pacote zlib";
}

// Descomprime um bloco deflate RAW (RFC1951), codec "deflate" do Avro.
std::string avro_inflate(const std::string& in, const std::string& ctx) {
  const ZlibApi& z = zlib();
  if (!z.lib) die(zlib_ausente_avro());
  ZStream s{};
  s.next_in = reinterpret_cast<const std::uint8_t*>(in.data());
  s.avail_in = static_cast<unsigned int>(in.size());
  if (z.inflate_init2(&s, kWindowBitsRaw, z.version(), static_cast<int>(sizeof(ZStream))) != 0) {
    die("falha ao inicializar a zlib (inflateInit2)");
  }
  std::string out;
  int ret;
  do {
    const std::size_t base = out.size();
    out.resize(base + 65536);
    s.next_out = reinterpret_cast<std::uint8_t*>(out.data() + base);
    s.avail_out = 65536;
    ret = z.inflate(&s, kZNoFlush);
  } while (ret == 0);
  z.inflate_end(&s);
  if (ret != kZStreamEnd) {
    die("falha ao descomprimir bloco avro em '" + ctx + "' (zlib retornou " +
        std::to_string(ret) + ")");
  }
  out.resize(s.total_out);
  return out;
}

// Comprime num bloco deflate RAW (RFC1951), codec "deflate" do Avro.
std::string avro_deflate(const std::string& in, const std::string& ctx) {
  const ZlibApi& z = zlib();
  if (!z.lib) die(zlib_ausente_avro());
  ZStream s{};
  if (z.deflate_init2(&s, kZDefaultCompression, kZDeflated, kWindowBitsRaw, 8, 0, z.version(),
                      static_cast<int>(sizeof(ZStream))) != 0) {
    die("falha ao inicializar a zlib (deflateInit2)");
  }
  s.next_in = reinterpret_cast<const std::uint8_t*>(in.data());
  s.avail_in = static_cast<unsigned int>(in.size());
  std::string out;
  int ret;
  do {
    const std::size_t base = out.size();
    out.resize(base + 65536);
    s.next_out = reinterpret_cast<std::uint8_t*>(out.data() + base);
    s.avail_out = 65536;
    ret = z.deflate(&s, kZFinish);
  } while (ret == 0);
  z.deflate_end(&s);
  if (ret != kZStreamEnd) {
    die("falha ao comprimir bloco avro em '" + ctx + "' (zlib retornou " + std::to_string(ret) +
        ")");
  }
  out.resize(s.total_out);
  return out;
}

// CRC32 (IEEE, como no trailer snappy do Avro) via zlib dlopen.
std::uint32_t avro_crc32(const std::string& in) {
  const ZlibApi& z = zlib();
  if (!z.lib) die(zlib_ausente_avro());
  return static_cast<std::uint32_t>(
      z.crc32(0, reinterpret_cast<const unsigned char*>(in.data()),
              static_cast<unsigned int>(in.size())));
}

// Bloco snappy do Avro: stream snappy valida + trailer de 4 bytes big-endian
// com o CRC32 dos dados NAO comprimidos (especificacao do codec snappy).
std::string avro_snappy_compress(const std::string& in) {
  std::string out = snappy_compress_literals(in);
  const std::uint32_t crc = avro_crc32(in);
  out += static_cast<char>((crc >> 24) & 0xFF);
  out += static_cast<char>((crc >> 16) & 0xFF);
  out += static_cast<char>((crc >> 8) & 0xFF);
  out += static_cast<char>(crc & 0xFF);
  return out;
}

std::string avro_snappy_decompress(const std::string& in, const std::string& ctx) {
  if (in.size() < 4) die("bloco snappy sem trailer de CRC32 em '" + ctx + "'");
  const std::string payload = in.substr(0, in.size() - 4);
  const std::uint32_t esperado =
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[in.size() - 4])) << 24) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[in.size() - 3])) << 16) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[in.size() - 2])) << 8) |
      static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[in.size() - 1]));
  std::string out;
  try {
    out = snappy_decompress(payload);
  } catch (const std::exception& e) {
    die("bloco snappy invalido em '" + ctx + "': " + e.what());
  }
  if (avro_crc32(out) != esperado) {
    die("CRC32 do bloco snappy diverge em '" + ctx + "'");
  }
  return out;
}

// Codec dos blocos OCF (manifests e manifest lists) na escrita: "deflate"
// (default), "null" ou "snappy", override via env ICEBERG_AVRO_CODEC. Valor
// invalido -> erro claro. A leitura aceita os tres, independente da env.
std::string avro_ocf_codec() {
  const char* env = std::getenv("ICEBERG_AVRO_CODEC");
  const std::string codec = env && *env ? env : "deflate";
  if (codec != "null" && codec != "deflate" && codec != "snappy") {
    die("ICEBERG_AVRO_CODEC invalido '" + codec + "' (esperado: null, deflate ou snappy)");
  }
  return codec;
}

// OCF writer: um unico bloco com todos os registros, sync fixo.
std::string ocf_write(const Value& schema, const std::string& schema_json,
                      const std::vector<Value>& records,
                      const std::vector<std::pair<std::string, std::string>>& extra_meta) {
  static const char kSync[] = "\x77\xB6\xD2\xD1\x6E\xA6\x86\x79"
                              "\x98\x72\x8A\x66\x88\xA8\x44\x56";
  const std::string codec = avro_ocf_codec();
  std::string block;
  for (const Value& r : records) avro_encode(block, schema, r);
  if (!records.empty()) {
    if (codec == "deflate") {
      block = avro_deflate(block, "manifest");
    } else if (codec == "snappy") {
      block = avro_snappy_compress(block);
    }
  }

  std::string out = "Obj\x01";
  // metadata map<string, bytes>: um bloco so
  std::vector<std::pair<std::string, std::string>> meta;
  meta.emplace_back("avro.schema", schema_json);
  meta.emplace_back("avro.codec", codec);
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
    put_long(out, static_cast<std::int64_t>(block.size()));
    out += block;
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
  if (codec != "null" && codec != "deflate" && codec != "snappy") {
    die("codec avro '" + codec + "' nao suportado (suportados: null, deflate e snappy)");
  }

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
    // descompressao do bloco conforme o codec do header (independente da
    // env ICEBERG_AVRO_CODEC, que so vale para a escrita)
    std::string body;
    if (codec == "deflate") {
      body = avro_inflate(raw, path);
    } else if (codec == "snappy") {
      body = avro_snappy_decompress(raw, path);
    } else {
      body = std::move(raw);
    }
    AvroDecoder bdec(body, path + " (bloco)");
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

// Field-ids oficiais da spec Iceberg (v2) em todos os campos — um reader
// real (pyiceberg) converte o schema Avro via essas propriedades; o writer/
// reader Avro proprio do tilt as ignora. Campos sempre nulos da spec
// (column_sizes, bounds, key_metadata, split_offsets, equality_ids) ficam
// de fora do schema escrito: o reader preenche com o default (None) e o
// writer nao precisa codifica-los.
const char* kManifestEntrySchema = R"AVRO({"type":"record","name":"manifest_entry","fields":[
{"name":"status","type":"int","field-id":0},
{"name":"snapshot_id","type":["null","long"],"default":null,"field-id":1},
{"name":"sequence_number","type":["null","long"],"default":null,"field-id":3},
{"name":"file_sequence_number","type":["null","long"],"default":null,"field-id":4},
{"name":"data_file","type":{"type":"record","name":"data_file","fields":[
{"name":"content","type":"int","field-id":134},
{"name":"file_path","type":"string","field-id":100},
{"name":"file_format","type":"string","field-id":101},
{"name":"partition","type":{"type":"record","name":"partition","fields":[]},"field-id":102},
{"name":"record_count","type":"long","field-id":103},
{"name":"file_size_in_bytes","type":"long","field-id":104},
{"name":"sort_order_id","type":["null","int"],"default":null,"field-id":140}
]},"field-id":2}]})AVRO";

// Manifest list v2 (field-ids 500+ da spec; 512/513/514 sao rows counts
// long, NAO has_* boolean — confusao corrigida nesta fase).
const char* kManifestListSchema = R"AVRO({"type":"record","name":"manifest_file","fields":[
{"name":"manifest_path","type":"string","field-id":500},
{"name":"manifest_length","type":"long","field-id":501},
{"name":"partition_spec_id","type":"int","field-id":502},
{"name":"content","type":"int","field-id":517},
{"name":"sequence_number","type":"long","field-id":515},
{"name":"min_sequence_number","type":"long","field-id":516},
{"name":"added_snapshot_id","type":"long","field-id":503},
{"name":"added_files_count","type":"int","field-id":504},
{"name":"existing_files_count","type":"int","field-id":505},
{"name":"deleted_files_count","type":"int","field-id":506},
{"name":"added_rows_count","type":"long","field-id":512},
{"name":"existing_rows_count","type":"long","field-id":513},
{"name":"deleted_rows_count","type":"long","field-id":514},
{"name":"partitions","type":["null",{"type":"array","element-id":508,"items":{"type":"record","name":"r508","fields":[
{"name":"contains_null","type":"boolean","field-id":509},
{"name":"contains_nan","type":["null","boolean"],"default":null,"field-id":518},
{"name":"lower_bound","type":["null","bytes"],"default":null,"field-id":510},
{"name":"upper_bound","type":["null","bytes"],"default":null,"field-id":511}
]}}],"default":null,"field-id":507},
{"name":"key_metadata","type":["null","bytes"],"default":null,"field-id":519}
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
    case ValueKind::Mapa: return "struct";  // aninhado (Marco 2 / B5)
    default: return "string";
  }
}

struct Column {
  std::string name;
  std::string type;  // iceberg ("struct" para aninhados; ver children)
  Value sample;      // valor da 1a linha (deduzir tipo)
  std::int64_t id = 0;       // field-id no schema iceberg (0 = nao atribuido)
  bool required = true;      // false = nullable (colunas novas por evolucao)
  bool is_struct = false;    // grupo STRUCT (Marco 2 / B5)
  std::vector<Column> children;
};

// Campo de um partition spec. `transform` e "identity" (padrao) ou
// "bucket[N]" (Fase 12-5a); outros transforms de leitura (truncate, year,
// month, day, hour) sao aceitos no parse sem die, mas sem poda por valor.
// `name` e o nome do campo de particao (identity: a coluna; bucket[N](col):
// "col_bucket_N", padrao Iceberg); `source_name` e a coluna de origem.
// `avro_ty` e o tipo do campo no record `partition` do manifest.
struct PartitionField {
  std::string name;
  std::int64_t field_id = 1000;
  std::int64_t source_id = 0;
  std::string avro_ty = "string";
  std::string transform = "identity";
  int num_buckets = 0;
  std::string source_name;
  std::string source_type;  // iceberg type da coluna de origem (bucket)
};

// MurmurHash3 x86_32 (seed 0), como a spec Iceberg usa no bucket transform.
std::uint32_t murmur3_x86_32(const std::uint8_t* data, std::size_t len) {
  const std::uint32_t c1 = 0xcc9e2d51u;
  const std::uint32_t c2 = 0x1b873593u;
  std::uint32_t h = 0;
  const std::size_t nblocks = len / 4;
  for (std::size_t i = 0; i < nblocks; ++i) {
    std::uint32_t k;
    std::memcpy(&k, data + i * 4, 4);
    k *= c1;
    k = (k << 15) | (k >> 17);
    k *= c2;
    h ^= k;
    h = (h << 13) | (h >> 19);
    h = h * 5 + 0xe6546b64u;
  }
  const std::uint8_t* tail = data + nblocks * 4;
  std::uint32_t k = 0;
  switch (len & 3) {
    case 3: k ^= static_cast<std::uint32_t>(tail[2]) << 16; [[fallthrough]];
    case 2: k ^= static_cast<std::uint32_t>(tail[1]) << 8; [[fallthrough]];
    case 1:
      k ^= static_cast<std::uint32_t>(tail[0]);
      k *= c1;
      k = (k << 15) | (k >> 17);
      k *= c2;
      h ^= k;
  }
  h ^= static_cast<std::uint32_t>(len);
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

// Serializacao do valor para o hash do bucket (spec Iceberg): int 4B LE,
// long 8B LE, string UTF-8, boolean 1B. double/float/decimal nao suportados
// (erro claro — o hash de ponto flutuante exige normalizacao de NaN).
std::string bucket_hash_bytes(const Value& v, const std::string& iceberg_type,
                              const std::string& col) {
  std::string out;
  if (iceberg_type == "int") {
    if (v.kind != ValueKind::Inteiro) die("bucket: coluna '" + col + "' exige inteiro");
    const std::int32_t x = static_cast<std::int32_t>(v.i);
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<char>((x >> (8 * k)) & 0xFF));
    return out;
  }
  if (iceberg_type == "long") {
    if (v.kind != ValueKind::Inteiro) die("bucket: coluna '" + col + "' exige inteiro");
    const std::uint64_t x = static_cast<std::uint64_t>(v.i);
    for (int k = 0; k < 8; ++k) out.push_back(static_cast<char>((x >> (8 * k)) & 0xFF));
    return out;
  }
  if (iceberg_type == "string") {
    if (v.kind != ValueKind::Texto) die("bucket: coluna '" + col + "' exige texto");
    return v.s;
  }
  if (iceberg_type == "boolean") {
    if (v.kind != ValueKind::Logico) die("bucket: coluna '" + col + "' exige logico");
    out.push_back(v.b ? '\1' : '\0');
    return out;
  }
  die("bucket: coluna '" + col + "' do tipo '" + iceberg_type +
      "' nao suportada (use inteiro, texto ou logico)");
}

// bucket(v) = murmur3(bytes) % N com semantica Java (% com sinal).
int bucket_of(const Value& v, const std::string& iceberg_type, int n, const std::string& col) {
  if (v.kind == ValueKind::Nulo) die("valor nulo em coluna de particao '" + col + "'");
  const std::string bytes = bucket_hash_bytes(v, iceberg_type, col);
  const std::int32_t h = static_cast<std::int32_t>(murmur3_x86_32(
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
  const int r = h % n;
  return r < 0 ? r + n : r;
}

// "bucket[16](col)" -> {col, 16}; "" quando nao e sintaxe bucket.
std::pair<std::string, int> parse_bucket(const std::string& s) {
  if (s.rfind("bucket[", 0) != 0) return {"", 0};
  const std::size_t rb = s.find(']', 7);
  if (rb == std::string::npos || rb + 1 >= s.size() || s[rb + 1] != '(' || s.back() != ')') {
    die("particionar_por: '" + s +
        "' invalido (use bucket[N](coluna), ex.: bucket[16](id))");
  }
  int n = 0;
  try {
    n = std::stoi(s.substr(7, rb - 7));
  } catch (const std::exception&) {
    die("particionar_por: '" + s + "' com N invalido (use bucket[N](coluna))");
  }
  if (n <= 0 || n > 100000) die("particionar_por: '" + s + "' com N fora de 1..100000");
  const std::string col = s.substr(rb + 2, s.size() - rb - 3);
  if (col.empty()) die("particionar_por: '" + s + "' sem coluna (use bucket[N](coluna))");
  return {col, n};
}

// "truncate[10](col)" -> {col, 10}; "" quando nao e sintaxe truncate.
std::pair<std::string, int> parse_truncate(const std::string& s) {
  if (s.rfind("truncate[", 0) != 0) return {"", 0};
  const std::size_t rb = s.find(']', 9);
  if (rb == std::string::npos || rb + 1 >= s.size() || s[rb + 1] != '(' || s.back() != ')') {
    die("particionar_por: '" + s +
        "' invalido (use truncate[W](coluna), ex.: truncate[10](id))");
  }
  int w = 0;
  try {
    w = std::stoi(s.substr(9, rb - 9));
  } catch (const std::exception&) {
    die("particionar_por: '" + s + "' com W invalido (use truncate[W](coluna))");
  }
  if (w <= 0) die("particionar_por: '" + s + "' com W <= 0");
  const std::string col = s.substr(rb + 2, s.size() - rb - 3);
  if (col.empty()) die("particionar_por: '" + s + "' sem coluna (use truncate[W](coluna))");
  return {col, w};
}

// "year(col)", "month(col)", "day(col)", "hour(col)" -> {col, nome}; "" se
// nao for transform temporal.
std::pair<std::string, std::string> parse_temporal(const std::string& s) {
  static const std::vector<std::string> names = {"year", "month", "day", "hour"};
  for (const std::string& name : names) {
    if (s.rfind(name + "(", 0) != 0) continue;
    if (s.back() != ')') continue;
    const std::string col = s.substr(name.size() + 1, s.size() - name.size() - 2);
    if (col.empty()) continue;
    return {col, name};
  }
  return {"", ""};
}

// Extrai componente ano/mes/dia/hora de uma string ISO (date ou timestamp).
// Formato minimo: "YYYY-MM-DD" ou "YYYY-MM-DDTHH:MM:SS..."; delimitador T
// ou espaco.
int extract_iso_component(const std::string& s, const std::string& component) {
  if (s.size() < 10) die("data/timestamp fora do formato ISO: '" + s + "'");
  int year = 0, month = 0, day = 0;
  try {
    year = std::stoi(s.substr(0, 4));
    month = std::stoi(s.substr(5, 2));
    day = std::stoi(s.substr(8, 2));
  } catch (const std::exception&) {
    die("data/timestamp fora do formato ISO: '" + s + "'");
  }
  if (component == "year") return year;
  if (component == "month") return month;
  if (component == "day") return day;
  if (component != "hour") die("componente temporal desconhecido: '" + component + "'");
  if (s.size() < 13 || (s[10] != 'T' && s[10] != ' ')) {
    die("timestamp fora do formato ISO (esperado 'YYYY-MM-DDTHH:...'): '" + s + "'");
  }
  try {
    return std::stoi(s.substr(11, 2));
  } catch (const std::exception&) {
    die("timestamp fora do formato ISO: '" + s + "'");
  }
}

// Aplica um transform de particao a um valor de origem.
Value apply_transform(const Value& v, const PartitionField& pf) {
  if (v.kind == ValueKind::Nulo) die("valor nulo em coluna de particao '" + pf.source_name + "'");
  if (pf.transform == "identity" || pf.transform.empty()) return v;
  if (pf.transform.rfind("bucket[", 0) == 0) {
    return Value::inteiro(bucket_of(v, pf.source_type, pf.num_buckets, pf.source_name));
  }
  if (pf.transform.rfind("truncate[", 0) == 0) {
    const std::size_t rb = pf.transform.find(']');
    int w = 0;
    try {
      w = std::stoi(pf.transform.substr(9, rb - 9));
    } catch (const std::exception&) {
      die("transform truncate invalido: '" + pf.transform + "'");
    }
    if (v.kind == ValueKind::Inteiro) {
      const std::int64_t q = v.i / w;
      return Value::inteiro(q * w);
    }
    if (v.kind == ValueKind::Texto) {
      const std::string prefix = v.s.substr(0, static_cast<std::size_t>(w));
      return Value::texto(prefix);
    }
    die("truncate: coluna '" + pf.source_name + "' deve ser inteiro ou texto");
  }
  if (pf.transform == "year" || pf.transform == "month" || pf.transform == "day" ||
      pf.transform == "hour") {
    if (v.kind != ValueKind::Texto) {
      die("transform " + pf.transform + ": coluna '" + pf.source_name +
          "' deve ser texto no formato ISO (date/timestamp)");
    }
    return Value::inteiro(extract_iso_component(v.s, pf.transform));
  }
  die("transform de particao nao suportado: '" + pf.transform + "'");
}

// Colunas da 1a linha, recursivo para mapas (structs aninhados; listas e
// escalares sao folhas). Chaves de struct: uniao em ordem de 1a aparicao
// (como o writer parquet); tipo pela 1a celula nao nula da chave.
// Colunas de struct com valor Nulo na 1a linha e mapa depois: a chave entra
// via uniao (tipo deduzido quando aparece).
void collect_struct_fields(const std::vector<const Value*>& cells, Column& out) {
  static const Value kNull = Value::nulo();
  std::vector<std::string> keys;
  for (const Value* cell : cells) {
    if (cell->kind != ValueKind::Mapa || !cell->map) continue;
    for (const auto& kv : cell->map->items) {
      if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
    }
  }
  for (const std::string& k : keys) {
    Column ch;
    ch.name = k;
    for (const Value* cell : cells) {
      if (cell->kind != ValueKind::Mapa || !cell->map) continue;
      const Value* f = cell->map->find(k);
      if (!f || f->kind == ValueKind::Nulo) continue;
      if (f->kind == ValueKind::Lista) {
        die("coluna struct com campo lista '" + k +
            "' ainda nao suportada no Iceberg (use colunas escalares/aninhadas)");
      }
      if (f->kind == ValueKind::Mapa) {
        ch.type = "struct";
        ch.is_struct = true;
        ch.sample = *f;
        std::vector<const Value*> sub;
        for (const Value* c2 : cells) {
          if (c2->kind == ValueKind::Mapa && c2->map) {
            if (const Value* f2 = c2->map->find(k)) {
              sub.push_back(f2);
              continue;
            }
          }
          sub.push_back(&kNull);
        }
        collect_struct_fields(sub, ch);
      } else {
        ch.type = iceberg_type_name(*f);
        ch.sample = *f;
      }
      break;
    }
    if (ch.type.empty()) {
      ch.type = "string";  // so nulos: assume texto (parquet falha se vazio de fato)
      ch.required = false;
    }
    out.children.push_back(std::move(ch));
  }
}

std::vector<Column> table_columns(const Value& tabela, const char* ctx) {
  if (tabela.kind != ValueKind::Lista && tabela.kind != ValueKind::Tabela) {
    die(std::string(ctx) + " espera uma tabela (lista de mapas)");
  }
  if (tabela.list->empty()) die(std::string(ctx) + ": tabela vazia (sem schema deduzivel)");
  const Value& first = tabela.list->front();
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");
  std::vector<Column> cols;
  for (const auto& kv : first.map->items) {
    // Tipo pela 1a celula nao nula (struct Nulo na 1a linha e mapa depois).
    const Value* rep = &kv.second;
    if (rep->kind == ValueKind::Nulo) {
      for (const Value& row : *tabela.list) {
        if (row.kind != ValueKind::Mapa || !row.map) break;
        const Value* f = row.map->find(kv.first);
        if (f && f->kind != ValueKind::Nulo) {
          rep = f;
          break;
        }
      }
    }
    Column c;
    c.name = kv.first;
    if (rep->kind == ValueKind::Mapa) {
      c.type = "struct";
      c.is_struct = true;
      c.sample = *rep;
      std::vector<const Value*> cells;
      for (const Value& row : *tabela.list) {
        if (row.kind == ValueKind::Mapa && row.map) {
          if (const Value* f = row.map->find(kv.first)) {
            cells.push_back(f);
            continue;
          }
        }
        static const Value kNull = Value::nulo();
        cells.push_back(&kNull);
      }
      collect_struct_fields(cells, c);
    } else {
      c.type = iceberg_type_name(kv.second);
      c.sample = kv.second;
    }
    cols.push_back(std::move(c));
  }
  return cols;
}

// Atribui field-ids em profundidade (struct recebe id, depois os filhos).
void assign_ids(std::vector<Column>& cols, std::int64_t& proximo) {
  for (Column& c : cols) {
    c.id = ++proximo;
    if (c.is_struct) assign_ids(c.children, proximo);
  }
}

// Ids em profundidade (struct recebe id, depois os filhos — mesma ordem do
// footer parquet, incluindo grupos struct para o mapeamento por id).
void leaf_ids(const std::vector<Column>& cols, std::vector<int>& out) {
  for (const Column& c : cols) {
    out.push_back(static_cast<int>(c.id));
    if (c.is_struct) {
      leaf_ids(c.children, out);
    }
  }
}

// Fields de um schema no metadata ({"id","name","required","type"} por campo,
// ids/required ja resolvidos no vetor). Struct aninhado: "type" e objeto
// {"type":"struct","fields":[...]} recursivo.
std::string schema_json_fields(const std::vector<Column>& fields) {
  std::string out = "[";
  for (std::size_t k = 0; k < fields.size(); ++k) {
    if (k) out += ',';
    out += "{\"id\":" + std::to_string(fields[k].id) + ",\"name\":\"" + json_escape(fields[k].name) +
           "\",\"required\":" + (fields[k].required ? "true" : "false") + ",\"type\":";
    if (fields[k].is_struct) {
      out += "{\"type\":\"struct\",\"fields\":" + schema_json_fields(fields[k].children) + "}";
    } else {
      out += "\"" + fields[k].type + "\"";
    }
    out += "}";
  }
  out += ']';
  return out;
}

// Schema Iceberg como objeto struct (createTable REST e update add-schema):
// {"type":"struct","schema-id":N,"fields":[...],"identifier-field-ids":[]}.
std::string schema_struct_json(std::int64_t schema_id, const std::vector<Column>& fields) {
  return "{\"type\":\"struct\",\"schema-id\":" + std::to_string(schema_id) +
         ",\"fields\":" + schema_json_fields(fields) + ",\"identifier-field-ids\":[]}";
}

// Partition spec como objeto (createTable REST): {"spec-id":0,"fields":[...]}.
std::string partition_spec_json(const std::vector<PartitionField>& spec) {
  std::string out = "{\"spec-id\":0,\"fields\":[";
  for (std::size_t k = 0; k < spec.size(); ++k) {
    const PartitionField& pf = spec[k];
    out += (k ? ", " : "") + std::string("{ \"field-id\": ") + std::to_string(pf.field_id) +
           ", \"source-id\": " + std::to_string(pf.source_id) + ", \"transform\": \"" +
           json_escape(pf.transform) + "\", \"name\": \"" + json_escape(pf.name) + "\" }";
  }
  out += "]}";
  return out;
}

std::string join_names(const std::vector<Column>& cols) {
  std::string out = "[";
  for (std::size_t k = 0; k < cols.size(); ++k) {
    if (k) out += ", ";
    out += cols[k].name;
  }
  out += ']';
  return out;
}

std::string join_part_cols(const std::vector<std::string>& cols) {
  std::string out = "[";
  for (std::size_t k = 0; k < cols.size(); ++k) {
    if (k) out += ", ";
    out += cols[k];
  }
  out += ']';
  return out;
}

// Monta o spec a partir da tabela escrita: localiza cada coluna (erro claro
// em repetida ou inexistente), deduz o tipo pelo iceberg type do schema e o
// source-id pelo id da coluna (posicao + 1). Field-ids 1000, 1001, ... na
// ordem das colunas. Alem de nome de coluna (identity), aceita transforms:
// "bucket[N](col)", "truncate[W](col)", "year(col)", "month(col)",
// "day(col)" e "hour(col)". Os transforms derivam o valor da particao a
// partir da coluna de origem, que permanece no parquet.
std::vector<PartitionField> make_spec(const std::vector<Column>& cols,
                                      const std::vector<std::string>& part_cols,
                                      const char* ctx) {
  std::vector<PartitionField> spec;
  for (std::size_t i = 0; i < part_cols.size(); ++i) {
    const std::string& col = part_cols[i];
    if (std::count(part_cols.begin(), part_cols.end(), col) > 1) {
      die(std::string(ctx) + ": coluna de particao '" + col + "' repetida");
    }
    const auto [bsrc, nbuckets] = parse_bucket(col);
    const auto [tsrc, tname] = parse_temporal(col);
    const auto [wsrc, width] = parse_truncate(col);
    const bool eh_bucket = !bsrc.empty();
    const bool eh_temporal = !tsrc.empty();
    const bool eh_truncate = !wsrc.empty();
    const bool eh_transform = eh_bucket || eh_temporal || eh_truncate;
    if (!eh_transform && col.find('(') != std::string::npos) {
      die(std::string(ctx) + ": transform '" + col +
          "' nao suportado na escrita (suportados: coluna, bucket[N](col), "
          "truncate[W](col), year(col), month(col), day(col), hour(col))");
    }
    const std::string lookup = eh_bucket ? bsrc : eh_temporal ? tsrc : eh_truncate ? wsrc : col;
    bool achou = false;
    for (std::size_t k = 0; k < cols.size(); ++k) {
      if (cols[k].name == lookup) {
        if (cols[k].is_struct) {
          die(std::string(ctx) + ": coluna de particao '" + lookup +
              "' e struct (particao exige coluna escalar)");
        }
        PartitionField pf;
        pf.field_id = 1000 + static_cast<std::int64_t>(i);
        pf.source_id = static_cast<std::int64_t>(k + 1);
        pf.source_name = lookup;
        pf.source_type = cols[k].type;
        if (eh_bucket) {
          pf.name = lookup + "_bucket_" + std::to_string(nbuckets);
          pf.transform = "bucket[" + std::to_string(nbuckets) + "]";
          pf.num_buckets = nbuckets;
          pf.avro_ty = "int";
        } else if (eh_temporal) {
          pf.name = tname + "_" + lookup;
          pf.transform = tname;
          pf.avro_ty = "int";
        } else if (eh_truncate) {
          pf.name = lookup + "_trunc_" + std::to_string(width);
          pf.transform = "truncate[" + std::to_string(width) + "]";
          pf.avro_ty = cols[k].type == "string" ? "string" : "int";
        } else {
          pf.name = col;
          pf.avro_ty = cols[k].type;
        }
        spec.push_back(std::move(pf));
        achou = true;
        break;
      }
    }
    if (!achou) {
      die(std::string(ctx) + ": coluna de particao '" + lookup +
          "' nao existe na tabela (colunas: " + join_names(cols) + ")");
    }
  }
  return spec;
}

// Valor de particao como string (path hive-style no data file). Erro claro
// em nulo e em texto com '/' (fase 26: sem escaping de caracteres especiais).
std::string partition_value_string(const Value& v, const std::string& col) {
  switch (v.kind) {
    case ValueKind::Nulo:
      die("valor nulo em coluna de particao '" + col +
          "' (fase 26: particao com nulo nao e suportada)");
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
            "' contem '/' (fase 26: caracteres especiais nao suportados)");
      }
      return v.s;
    default:
      die("coluna de particao '" + col + "' deve ser texto, inteiro, decimal ou logico");
  }
}

// Linha sem as colunas de particao identity: o parquet nao armazena essas
// colunas (os valores vivem no path e no record `partition` do manifest).
// Campos derivados (bucket etc.) nao sao removidos: a coluna de origem
// permanece no arquivo e o valor de particao e derivado.
Value strip_partition_columns(const Value& row, const std::vector<PartitionField>& spec) {
  Value m = Value::mapa();
  for (const auto& kv : row.map->items) {
    bool eh_particao = false;
    for (const PartitionField& pf : spec) {
      if (pf.transform == "identity" && kv.first == pf.name) eh_particao = true;
    }
    if (!eh_particao) m.map->set(kv.first, kv.second);
  }
  return m;
}

struct PartitionGroup {
  std::string key;      // caminho relativo hive-style: "c1=v1/c2=v2"
  Value part_map;       // valores tipados por coluna (para o record `partition`)
  Value rows;           // tabela sem as colunas de particao
};

// Agrupa as linhas pela combinacao dos valores das colunas de particao, na
// ordem de 1a aparicao das chaves (define a ordem das entradas do manifest).
std::vector<PartitionGroup> partition_rows(const Value& tabela,
                                           const std::vector<PartitionField>& spec) {
  std::vector<PartitionGroup> grupos;
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
    PartitionGroup candidato;
    candidato.part_map = Value::mapa();
    for (const PartitionField& pf : spec) {
      const Value* cell = row.map->find(pf.source_name);
      const Value& v = cell ? *cell : Value::nulo();
      const Value part_val = apply_transform(v, pf);
      const std::string valor = partition_value_string(part_val, pf.name);
      candidato.key += (candidato.key.empty() ? "" : "/") + pf.name + "=" + valor;
      candidato.part_map.map->set(pf.name, part_val);
    }
    auto it = std::find_if(grupos.begin(), grupos.end(),
                           [&](const PartitionGroup& g) { return g.key == candidato.key; });
    if (it == grupos.end()) {
      candidato.rows = Value::tabela();
      it = grupos.insert(grupos.end(), std::move(candidato));
    }
    it->rows.list->push_back(strip_partition_columns(row, spec));
  }
  return grupos;
}

// ---------------------------------------------------------------------------
// Metadata (v<N>.metadata.json — nome canonico do HadoopCatalog; o leitor
// tambem aceita o layout legado v<N>-<uuid>.metadata.json)
// ---------------------------------------------------------------------------

std::vector<std::string> list_metadata_files(const std::string& meta_dir) {
  std::vector<std::string> entries;
  if (!tilt_listdir(meta_dir, entries)) return {};
  std::vector<std::pair<std::string, std::string>> found;
  for (const std::string& name : entries) {
    if (name.size() > 14 && name.compare(name.size() - 14, 14, ".metadata.json") == 0) {
      found.emplace_back(name, meta_dir + "/" + name);
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  out.reserve(found.size());
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

// Versao pelo nome do arquivo: "v10.metadata.json" -> 10 (canonico);
// "v0-<uuid>.metadata.json" -> 0 (legado). Fora de "v" + digitos -> -1
// (desconhecida; perde na comparacao de "maior versao").
std::int64_t metadata_version_from_name(const std::string& path) {
  const std::string base = path.substr(path.find_last_of('/') + 1);
  if (base.size() < 2 || base[0] != 'v' || base[1] < '0' || base[1] > '9') return -1;
  try {
    return std::stoll(base.substr(1));
  } catch (const std::exception&) {
    return -1;
  }
}

struct Snapshot {
  std::int64_t id = 0;
  std::int64_t ts = 0;
  std::string operation;
  std::string manifest_list;
  std::int64_t parent = -1;
  bool has_parent = false;
};

// Uma versao do schema no metadata (id + fields com ids/required estáveis —
// evolucao de schema, fase 27).
struct SchemaVer {
  std::int64_t id = 0;
  std::vector<Column> fields;
};

struct TableMeta {
  std::int64_t last_updated = 0;
  std::int64_t current_snapshot = -1;
  std::vector<Column> schema_cols;  // do current-schema-id
  std::vector<SchemaVer> schemas;   // todas as versoes (historico)
  std::int64_t current_schema_id = 0;
  std::int64_t last_column_id = 0;
  std::vector<Snapshot> snapshots;
  std::vector<std::pair<std::int64_t, std::int64_t>> snapshot_log;  // (ts, id)
  std::vector<PartitionField> spec;  // do default-spec-id (vazio = sem particao)
  std::int64_t version = -1;
  std::string location;
  std::string uuid;  // estavel por tabela (metadata "uuid", spec v2)
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
  if (const Value* id = map_find(md, "table-uuid"); id && id->kind == ValueKind::Texto) {
    out.uuid = id->s;
  }
  if (const Value* lu = map_find(md, "last-updated-ms"); lu && lu->kind == ValueKind::Inteiro) {
    out.last_updated = lu->i;
  }
  if (const Value* cs = map_find(md, "current-snapshot-id"); cs && cs->kind == ValueKind::Inteiro) {
    out.current_snapshot = cs->i;
  }

  // schemas (historico completo; current-schema-id aponta o corrente)
  if (const Value* c = map_find(md, "current-schema-id"); c && c->kind == ValueKind::Inteiro) {
    out.current_schema_id = c->i;
  }
  if (const Value* lc = map_find(md, "last-column-id"); lc && lc->kind == ValueKind::Inteiro) {
    out.last_column_id = lc->i;
  }
  if (const Value* schemas = map_find(md, "schemas"); schemas && schemas->kind == ValueKind::Lista) {
    // Campos recursivos (structs aninhados tem "type" objeto).
    std::function<void(const Value&, std::vector<Column>&)> le_campos =
        [&](const Value& lista, std::vector<Column>& dst) {
          if (lista.kind != ValueKind::Lista || !lista.list) return;
          for (const Value& f : *lista.list) {
            const Value* fid = map_find(f, "id");
            const Value* name = map_find(f, "name");
            const Value* type = map_find(f, "type");
            const Value* req = map_find(f, "required");
            Column c;
            c.id = fid && fid->kind == ValueKind::Inteiro ? fid->i : 0;
            c.name = name && name->kind == ValueKind::Texto ? name->s : "";
            c.required = req ? (req->kind == ValueKind::Logico ? req->b : true) : true;
            if (type && type->kind == ValueKind::Mapa && type->map) {
              const Value* st = map_find(*type, "type");
              const Value* sub = map_find(*type, "fields");
              if (st && st->kind == ValueKind::Texto && st->s == "struct" && sub) {
                c.type = "struct";
                c.is_struct = true;
                le_campos(*sub, c.children);
              } else {
                c.type = "string";
              }
            } else {
              c.type = type && type->kind == ValueKind::Texto ? type->s : "string";
            }
            out.last_column_id = std::max(out.last_column_id, c.id);
            std::function<void(const Column&)> max_filho = [&](const Column& x) {
              for (const Column& ch : x.children) {
                out.last_column_id = std::max(out.last_column_id, ch.id);
                max_filho(ch);
              }
            };
            max_filho(c);
            dst.push_back(std::move(c));
          }
        };
    for (const Value& s : *schemas->list) {
      const Value* sid = map_find(s, "schema-id");
      if (!sid || sid->kind != ValueKind::Inteiro) continue;
      SchemaVer ver;
      ver.id = sid->i;
      if (const Value* fields = map_find(s, "fields")) {
        le_campos(*fields, ver.fields);
      }
      out.schemas.push_back(std::move(ver));
    }
  }
  for (const SchemaVer& ver : out.schemas) {
    if (ver.id == out.current_schema_id) out.schema_cols = ver.fields;
  }

  // partition spec corrente (default-spec-id): identity e bucket[N]
  // (Fase 12-5a) com uma ou mais colunas; truncate/year/month/day/hour sao
  // aceitos na leitura sem poda por valor. Aceita "partition-specs" (formato
  // atual) e cai no legado "partition-spec" (lista de nomes) quando so ele
  // existe.
  std::int64_t default_spec = 0;
  if (const Value* ds = map_find(md, "default-spec-id"); ds && ds->kind == ValueKind::Inteiro) {
    default_spec = ds->i;
  }
  auto field_from_json = [&](const Value& f) {
    PartitionField pf;
    const Value* name = map_find(f, "name");
    const Value* fid = map_find(f, "field-id");
    const Value* sid = map_find(f, "source-id");
    const Value* tr = map_find(f, "transform");
    pf.name = name && name->kind == ValueKind::Texto ? name->s : "";
    pf.field_id = fid && fid->kind == ValueKind::Inteiro ? fid->i : 1000;
    pf.source_id = sid && sid->kind == ValueKind::Inteiro ? sid->i : 0;
    const std::string transform = tr && tr->kind == ValueKind::Texto ? tr->s : "";
    if (transform == "identity" || transform.empty()) {
      pf.transform = "identity";
    } else if (transform.rfind("bucket[", 0) == 0) {
      pf.transform = transform;
      // "bucket[16]" ou "bucket(16)" / "bucket" com num_buckets separado
      const std::size_t rb = transform.find(']');
      try {
        pf.num_buckets = rb == std::string::npos
                             ? 0
                             : std::stoi(transform.substr(7, rb - 7));
      } catch (const std::exception&) {
        pf.num_buckets = 0;
      }
      if (pf.num_buckets <= 0) {
        die("transform de particao '" + transform + "' com N invalido");
      }
      pf.avro_ty = "int";
    } else if (transform == "year" || transform == "month" || transform == "day" ||
               transform == "hour" || transform.rfind("truncate[", 0) == 0) {
      // Leitura sem poda por valor (o valor de particao e derivado e nao ha
      // coluna correspondente no schema); nunca dar die aqui.
      pf.transform = transform;
      pf.avro_ty = "int";
    } else if (transform == "void") {
      pf.transform = transform;
      pf.avro_ty = "int";
    } else {
      die("transform de particao '" + transform +
          "' nao suportada (suportadas: identity, bucket[N], truncate[W], year, month, day, hour)");
    }
    for (std::size_t k = 0; k < out.schema_cols.size(); ++k) {
      // o source-id e o field-id da coluna no schema (estavel entre versoes)
      if (out.schema_cols[k].id == pf.source_id) {
        if (pf.transform == "identity") pf.avro_ty = out.schema_cols[k].type;
        pf.source_name = out.schema_cols[k].name;
        pf.source_type = out.schema_cols[k].type;
      }
    }
    out.spec.push_back(std::move(pf));
  };
  bool found_spec = false;
  if (const Value* specs = map_find(md, "partition-specs");
      specs && specs->kind == ValueKind::Lista) {
    for (const Value& s : *specs->list) {
      const Value* sid = map_find(s, "spec-id");
      if (!sid || sid->kind != ValueKind::Inteiro || sid->i != default_spec) continue;
      found_spec = true;
      if (const Value* fields = map_find(s, "fields"); fields && fields->kind == ValueKind::Lista) {
        for (const Value& f : *fields->list) field_from_json(f);
      }
    }
  }
  if (!found_spec) {
    if (const Value* legacy = map_find(md, "partition-spec");
        legacy && legacy->kind == ValueKind::Lista) {
      for (const Value& n : *legacy->list) {
        if (n.kind != ValueKind::Texto) continue;
        PartitionField pf;
        pf.name = n.s;
        pf.field_id = 1000 + static_cast<std::int64_t>(out.spec.size());
        for (std::size_t k = 0; k < out.schema_cols.size(); ++k) {
          if (out.schema_cols[k].name == pf.name) {
            pf.source_id = static_cast<std::int64_t>(k + 1);
            pf.avro_ty = out.schema_cols[k].type;
          }
        }
        out.spec.push_back(std::move(pf));
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

  // versao pelo nome do arquivo (v<N>.metadata.json canonico; v<N>-<uuid>
  // legado ainda aceito na leitura)
  out.version = metadata_version_from_name(path);
  return md;
}

// le o metadata mais recente (maior versao; empate lexicografico). Comparar
// pela versao parseada, nao pela ordem lexicografica do nome: "v10" < "v2".
std::string latest_metadata_path(const std::string& dir, TableMeta& meta) {
  const std::string meta_dir = dir + "/metadata";
  const std::vector<std::string> files = list_metadata_files(meta_dir);
  if (files.empty()) {
    die("tabela nao existe em '" + dir + "' (use escrever_iceberg para criar)");
  }
  const std::string* best = &files.front();
  for (const std::string& f : files) {
    const std::int64_t v = metadata_version_from_name(f);
    const std::int64_t bv = metadata_version_from_name(*best);
    if (v > bv || (v == bv && f > *best)) best = &f;
  }
  parse_metadata(*best, meta);
  return *best;
}

// ---------------------------------------------------------------------------
// Manifests
// ---------------------------------------------------------------------------

// Schema do manifest entry com os campos do record "partition" do data_file.
// O schema Avro e dinamico: os fields do partition record seguem o spec da
// tabela (fase 26), com uniao ["null", T] por campo (null = partition value
// ausente; o tilt nao escreve nulos, mas le tabelas que os tenham).
std::string manifest_entry_schema_json(const std::vector<PartitionField>& spec) {
  std::string fields;
  for (std::size_t k = 0; k < spec.size(); ++k) {
    const PartitionField& pf = spec[k];
    fields += (k ? "," : "") + std::string("{\"name\":\"") + json_escape(pf.name) +
              "\",\"type\":[\"null\",\"" + pf.avro_ty +
              "\"],\"default\":null,\"field-id\":" + std::to_string(pf.field_id) + "}";
  }
  std::string s = kManifestEntrySchema;
  const std::string pos_name = "\"name\":\"partition\",\"fields\":[]";
  const std::size_t pos = s.find(pos_name);
  if (pos == std::string::npos) die("schema interno do manifest invalido (partition record)");
  s.replace(pos, pos_name.size(),
            "\"name\":\"partition\",\"fields\":[" + fields + "]");
  return s;
}

Value make_manifest_entry(int status, std::int64_t snapshot_id, const std::string& file_path,
                          std::int64_t record_count, std::int64_t file_size, int content,
                          const std::vector<PartitionField>& spec, const Value& part_map) {
  Value e = Value::mapa();
  e.map->set("status", Value::inteiro(status));
  e.map->set("snapshot_id", Value::inteiro(snapshot_id));
  e.map->set("sequence_number", Value::nulo());
  e.map->set("file_sequence_number", Value::nulo());
  Value df = Value::mapa();
  df.map->set("content", Value::inteiro(content));
  df.map->set("file_path", Value::texto(file_path));
  df.map->set("file_format", Value::texto("PARQUET"));
  Value part = Value::mapa();
  for (const PartitionField& pf : spec) {
    // valor no tipo da coluna (string/long/double/boolean) ou null
    const Value* v = part_map.map ? part_map.map->find(pf.name) : nullptr;
    part.map->set(pf.name, v ? *v : Value::nulo());
  }
  df.map->set("partition", std::move(part));
  df.map->set("record_count", Value::inteiro(record_count));
  df.map->set("file_size_in_bytes", Value::inteiro(file_size));
  df.map->set("sort_order_id", Value::nulo());
  e.map->set("data_file", std::move(df));
  return e;
}

struct FileInfo {
  std::string path;  // com file://
  std::int64_t records = 0;
  std::int64_t size = 0;
  int content = 0;   // 0 DATA, 1 POSITION DELETES, 2 EQUALITY DELETES
  Value part_map;  // valores de particao (tipados) por coluna; vazio = sem particao
};

std::string write_manifest(const std::string& meta_dir,
                           const std::vector<std::pair<int, std::string>>& changes,
                           std::int64_t snapshot_id, const std::vector<FileInfo>& files,
                           const std::vector<PartitionField>& spec) {
  const std::string schema_json = manifest_entry_schema_json(spec);
  Value schema;
  try {
    schema = json_parse(schema_json);
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
                                          info ? info->size : 0, info ? info->content : 0,
                                          spec, info ? info->part_map : Value::mapa()));
  }
  const std::string name = new_uuid() + "-m0.avro";
  const std::string path = meta_dir + "/" + name;
  const std::string bytes = ocf_write(schema, schema_json, records, {});
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) die("nao foi possivel gravar '" + path + "'");
  out << bytes;
  if (!out) die("falha ao gravar '" + path + "'");
  return name;
}

// Comparacao natural de valores de particao (min/max dos summaries do
// manifest list): numerico por valor, texto lexicografico, booleano 0<1.
bool partition_less(const Value& a, const Value& b) {
  const bool a_num = a.kind == ValueKind::Inteiro || a.kind == ValueKind::Decimal;
  const bool b_num = b.kind == ValueKind::Inteiro || b.kind == ValueKind::Decimal;
  if (a_num && b_num) return a.as_number() < b.as_number();
  if (a.kind == ValueKind::Texto && b.kind == ValueKind::Texto) return a.s < b.s;
  if (a.kind == ValueKind::Logico && b.kind == ValueKind::Logico) return !a.b && b.b;
  return false;  // tipos divergentes: nao reordena
}

// Serializacao binaria de um valor de particao para lower/upper_bound do
// manifest list (single-value serialization da spec): string UTF-8, int em
// 4 bytes little-endian, long e double em 8 bytes little-endian, boolean
// 1 byte.
std::string partition_bound_bytes(const PartitionField& pf, const Value& v) {
  std::string out;
  if (pf.avro_ty == "string") {
    return v.kind == ValueKind::Texto ? v.s : std::string();
  }
  if (pf.avro_ty == "boolean") {
    out += (v.kind == ValueKind::Logico && v.b) ? '\1' : '\0';
    return out;
  }
  if (pf.avro_ty == "int") {
    const std::int32_t x = v.kind == ValueKind::Inteiro
                               ? static_cast<std::int32_t>(v.i)
                               : static_cast<std::int32_t>(v.as_number());
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<char>((x >> (8 * k)) & 0xFF));
    return out;
  }
  if (pf.avro_ty == "double" || pf.avro_ty == "float") {
    const double d = v.as_number();
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    put_u64(out, bits);
    return out;
  }
  put_u64(out, static_cast<std::uint64_t>(v.kind == ValueKind::Inteiro
                                              ? v.i
                                              : static_cast<std::int64_t>(v.as_number())));
  return out;
}

std::string write_manifest_list(const std::string& meta_dir, const std::string& manifest_name,
                                std::int64_t snapshot_id, int added, int existing, int deleted,
                                std::int64_t added_rows, std::int64_t existing_rows,
                                const std::vector<FileInfo>& files,
                                const std::vector<PartitionField>& spec, int list_content = 0) {
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
  rec.map->set("content", Value::inteiro(list_content));  // 0 DATA, 1 DELETES
  // 1a passada: sem data sequence numbers reais (manifest entries os tem
  // nulos); 0 e o neutro e readers reais aceitam.
  rec.map->set("sequence_number", Value::inteiro(0));
  rec.map->set("min_sequence_number", Value::inteiro(0));
  rec.map->set("added_snapshot_id", Value::inteiro(snapshot_id));
  rec.map->set("added_files_count", Value::inteiro(added));
  rec.map->set("existing_files_count", Value::inteiro(existing));
  rec.map->set("deleted_files_count", Value::inteiro(deleted));
  rec.map->set("added_rows_count", Value::inteiro(added_rows));
  rec.map->set("existing_rows_count", Value::inteiro(existing_rows));
  rec.map->set("deleted_rows_count", Value::inteiro(0));
  // Summaries por campo de particao (field-id 507): readers reais (Spark)
  // consultam contains_null/lower/upper para podar manifests por predicado
  // de particao — lista vazia em tabela particionada estoura no evaluator.
  // Sem spec, null (como o writer real). Bounds em bytes single-value.
  Value parts = Value::nulo();
  if (!spec.empty()) {
    parts = Value::lista();
    for (const PartitionField& pf : spec) {
      bool contains_null = false;
      bool has = false;
      Value lower;
      Value upper;
      for (const FileInfo& f : files) {
        const Value* pv = f.part_map.map ? f.part_map.map->find(pf.name) : nullptr;
        if (!pv || pv->kind == ValueKind::Nulo) {
          contains_null = true;
          continue;
        }
        if (!has) {
          lower = upper = *pv;
          has = true;
          continue;
        }
        if (partition_less(*pv, lower)) lower = *pv;
        if (partition_less(upper, *pv)) upper = *pv;
      }
      Value s = Value::mapa();
      s.map->set("contains_null", Value::logico(contains_null));
      s.map->set("contains_nan", Value::nulo());
      s.map->set("lower_bound",
                 has ? Value::texto(partition_bound_bytes(pf, lower)) : Value::nulo());
      s.map->set("upper_bound",
                 has ? Value::texto(partition_bound_bytes(pf, upper)) : Value::nulo());
      parts.list->push_back(std::move(s));
    }
  }
  rec.map->set("partitions", std::move(parts));
  rec.map->set("key_metadata", Value::nulo());

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

std::string build_metadata_json(const std::string& dir, const std::string& uuid,
                                const std::vector<SchemaVer>& schemas,
                                std::int64_t current_schema_id, std::int64_t last_column_id,
                                const std::vector<PartitionField>& spec,
                                const std::vector<Snapshot>& snapshots,
                                std::vector<std::pair<std::int64_t, std::int64_t>> snapshot_log,
                                std::int64_t current_snapshot, std::int64_t last_updated) {
  std::string out = "{\n";
  out += "  \"format-version\": 2,\n";
  out += "  \"table-uuid\": \"" + json_escape(uuid) + "\",\n";
  out += "  \"location\": \"" + json_escape(dir) + "\",\n";
  // campos obrigatorios do spec v2 que readers reais (Spark/TableMetadataParser)
  // exigem: sequence numbers ainda nao sao atribuidos (tabela nova = 0) e a
  // tabela fica sem sort order (order vazio, como uma tabela recém-criada).
  out += "  \"last-sequence-number\": 0,\n";
  out += "  \"last-updated-ms\": " + std::to_string(last_updated) + ",\n";
  out += "  \"last-column-id\": " + std::to_string(last_column_id) + ",\n";
  // ids de campo de particao comecam em 1000 (spec); sem spec fica 999
  std::int64_t last_partition_id = 999;
  for (const PartitionField& pf : spec) last_partition_id = std::max(last_partition_id, pf.field_id);
  out += "  \"last-partition-id\": " + std::to_string(last_partition_id) + ",\n";
  out += "  \"schemas\": [\n";
  for (std::size_t k = 0; k < schemas.size(); ++k) {
    out += (k ? ",\n" : "");
    out += "    {\n      \"schema-id\": " + std::to_string(schemas[k].id) + ",\n      \"type\": \"struct\",\n";
    out += "      \"fields\": " + schema_json_fields(schemas[k].fields) + "\n    }";
  }
  out += "\n  ],\n";
  out += "  \"current-schema-id\": " + std::to_string(current_schema_id) + ",\n";
  // "partition-spec" legado (lista de nomes) + "partition-specs" atual com
  // field-id/source-id/transform — um reader real (pyiceberg) resolve ambos.
  out += "  \"partition-spec\": [";
  for (std::size_t k = 0; k < spec.size(); ++k) {
    out += (k ? ", " : "") + std::string("\"") + json_escape(spec[k].name) + "\"";
  }
  out += "],\n";
  out += "  \"partition-specs\": [\n    { \"spec-id\": 0, \"fields\": [";
  for (std::size_t k = 0; k < spec.size(); ++k) {
    const PartitionField& pf = spec[k];
    out += (k ? ", " : "") + std::string("{ \"field-id\": ") + std::to_string(pf.field_id) +
           ", \"source-id\": " + std::to_string(pf.source_id) + ", \"transform\": \"" +
           json_escape(pf.transform) + "\", \"name\": \"" + json_escape(pf.name) + "\" }";
  }
  out += "] }\n  ],\n";
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
  out += "  \"metadata-log\": [],\n";
  out += "  \"sort-orders\": [ { \"order-id\": 0, \"fields\": [] } ],\n";
  out += "  \"default-sort-order-id\": 0\n";
  out += "}\n";
  return out;
}

void commit_metadata(const std::string& dir, std::int64_t version, const std::string& content) {
  const std::string meta_dir = dir + "/metadata";
  // Nome canonico do HadoopCatalog: o HadoopTableOperations do Iceberg/Spark
  // resolve "v<N>.metadata.json" (regex v([^\..*]) + parseInt do numero e
  // procura exata do arquivo). O layout antigo v<N>-<uuid>.metadata.json
  // segue aceito na leitura (metadata_version_from_name).
  const std::string final_path =
      meta_dir + "/v" + std::to_string(version) + ".metadata.json";
  const std::string tmp_path =
      meta_dir + "/.commit-" + std::to_string(tilt::rt::tilt_getpid()) + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out) die("nao foi possivel gravar '" + tmp_path + "'");
    out << content;
    out.flush();
    if (!out) die("falha ao gravar '" + tmp_path + "'");
  }
  if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    die("falha ao commitar o metadata em '" + final_path + "'");
  }
  // version-hint.txt: ponteiro de versao best-effort, como o HadoopCatalog
  // real grava — readers (Spark) leem o hint antes de listar o diretorio.
  // Falha aqui nao invalida o commit (o fallback de listagem cobre).
  std::ofstream hint(meta_dir + "/version-hint.txt", std::ios::trunc);
  if (hint) hint << version;
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

// Entrada de manifest: caminho do data file + record `partition` (mapa com o
// valor de particao por nome de coluna; vazio = sem particao) + contagens.
// `content`: 0 = DATA, 1 = POSITION DELETES, 2 = EQUALITY DELETES (Fase 12-5a).
struct ActiveEntry {
  int status = 0;
  int content = 0;
  std::string path;
  Value partition;
  std::int64_t records = 0;
  std::int64_t size = 0;
};

// coleta (status, content, file_path, partition) dos manifests de um snapshot
void collect_manifest_entries(const Snapshot& snap, std::vector<ActiveEntry>& out) {
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
      ActiveEntry entry;
      entry.status = static_cast<int>(status->i);
      entry.path = fp->s;
      const Value* ct = map_find(*df, "content");
      entry.content = ct && ct->kind == ValueKind::Inteiro ? static_cast<int>(ct->i) : 0;
      const Value* part = map_find(*df, "partition");
      entry.partition = (part && part->kind == ValueKind::Mapa) ? *part : Value::mapa();
      const Value* rc = map_find(*df, "record_count");
      entry.records = rc && rc->kind == ValueKind::Inteiro ? rc->i : 0;
      const Value* fs = map_find(*df, "file_size_in_bytes");
      entry.size = fs && fs->kind == ValueKind::Inteiro ? fs->i : 0;
      out.push_back(std::move(entry));
    }
  }
}

// arquivos ativos no snapshot corrente: percorre a cadeia de pais do mais
// antigo para o mais novo aplicando adds menos removes (remove = status 2).
// Entradas com content 1/2 (position/equality deletes, Fase 12-5a) vao para
// `deletes`; as de dados (content 0) para `data`.
struct ResolvedFiles {
  std::vector<ActiveEntry> data;
  std::vector<ActiveEntry> deletes;
};
ResolvedFiles resolve_files(const TableMeta& meta) {
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

  ResolvedFiles out;
  auto apaga_por_path = [](std::vector<ActiveEntry>& v, const std::string& path) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const ActiveEntry& a) { return a.path == path; }),
            v.end());
  };
  auto contem = [](const std::vector<ActiveEntry>& v, const std::string& path) {
    return std::find_if(v.begin(), v.end(),
                        [&](const ActiveEntry& a) { return a.path == path; }) != v.end();
  };
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    std::vector<ActiveEntry> entries;
    collect_manifest_entries(**it, entries);
    for (const ActiveEntry& entry : entries) {
      std::vector<ActiveEntry>& alvo = entry.content == 0 ? out.data : out.deletes;
      if (entry.status == 2) {  // DELETED
        apaga_por_path(alvo, entry.path);
      } else {  // ADDED (1) ou EXISTING (0)
        if (!contem(alvo, entry.path)) alvo.push_back(entry);
      }
    }
  }
  return out;
}

std::vector<ActiveEntry> resolve_active_files(const TableMeta& meta) {
  return resolve_files(meta).data;
}

std::string strip_scheme(const std::string& path) {
  return path.rfind("file://", 0) == 0 ? path.substr(7) : path;
}

// Grava os data files de uma escrita/anexo. Sem spec, um unico arquivo
// <dir>/data/<uuid>.parquet com a tabela inteira (comportamento original);
// com spec, um arquivo por combinacao de valores em
// <dir>/data/<c1>=<v1>/<c2>=<v2>/00000-0-<uuid>.parquet (naming iceberg:
// <partition-path>/<file>.parquet), sem as colunas de particao no parquet.
// `ids_by_name` (append): field-ids do schema iceberg corrente por nome —
// colunas novas de uma evolucao de schema levam o id novo no parquet;
// sem ele, os ids seguem a posicao na 1a linha (escrita original).
std::vector<FileInfo> write_data_files(const std::string& dir, const Value& tabela,
                                       const std::vector<PartitionField>* spec,
                                       const std::vector<Column>* ids_by_name) {
  std::vector<FileInfo> out;
  std::vector<PartitionGroup> grupos;
  // ids das colunas no schema iceberg: arvore com field-ids em profundidade;
  // os field_ids do parquet seguem esses ids na ordem das folhas (a coluna
  // de particao identity fica fora do parquet mas mantem o id).
  // Nomes top-level removidos do parquet (identity).
  std::vector<std::string> removidas;
  if (spec != nullptr) {
    for (const PartitionField& pf : *spec) {
      if (pf.transform == "identity") removidas.push_back(pf.name);
    }
  }
  std::vector<Column> arvore_ids;
  if (spec != nullptr && ids_by_name == nullptr) {
    arvore_ids = table_columns(tabela, "escrever_iceberg");
    std::int64_t proximo = 0;
    assign_ids(arvore_ids, proximo);
  }
  auto resolve_folhas = [&](const std::string& name, std::vector<int>& dst) {
    const std::vector<Column>& base =
        ids_by_name != nullptr ? *ids_by_name : arvore_ids;
    for (const Column& c : base) {
      if (c.name != name) continue;
      std::vector<int> tmp;
      leaf_ids(std::vector<Column>{c}, tmp);  // struct entra com o proprio id
      if (tmp.empty() || tmp.front() <= 0) {
        die("coluna '" + name + "' sem field-id no schema (evolucao de schema inconsistente)");
      }
      dst.insert(dst.end(), tmp.begin(), tmp.end());
      return;
    }
    die("coluna '" + name + "' fora do schema (evolucao de schema inconsistente)");
  };
  if (spec != nullptr) {
    grupos = partition_rows(tabela, *spec);
  } else {
    PartitionGroup g;
    g.rows = tabela;
    grupos.push_back(std::move(g));
  }
  for (const PartitionGroup& g : grupos) {
    std::string subdir = dir + "/data";
    std::string nome;
    if (spec == nullptr) {
      // nomenclatura historica (uuid) preservada para tabelas sem particao
      nome = new_uuid() + ".parquet";
    } else {
      subdir += "/" + g.key;
      mkdir_p(subdir);
      nome = "00000-0-" + new_uuid() + ".parquet";
    }
    const std::string path = subdir + "/" + nome;
    std::vector<int> field_ids;
    if (spec != nullptr || ids_by_name != nullptr) {
      const Value& first = g.rows.list->front();
      for (const auto& kv : first.map->items) {
        if (std::find(removidas.begin(), removidas.end(), kv.first) != removidas.end()) {
          continue;  // identity fora do parquet
        }
        resolve_folhas(kv.first, field_ids);
      }
    }
    parquet_write(path, g.rows, field_ids.empty() ? nullptr : &field_ids);
    const std::int64_t size = file_size(path);
    if (size <= 0) die("falha ao gravar '" + path + "'");
    FileInfo info;
    info.path = "file://" + path;
    info.records = static_cast<std::int64_t>(g.rows.list->size());
    info.size = size;
    if (spec != nullptr) info.part_map = g.part_map;
    out.push_back(std::move(info));
  }
  return out;
}

// Converte o valor do record `partition` para o tipo declarado no schema
// (o Avro ja devolve tipado; texto divergente e convertido, senao mantido).
Value partition_rehydrate(const Value& v, const std::string& iceberg_type) {
  if (v.kind != ValueKind::Texto) return v;
  try {
    if (iceberg_type == "long" || iceberg_type == "int") {
      return Value::inteiro(std::stoll(v.s));
    }
    if (iceberg_type == "double" || iceberg_type == "float") {
      return Value::decimal(std::stod(v.s));
    }
    if (iceberg_type == "boolean") {
      if (v.s == "true") return Value::logico(true);
      if (v.s == "false") return Value::logico(false);
    }
  } catch (const std::exception&) {
    // fora de formato/alcance: mantem texto (decisao da fase 26)
  }
  return v;
}

// Resultado da validacao de schema de um append (evolucao de schema, fase 27).
struct MergedSchema {
  std::vector<Column> cols;  // schema resultante: antigas (ids estáveis) + novas no fim
  bool evolved = false;
  std::int64_t last_column_id = 0;
};

// Validacao por nome: toda coluna do schema atual precisa existir na tabela
// anexada (remover coluna -> erro); coluna em comum precisa ter o mesmo
// tipo; colunas novas entram optional no fim com id novo (proximo livre
// apos last-column-id). Ordem das colunas antigas e livre — a projecao dos
// parquet na leitura e por nome/field-id.
MergedSchema merge_append_schema(const TableMeta& meta, const Value& tabela) {
  const std::vector<Column> got = table_columns(tabela, "anexar_iceberg");
  MergedSchema merged;
  merged.cols = meta.schema_cols;
  merged.last_column_id = meta.last_column_id;
  if (merged.last_column_id <= 0) {
    std::function<void(const std::vector<Column>&)> max_id = [&](const std::vector<Column>& cs) {
      for (const Column& c : cs) {
        merged.last_column_id = std::max(merged.last_column_id, c.id);
        if (c.is_struct) max_id(c.children);
      }
    };
    max_id(merged.cols);
  }
  // Compara recursiva: struct casa com struct (filhos por nome, ids
  // estaveis); escalar casa por tipo. Nova (sub)coluna entra optional no fim
  // com id novo. Tipos escalares top-level: deduzidos da tabela (1o nao
  // nulo, como antes); aninhados: ja deduzidos em table_columns.
  std::function<void(std::vector<Column>&, const std::vector<Column>&, const std::string&)> mescla =
      [&](std::vector<Column>& velhas, const std::vector<Column>& novas, const std::string& ctx) {
        for (Column& old : velhas) {
          const Column* ty = nullptr;
          for (const Column& nt : novas) {
            if (nt.name == old.name) ty = &nt;
          }
          if (!ty) {
            die("anexar_iceberg: coluna '" + ctx + old.name +
                "' ausente na tabela anexada (evolucao de schema suporta apenas adicao de "
                "colunas)");
          }
          if (old.is_struct || ty->is_struct) {
            if (!old.is_struct || !ty->is_struct) {
              die("anexar_iceberg: coluna '" + ctx + old.name +
                  "' com tipo divergente (struct x escalar)");
            }
            mescla(old.children, ty->children, ctx + old.name + ".");
            continue;
          }
          std::string ty_s;
          if (ctx.empty()) {
            for (const Value& row : *tabela.list) {
              if (row.kind != ValueKind::Mapa || !row.map) break;
              const Value* cell = row.map->find(ty->name);
              if (cell && cell->kind != ValueKind::Nulo) {
                ty_s = iceberg_type_name(*cell);
                break;
              }
            }
          } else {
            ty_s = ty->type;
          }
          if (!ty_s.empty() && ty_s != old.type) {
            die("anexar_iceberg: coluna '" + ctx + old.name + "' com tipo divergente (esperado " +
                old.type + "; recebido " + ty_s +
                ") (evolucao de schema suporta apenas adicao de colunas)");
          }
        }
        for (const Column& x : novas) {
          bool known = false;
          for (const Column& o : velhas) {
            if (o.name == x.name) known = true;
          }
          if (known) continue;
          Column c = x;
          if (c.type.empty()) c.type = "string";
          c.id = ++merged.last_column_id;
          c.required = false;  // nova e optional (linhas antigas ficam nulas)
          if (c.is_struct) {
            std::function<void(Column&)> renumera = [&](Column& y) {
              for (Column& ch : y.children) {
                if (ch.type.empty()) ch.type = "string";
                ch.id = ++merged.last_column_id;
                ch.required = false;
                if (ch.is_struct) renumera(ch);
              }
            };
            renumera(c);
          }
          velhas.push_back(std::move(c));
          merged.evolved = true;
        }
      };
  mescla(merged.cols, got, "");
  return merged;
}

// ---------------------------------------------------------------------------
// Nucleos compartilhados pelos modos Hadoop (local) e REST catalog (fase 29):
// write_core/append_core fazem todo o trabalho de escrita local (data files,
// manifest, manifest list e metadata JSON) e read_core le a partir de um
// TableMeta ja carregado. O modo Hadoop so carrega o metadata do diretorio e
// commita; o modo REST troca essas duas pontas por chamadas ao catalogo.
// ---------------------------------------------------------------------------

struct WriteCore {
  Snapshot snap;
  std::vector<Column> cols;
  std::vector<PartitionField> spec_fields;
  std::string json;
};

// `schema_id` e o id sob o qual o schema desta escrita e registrado (0 na
// criacao; max+1 quando se sobrescreve uma tabela REST existente).
WriteCore write_core(const std::string& dir, const Value& tabela,
                     const std::vector<std::string>& part_cols, std::int64_t schema_id) {
  WriteCore wc;
  wc.cols = table_columns(tabela, "escrever_iceberg");
  // ids em profundidade (struct recebe id, depois os filhos); as colunas
  // fonte da particao ficam optional no metadata (padrao Spark — readers
  // reais so reidratam campo optional)
  std::int64_t proximo = 0;
  assign_ids(wc.cols, proximo);
  for (std::size_t k = 0; k < wc.cols.size(); ++k) {
    wc.cols[k].required = std::find(part_cols.begin(), part_cols.end(), wc.cols[k].name) ==
                          part_cols.end();
  }
  const std::vector<PartitionField> spec = make_spec(wc.cols, part_cols, "escrever_iceberg");
  const std::vector<PartitionField>* pspec = spec.empty() ? nullptr : &spec;
  mkdir_if_missing(dir);
  const std::string meta_dir = dir + "/metadata";
  mkdir_if_missing(meta_dir);
  mkdir_if_missing(dir + "/data");

  const std::vector<FileInfo> datas = write_data_files(dir, tabela, pspec, &wc.cols);

  std::vector<std::pair<int, std::string>> changes;
  for (const FileInfo& f : datas) changes.emplace_back(1, f.path);

  wc.spec_fields = spec;

  const std::int64_t snapshot_id = new_snapshot_id();
  const std::int64_t ts = now_ms();

  const std::string manifest_name =
      write_manifest(meta_dir, changes, snapshot_id, datas, wc.spec_fields);
  std::int64_t added_rows = 0;
  for (const FileInfo& f : datas) added_rows += f.records;
  const std::string list_path =
      write_manifest_list(meta_dir, manifest_name, snapshot_id,
                          static_cast<int>(datas.size()), 0, 0, added_rows, 0, datas,
                          wc.spec_fields);

  wc.snap.id = snapshot_id;
  wc.snap.ts = ts;
  wc.snap.operation = "overwrite";
  wc.snap.manifest_list = list_path;
  wc.snap.has_parent = false;
  wc.snap.parent = -1;

  wc.json = build_metadata_json(dir, new_uuid(), {SchemaVer{schema_id, wc.cols}}, schema_id,
                                proximo, wc.spec_fields,
                                {wc.snap}, {{ts, snapshot_id}}, snapshot_id, ts);
  return wc;
}

struct AppendCore {
  Snapshot snap;
  std::vector<Column> cols;  // schema resultante (merged)
  std::vector<SchemaVer> schemas;
  std::int64_t current_schema_id = 0;
  bool evolved = false;
  std::int64_t last_column_id = 0;
  std::int64_t version = 0;
  std::string json;
};

AppendCore append_core(const std::string& dir, const std::string& dir_display,
                       const TableMeta& meta, const Value& tabela,
                       const std::vector<std::string>& part_cols_req) {
  AppendCore ac;
  // Validacao de schema por nome com merge para evolucao (fase 27): colunas
  // novas entram optional no fim com id novo; remocao/tipo divergente -> erro.
  const MergedSchema merged = merge_append_schema(meta, tabela);
  ac.cols = merged.cols;

  // Particao: herda o spec da tabela existente; erro se explicita e diverge
  // (a ordem das colunas importa). Mensagens com o dir digitado pelo usuario
  // (dir_display), nao com a location absoluta usada na gravacao.
  const std::vector<PartitionField>* pspec = nullptr;
  std::vector<PartitionField> spec_fields;
  if (!meta.spec.empty()) {
    pspec = &meta.spec;
    spec_fields = meta.spec;
    std::vector<std::string> nomes;
    for (const PartitionField& pf : meta.spec) nomes.push_back(pf.name);
    if (!part_cols_req.empty()) {
      // Normaliza o pedido ("bucket[4](id)" -> "id_bucket_4") antes de
      // comparar com os nomes derivados do spec corrente.
      std::vector<std::string> pedidos;
      for (const std::string& p : part_cols_req) {
        const auto [bsrc, n] = parse_bucket(p);
        pedidos.push_back(bsrc.empty() ? p : bsrc + "_bucket_" + std::to_string(n));
      }
      if (pedidos != nomes) {
        die("anexar_iceberg: tabela em '" + dir_display + "' ja e particionada por " +
            join_part_cols(nomes) + " (recebido particionar_por: " +
            join_part_cols(part_cols_req) + ")");
      }
    }
    // as colunas de particao precisam existir na tabela anexada (schema ja
    // foi validado acima, entao so falta o valor em si — validado ao agrupar)
  } else if (!part_cols_req.empty()) {
    die("anexar_iceberg: tabela em '" + dir_display +
        "' nao e particionada — recrie-a com escrever_iceberg tabela, \"" + dir_display +
        "\", particionar_por: \"" + part_cols_req.front() + "\"");
  }

  mkdir_if_missing(dir + "/data");

  // Arquivos ja ativos: entram no novo manifest como EXISTING (status 0),
  // como faz um append rapido de writer real — um reader como pyiceberg so
  // enxerga os manifests do snapshot corrente e nao anda a cadeia de pais.
  const std::vector<ActiveEntry> previous =
      meta.current_snapshot >= 0 ? resolve_active_files(meta) : std::vector<ActiveEntry>{};

  const std::vector<FileInfo> datas = write_data_files(dir, tabela, pspec, &merged.cols);

  std::vector<std::pair<int, std::string>> changes;
  std::vector<FileInfo> infos;
  std::int64_t existing_rows = 0;
  for (const ActiveEntry& e : previous) {
    changes.emplace_back(0, e.path);
    FileInfo info;
    info.path = e.path;
    info.records = e.records;
    info.size = e.size;
    info.part_map = e.partition;
    infos.push_back(std::move(info));
    existing_rows += e.records;
  }
  std::int64_t added_rows = 0;
  for (const FileInfo& f : datas) {
    changes.emplace_back(1, f.path);
    infos.push_back(f);
    added_rows += f.records;
  }

  const std::int64_t snapshot_id = new_snapshot_id();
  const std::int64_t ts = now_ms();

  const std::string meta_dir = dir + "/metadata";
  const std::string manifest_name =
      write_manifest(meta_dir, changes, snapshot_id, infos, spec_fields);
  const std::string list_path = write_manifest_list(
      meta_dir, manifest_name, snapshot_id, static_cast<int>(datas.size()),
      static_cast<int>(previous.size()), 0, added_rows, existing_rows, infos, spec_fields);

  ac.snap.id = snapshot_id;
  ac.snap.ts = ts;
  ac.snap.operation = "append";
  ac.snap.manifest_list = list_path;
  ac.snap.parent = meta.current_snapshot;
  ac.snap.has_parent = meta.current_snapshot >= 0;

  std::vector<Snapshot> snapshots = meta.snapshots;
  snapshots.push_back(ac.snap);
  std::vector<std::pair<std::int64_t, std::int64_t>> log = meta.snapshot_log;
  log.emplace_back(ts, snapshot_id);

  ac.schemas = meta.schemas;
  if (ac.schemas.empty()) {
    // metadata legado/externo sem "schemas": registra o deduzido como v0
    ac.schemas.push_back(SchemaVer{0, merged.cols});
  }
  ac.current_schema_id = meta.current_schema_id;
  ac.evolved = merged.evolved;
  if (merged.evolved) {
    std::int64_t max_id = 0;
    for (const SchemaVer& s : ac.schemas) max_id = std::max(max_id, s.id);
    ac.current_schema_id = max_id + 1;
    ac.schemas.push_back(SchemaVer{ac.current_schema_id, merged.cols});
  }

  ac.last_column_id = merged.last_column_id;
  ac.version = meta.version < 0 ? 0 : meta.version + 1;
  // uuid estavel por tabela (spec v2): reaproveita o do metadata corrente
  ac.json = build_metadata_json(dir, meta.uuid.empty() ? new_uuid() : meta.uuid, ac.schemas,
                                ac.current_schema_id, merged.last_column_id, spec_fields,
                                snapshots, log, snapshot_id, ts);
  return ac;
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

// Predicados de `onde` separados em: (a) pruning — colunas do partition
// spec da tabela, comparam contra o record `partition` dos manifests e pulam
// data file inteiro; (b) residual — filtra linhas apos a reidratacao.
struct OndeFilter {
  std::vector<std::pair<std::string, Value>> prune;
  std::vector<std::pair<std::string, Value>> residual;
};

OndeFilter split_onde(const Value* onde, const std::vector<PartitionField>& spec) {
  OndeFilter f;
  if (onde && onde->kind == ValueKind::Mapa && onde->map) {
    for (const auto& kv : onde->map->items) {
      // Predicado direto no nome do campo de particao (ex.: ano=2024).
      bool eh_particao = false;
      for (const PartitionField& pf : spec) {
        if (pf.name == kv.first) eh_particao = true;
      }
      // Predicado na coluna de ORIGEM de um transform: aplica o transform ao
      // valor e poda pelo campo de particao correspondente. O predicado original
      // segue como residual para confirmacao exata.
      for (const PartitionField& pf : spec) {
        if (pf.transform == "identity" || pf.source_name != kv.first) continue;
        try {
          Value transformed = apply_transform(kv.second, pf);
          f.prune.emplace_back(pf.name, std::move(transformed));
        } catch (const std::exception&) {
          // valor incompativel (ex.: nulo): sem poda, segue como residual
        }
        break;
      }
      if (eh_particao) {
        f.prune.push_back(kv);
      } else {
        f.residual.push_back(kv);
      }
    }
  }
  return f;
}

// Data file passa no pruning se bater com TODOS os predicados de particao
// (valor ausente/nulo no record `partition` nunca bate igualdade).
bool passa_pruning(const ActiveEntry& e, const std::vector<std::pair<std::string, Value>>& prune,
                   const std::vector<PartitionField>& spec) {
  for (const auto& [col, pred] : prune) {
    const Value* pv = e.partition.map ? e.partition.map->find(col) : nullptr;
    if (!pv || pv->kind == ValueKind::Nulo) return false;
    // texto divergente e convertido pelo tipo declarado no spec (tabelas de
    // outros escritores podem serializar o valor de particao como string)
    std::string ty = "string";
    for (const PartitionField& pf : spec) {
      if (pf.name == col) ty = pf.avro_ty;
    }
    if (!pred_eq(partition_rehydrate(*pv, ty), pred)) return false;
  }
  return true;
}

// Deletes da Fase 12-5a, carregados dos manifests do snapshot corrente:
// - position: arquivo {file_path, pos} -> linhas apagadas por (arquivo,
//   indice fisico no arquivo);
// - equality: linhas-chave; apaga a linha de dados quando TODAS as colunas
//   em comum batem (pred_eq) — exige ao menos 1 coluna em comum.
struct LoadedDeletes {
  std::map<std::string, std::set<std::int64_t>> pos;  // path sem scheme -> posicoes
  std::vector<Value> eq;                              // linhas de equality delete
};

LoadedDeletes load_deletes(const std::vector<ActiveEntry>& deletes) {
  LoadedDeletes out;
  for (const ActiveEntry& d : deletes) {
    const std::string path = strip_scheme(d.path);
    Value chunk;
    try {
      chunk = parquet_read(path);
    } catch (const std::exception& e) {
      die("arquivo de delete '" + path + "' ilegivel: " + e.what());
    }
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) continue;
    if (d.content == 1) {
      for (const Value& row : *chunk.list) {
        if (row.kind != ValueKind::Mapa || !row.map) continue;
        const Value* fp = row.map->find("file_path");
        const Value* ps = row.map->find("pos");
        if (!fp || fp->kind != ValueKind::Texto || !ps || !ps->is_number()) continue;
        out.pos[strip_scheme(fp->s)].insert(static_cast<std::int64_t>(ps->as_number()));
      }
    } else if (d.content == 2) {
      for (const Value& row : *chunk.list) {
        if (row.kind == ValueKind::Mapa && row.map) out.eq.push_back(row);
      }
    }
  }
  return out;
}

// Linha de dados apagada por position ou equality delete?
bool apagada_por_delete(const std::string& fpath_norm, std::int64_t pos, const Value& row,
                        const LoadedDeletes& dels) {
  const auto itp = dels.pos.find(fpath_norm);
  if (itp != dels.pos.end() && itp->second.count(pos)) return true;
  if (row.kind != ValueKind::Mapa || !row.map) return false;
  for (const Value& d : dels.eq) {
    if (d.kind != ValueKind::Mapa || !d.map) continue;
    int compartilhadas = 0;
    bool bate = true;
    for (const auto& kv : d.map->items) {
      const Value* cell = row.map->find(kv.first);
      if (!cell) continue;
      ++compartilhadas;
      if (!pred_eq(*cell, kv.second)) {
        bate = false;
        break;
      }
    }
    if (bate && compartilhadas > 0) return true;
  }
  return false;
}

// Projeta a linha no schema (union-by-name recursivo, Marco 2 / B5):
// coluna ausente -> Nulo (evolucao); struct casa por nome dos campos;
// chave fora do schema -> die.
Value project_schema(const Value& row, const std::vector<Column>& schema, const std::string& path,
                     const std::string& fpath) {
  Value m = Value::mapa();
  for (const Column& c : schema) {
    const Value* cell = (row.kind == ValueKind::Mapa && row.map) ? row.map->find(c.name) : nullptr;
    if (c.is_struct) {
      if (!cell || cell->kind == ValueKind::Nulo) {
        m.map->set(c.name, Value::nulo());
      } else if (cell->kind != ValueKind::Mapa || !cell->map) {
        die("schema divergente em '" + fpath + "' (coluna '" + path + c.name + "' nao e struct)");
      } else {
        m.map->set(c.name, project_schema(*cell, c.children, path + c.name + ".", fpath));
      }
      continue;
    }
    m.map->set(c.name, cell ? *cell : Value::nulo());
  }
  if (row.kind == ValueKind::Mapa && row.map) {
    for (const auto& kv : row.map->items) {
      bool known = false;
      for (const Column& c : schema) {
        if (c.name == kv.first) known = true;
      }
      if (!known) {
        die("schema divergente em '" + fpath + "' (coluna '" + path + kv.first +
            "' fora do metadata)");
      }
    }
  }
  return m;
}

// Filtro residual sobre a linha final (compartilhado entre leitura e delete).
bool residual_match(const Value& row,
                    const std::vector<std::pair<std::string, Value>>& residual) {
  for (const auto& [col, val] : residual) {
    const Value* cell = row.map ? row.map->find(col) : nullptr;
    if (!pred_eq(cell ? *cell : Value::nulo(), val)) return false;
  }
  return true;
}

// Projeta a linha bruta do parquet no schema corrente (mesma regra da
// leitura: reidrata particao identity, nulo no resto; structs por nome).
Value project_row(const ActiveEntry& f, const Value& row, const TableMeta& meta) {
  if (!meta.spec.empty() && !meta.schema_cols.empty()) {
    Value base = Value::mapa();
    if (row.kind == ValueKind::Mapa && row.map) {
      for (const auto& kv : row.map->items) base.map->set(kv.first, kv.second);
    }
    for (const Column& c : meta.schema_cols) {
      if (c.is_struct) continue;  // particao nunca e struct
      if (!base.map->find(c.name)) {
        const Value* pv = f.partition.map ? f.partition.map->find(c.name) : nullptr;
        base.map->set(c.name, pv ? partition_rehydrate(*pv, c.type) : Value::nulo());
      }
    }
    const std::string path = strip_scheme(f.path);
    return project_schema(base, meta.schema_cols, "", path);
  }
  if (!meta.schema_cols.empty()) {
    const std::string path = strip_scheme(f.path);
    return project_schema(row, meta.schema_cols, "", path);
  }
  return row;
}

Value read_core(const TableMeta& meta, const Value* onde) {
  const OndeFilter filtro = split_onde(onde, meta.spec);
  const ResolvedFiles resolvidos = resolve_files(meta);
  std::vector<ActiveEntry> active = resolvidos.data;
  const LoadedDeletes dels = load_deletes(resolvidos.deletes);
  // Pruning: data file so entra se bater com todos os predicados de particao.
  if (!filtro.prune.empty()) {
    active.erase(std::remove_if(active.begin(), active.end(),
                                [&](const ActiveEntry& e) {
                                  return !passa_pruning(e, filtro.prune, meta.spec);
                                }),
                 active.end());
  }
  if (active.empty()) {
    // sem match no pruning -> tabela vazia (sem predicados de particao,
    // continua sendo erro: nao ha data file ativo nenhum)
    if (!filtro.prune.empty()) return Value::tabela();
    die("tabela em '" + meta.location + "' esta vazia (nenhum data file ativo)");
  }

  // Filtro residual aplicado sobre a linha final (reidratada ou legado).
  auto passa_residual = [&](const Value& row) {
    for (const auto& [col, val] : filtro.residual) {
      const Value* cell = row.map ? row.map->find(col) : nullptr;
      if (!pred_eq(cell ? *cell : Value::nulo(), val)) return false;
    }
    return true;
  };

  // Tabela particionada: reidrata as colunas de particao a partir do record
  // `partition` dos manifests, na ordem do schema do metadata. Deletes
  // (position por indice fisico, equality por valor) filtram antes do
  // residual.
  if (!meta.spec.empty() && !meta.schema_cols.empty()) {
    Value out = Value::tabela();
    for (const ActiveEntry& f : active) {
      const std::string path = strip_scheme(f.path);
      Value chunk = parquet_read(path);
      if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
        die("arquivo '" + path + "' nao e uma tabela parquet");
      }
      std::int64_t pos = 0;
      for (Value& row : *chunk.list) {
        if (row.kind != ValueKind::Mapa || !row.map) {
          die("linha de '" + path + "' nao e um mapa");
        }
        const Value m = project_row(f, row, meta);
        const bool apagada = apagada_por_delete(path, pos, m, dels);
        ++pos;
        if (apagada) continue;
        if (passa_residual(m)) out.list->push_back(m);
      }
    }
    return out;
  }

  // Sem particao, com schema no metadata: projeta no schema corrente
  // (union-by-name — arquivo de antes da evolucao fica sem a coluna nova e
  // a projecao devolve nulo nas linhas dele).
  if (!meta.schema_cols.empty()) {
    Value out = Value::tabela();
    for (const ActiveEntry& f : active) {
      const std::string path = strip_scheme(f.path);
      Value chunk = parquet_read(path);
      if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
        die("arquivo '" + path + "' nao e uma tabela parquet");
      }
      std::int64_t pos = 0;
      for (Value& row : *chunk.list) {
        if (row.kind != ValueKind::Mapa || !row.map) {
          die("linha de '" + path + "' nao e um mapa");
        }
        const Value m = project_schema(row, meta.schema_cols, "", path);
        const bool apagada = apagada_por_delete(path, pos, m, dels);
        ++pos;
        if (apagada) continue;
        if (passa_residual(m)) out.list->push_back(m);
      }
    }
    return out;
  }

  // Metadata legado sem "schemas": deducao posicional a partir do 1o arquivo.
  Value out = Value::tabela();
  std::vector<Column> schema_cols;
  for (const ActiveEntry& f : active) {
    const std::string path = strip_scheme(f.path);
    Value chunk = parquet_read(path);
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) {
      die("arquivo '" + path + "' nao e uma tabela parquet");
    }
    std::int64_t pos = 0;
    for (Value& row : *chunk.list) {
      if (row.kind != ValueKind::Mapa || !row.map) die("linha de '" + path + "' nao e um mapa");
      if (schema_cols.empty()) {
        for (const auto& kv : row.map->items) {
          Column nc;
          nc.name = kv.first;
          nc.type = iceberg_type_name(kv.second);
          nc.sample = kv.second;
          schema_cols.push_back(std::move(nc));
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
      const bool apagada = apagada_por_delete(path, pos, row, dels);
      ++pos;
      if (apagada) continue;
      if (passa_residual(row)) out.list->push_back(std::move(row));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// REST catalog (fase 29) — opt-in via ICEBERG_CATALOG=rest + ICEBERG_URI.
// Subconjunto do Iceberg REST Open API (prefixo v1, namespace "default"):
//   - loadTable:    GET  /v1/namespaces/default/tables/<tabela>
//   - createTable:  POST /v1/namespaces/default/tables
//   - commit:       POST /v1/namespaces/default/tables/<tabela>/transactions
// O argumento `dir` continua sendo o diretorio local da tabela (a location,
// enviada como file:// no createTable); o nome da tabela no catalogo e o
// basename desse caminho. O tilt segue gravando data files/manifests/metadata
// localmente em <location> e referencia as locations nos updates — downloads
// de metadata so ocorrem na leitura (metadata-location do loadTable).
// Subconjunto de updates usado no commit (requirements + updates):
//   - 1o write (tabela nova):  assert-current-snapshot-id(-1) +
//     [upgrade-format-version(2), set-location, set-properties, add-snapshot,
//      set-snapshot-ref(main)];
//   - append:                  assert-current-snapshot-id(atual) +
//     [add-snapshot, set-snapshot-ref] (+ add-schema com last-column-id e
//      set-current-schema quando o schema evoluiu);
//   - sobrescrita de tabela existente: assert-current-snapshot-id(atual) +
//     [remove-snapshot-ref(main), remove-snapshots(antigos), add-schema,
//      set-current-schema, add-snapshot, set-snapshot-ref] (o partition spec
//      e mantido; divergencia de particionamento -> erro claro).
// ---------------------------------------------------------------------------

struct RestCfg {
  bool ativo = false;
  std::string uri;  // sem barra final
};

RestCfg rest_cfg() {
  RestCfg cfg;
  const char* cat = std::getenv("ICEBERG_CATALOG");
  if (!cat || std::string(cat) != "rest") return cfg;
  const char* uri = std::getenv("ICEBERG_URI");
  if (!uri || !*uri) {
    die("ICEBERG_CATALOG=rest exige ICEBERG_URI (ex.: ICEBERG_URI=http://localhost:8181)");
  }
  cfg.ativo = true;
  cfg.uri = uri;
  while (!cfg.uri.empty() && cfg.uri.back() == '/') cfg.uri.pop_back();
  return cfg;
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    out += c == '\'' ? "'\\''" : std::string(1, c);
  }
  out += "'";
  return out;
}

std::string slurp_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Caminho relativo -> absoluto (a location local da tabela — usada nos modos
// Hadoop e REST; readers reais esperam file:// com path absoluto).
std::string abs_path(const std::string& dir) {
  if (!dir.empty() && (dir.front() == '/' || (dir.size() > 2 && dir[1] == ':'))) return dir;
  std::string cwd;
  if (!tilt::rt::tilt_getcwd(cwd)) die("nao foi possivel obter o diretorio atual");
  return cwd + "/" + dir;
}

// Nome da tabela no catalogo: basename da location.
std::string table_name(const std::string& location) {
  std::string loc = location;
  while (!loc.empty() && loc.back() == '/') loc.pop_back();
  const std::size_t slash = loc.find_last_of('/');
  const std::string nome = slash == std::string::npos ? loc : loc.substr(slash + 1);
  if (nome.empty()) die("nome de tabela invalido em '" + location + "'");
  return nome;
}

// Percent-encoding de um segmento de path (RFC 3986 unreserved, como no SigV4).
std::string uri_encode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

// Executa o curl: corpo da resposta vai para arquivo temporario (-o), status
// vem no stdout (-w). Com falhar=true, falha de transporte ou HTTP >= 400 ->
// die com o corpo da resposta truncado em ~200 chars. Retorna o status HTTP.
int rest_http(const std::string& method, const std::string& url, const std::string& body,
              bool falhar, std::string& corpo) {
  std::string body_file;
  std::string cmd = "curl -s ";
  if (falhar) cmd += "--fail-with-body ";
  cmd += "-w '%{http_code}' -X " + method;
  cmd += " -H " + shell_quote("Content-Type: application/json");
  if (!body.empty()) {
    std::string body_path;
    const int fd = tilt_tempfile("iceberg_body", body_path);
    if (fd < 0) die("nao foi possivel criar arquivo temporario");
    tilt_close_file(fd);
    {
      std::ofstream out(body_path, std::ios::trunc);
      out << body;
      if (!out) {
        std::remove(body_path.c_str());
        die("falha ao gravar o corpo da requisicao REST");
      }
    }
    body_file = body_path;
    cmd += " --data @" + body_file;
  }
  std::string out_file;
  const int ofd = tilt_tempfile("iceberg_resp", out_file);
  if (ofd < 0) {
    if (!body_file.empty()) std::remove(body_file.c_str());
    die("nao foi possivel criar arquivo temporario");
  }
  tilt_close_file(ofd);
  cmd += " -o " + shell_quote(out_file) + " " + shell_quote(url);

  std::string resp;
  {
    std::array<char, 4096> buf{};
    FILE* pipe = tilt_popen(cmd.c_str(), "r");
    if (!pipe) {
      if (!body_file.empty()) std::remove(body_file.c_str());
      std::remove(out_file.c_str());
      die("nao foi possivel executar 'curl'");
    }
    std::size_t n;
    while ((n = ::fread(buf.data(), 1, buf.size(), pipe)) > 0) resp.append(buf.data(), n);
    const int rc = tilt_pclose(pipe);
    if (!body_file.empty()) std::remove(body_file.c_str());
    if (rc != 0) {
      corpo = slurp_file(out_file);
      std::remove(out_file.c_str());
      die("requisicao ao catalogo REST falhou (curl codigo " + std::to_string(rc) +
          "): verifique ICEBERG_URI. Resposta: " + corpo.substr(0, 200));
    }
  }
  int status = 0;
  std::istringstream(resp) >> status;
  corpo = slurp_file(out_file);
  std::remove(out_file.c_str());
  if (falhar && status >= 400) {
    die("catalogo REST respondeu HTTP " + std::to_string(status) + ": " + corpo.substr(0, 200));
  }
  return status;
}

Value rest_parse_json(const std::string& texto, const char* ctx) {
  try {
    return json_parse(texto);
  } catch (const std::exception& e) {
    die(std::string(ctx) + ": resposta malformada do catalogo REST (JSON invalido): " + e.what());
  }
}

const char* kTablesPath = "/v1/namespaces/default/tables/";

// loadTable. Com falhar=false, 404 retorna o status para o caller tratar
// (tabela nova no escrever / erro claro no anexar/ler); 200 devolve o JSON da
// resposta em `resp`; demais >= 400 encerram com o corpo do erro.
int rest_load_table(const RestCfg& rc, const std::string& table, Value& resp, bool falhar) {
  std::string corpo;
  const int status = rest_http("GET", rc.uri + kTablesPath + uri_encode(table), "", falhar, corpo);
  if (status == 404 && !falhar) return 404;
  resp = rest_parse_json(corpo, "loadTable");
  if (const Value* md = map_find(resp, "metadata"); !md || md->kind != ValueKind::Mapa) {
    die("loadTable: resposta sem 'metadata'");
  }
  if (const Value* ml = map_find(resp, "metadata-location");
      !ml || ml->kind != ValueKind::Texto || ml->s.empty()) {
    die("loadTable: resposta sem 'metadata-location'");
  }
  return status;
}

// createTable: cria a tabela viva no catalogo (schema + partition-spec ja vao
// no corpo; o primeiro snapshot entra no commit subsequente).
void rest_create_table(const RestCfg& rc, const std::string& table, const std::string& location_uri,
                       const std::vector<Column>& cols,
                       const std::vector<PartitionField>& spec) {
  std::string body = "{\"name\":\"" + json_escape(table) + "\",\"location\":\"" +
                     json_escape(location_uri) + "\",\"schema\":" +
                     schema_struct_json(0, cols);
  if (!spec.empty()) body += ",\"partition-spec\":" + partition_spec_json(spec);
  body += ",\"properties\":{\"engine\":\"tilt\"}}";
  std::string corpo;
  rest_http("POST", rc.uri + "/v1/namespaces/default/tables", body, true, corpo);
  rest_parse_json(corpo, "createTable");
}

// commit: POST .../transactions com requirements + updates.
void rest_commit(const RestCfg& rc, const std::string& table, const std::string& requirements,
                 const std::string& updates) {
  const std::string body = "{\"requirements\":" + requirements + ",\"updates\":" + updates + "}";
  std::string corpo;
  rest_http("POST", rc.uri + kTablesPath + uri_encode(table) + "/transactions", body, true, corpo);
  rest_parse_json(corpo, "commit");
}

// metadata-location do loadTable -> caminho de arquivo legivel pelo
// parse_metadata: file:// le direto do disco; http(s) baixa via curl para um
// temporario (1a passada: o arquivo temporario vive ate o fim do processo).
std::string fetch_metadata_path(const std::string& metadata_location) {
  if (metadata_location.rfind("file://", 0) == 0) return metadata_location.substr(7);
  if (metadata_location.rfind("http://", 0) == 0 ||
      metadata_location.rfind("https://", 0) == 0) {
    std::string corpo;
    rest_http("GET", metadata_location, "", true, corpo);
    std::string meta_path;
    const int fd = tilt_tempfile("iceberg_meta", meta_path);
    if (fd < 0) die("nao foi possivel criar arquivo temporario");
    tilt_close_file(fd);
    {
      std::ofstream out(meta_path, std::ios::trunc);
      out << corpo;
      if (!out) {
        std::remove(meta_path.c_str());
        die("falha ao gravar o metadata baixado do catalogo REST");
      }
    }
    return meta_path;
  }
  die("metadata-location '" + metadata_location +
      "' nao suportada (fase 29: somente file:// e http(s)://)");
}

// Builders de requirements/updates de commit (Iceberg REST Open API).
std::string join_updates(const std::vector<std::string>& upds) {
  std::string out;
  for (std::size_t k = 0; k < upds.size(); ++k) {
    if (k) out += ',';
    out += upds[k];
  }
  return out;
}

std::string req_assert_current_snapshot(std::int64_t id) {
  return "{\"type\":\"assert-current-snapshot-id\",\"snapshot-id\":" + std::to_string(id) + "}";
}

std::string upd_upgrade_format() {
  return R"({"action":"upgrade-format-version","format-version":2})";
}

std::string upd_set_location(const std::string& loc) {
  return "{\"action\":\"set-location\",\"location\":\"" + json_escape(loc) + "\"}";
}

std::string upd_set_properties() {
  return R"({"action":"set-properties","properties":{"engine":"tilt"}})";
}

std::string upd_add_schema(std::int64_t schema_id, const std::vector<Column>& fields,
                           std::int64_t last_column_id) {
  return "{\"action\":\"add-schema\",\"schema\":" +
         schema_struct_json(schema_id, fields) + ",\"last-column-id\":" +
         std::to_string(last_column_id) + "}";
}

std::string upd_set_current_schema(std::int64_t id) {
  return "{\"action\":\"set-current-schema\",\"schema-id\":" + std::to_string(id) + "}";
}

std::string upd_add_snapshot(const Snapshot& s, std::int64_t schema_id) {
  std::string out = "{\"action\":\"add-snapshot\",\"snapshot\":{\"snapshot-id\":" +
                    std::to_string(s.id) + ",\"timestamp-ms\":" + std::to_string(s.ts) +
                    ",\"summary\":{\"operation\":\"" + s.operation + "\"}";
  if (schema_id >= 0) out += ",\"schema-id\":" + std::to_string(schema_id);
  out += ",\"manifest-list\":\"" + json_escape(s.manifest_list) + "\"}}";
  return out;
}

std::string upd_set_snapshot_ref(std::int64_t snapshot_id) {
  return "{\"action\":\"set-snapshot-ref\",\"ref-name\":\"main\",\"snapshot-id\":" +
         std::to_string(snapshot_id) + ",\"type\":\"branch\"}";
}

std::string upd_remove_snapshot_ref() {
  return R"({"action":"remove-snapshot-ref","ref-name":"main"})";
}

std::string upd_remove_snapshots(const std::vector<Snapshot>& snaps) {
  std::string ids;
  for (std::size_t k = 0; k < snaps.size(); ++k) {
    if (k) ids += ',';
    ids += std::to_string(snaps[k].id);
  }
  return "{\"action\":\"remove-snapshots\",\"snapshot-ids\":[" + ids + "]}";
}

void iceberg_write_rest(const RestCfg& rc, const std::string& dir, const Value& tabela,
                        const std::vector<std::string>& part_cols) {
  const std::string location = abs_path(dir);
  const std::string tabela_nome = table_name(location);
  const std::string location_uri = "file://" + location;

  Value resp;
  const int st = rest_load_table(rc, tabela_nome, resp, /*falhar=*/false);
  const bool nova = st == 404;

  // metadata anterior (sobrescrita): necessario para versionamento, schema-id
  // novo e requirement assert-current-snapshot-id.
  TableMeta meta_antiga;
  std::int64_t schema_id = 0;
  if (!nova) {
    parse_metadata(fetch_metadata_path(map_find(resp, "metadata-location")->s), meta_antiga);
    for (const SchemaVer& s : meta_antiga.schemas) schema_id = std::max(schema_id, s.id + 1);
  }

  // sobrescreve: limpa o metadata local anterior (o catalogo e a fonte da
  // verdade; data/manifest orfao fica para tras, como no modo Hadoop).
  for (const std::string& old : list_metadata_files(location + "/metadata")) {
    if (std::remove(old.c_str()) != 0) die("nao foi possivel limpar '" + old + "'");
  }

  const WriteCore wc = write_core(location, tabela, part_cols, schema_id);
  commit_metadata(location, nova ? 0 : meta_antiga.version + 1, wc.json);

  if (nova) {
    rest_create_table(rc, tabela_nome, location_uri, wc.cols, wc.spec_fields);
    const std::vector<std::string> upds = {
        upd_upgrade_format(), upd_set_location(location_uri), upd_set_properties(),
        upd_add_snapshot(wc.snap, schema_id), upd_set_snapshot_ref(wc.snap.id)};
    rest_commit(rc, tabela_nome, "[" + req_assert_current_snapshot(-1) + "]",
                "[" + join_updates(upds) + "]");
    return;
  }

  // Sobrescrita de tabela existente: o subconjunto mantem o partition spec
  // (divergencia exigiria add-partition-spec — fora do subconjunto da fase 29).
  std::vector<std::string> antigo, novo;
  for (const PartitionField& pf : meta_antiga.spec) antigo.push_back(pf.name);
  for (const PartitionField& pf : wc.spec_fields) novo.push_back(pf.name);
  if (antigo != novo) {
    die("escrever_iceberg via REST em tabela existente: particionamento divergente nao "
        "suportado (fase 29: o subconjunto mantem o partition spec da tabela; "
        "exclua a tabela no catalogo para recriar com outro particionar_por)");
  }
  std::vector<std::string> upds;
  if (meta_antiga.current_snapshot >= 0) {
    upds.push_back(upd_remove_snapshot_ref());
    upds.push_back(upd_remove_snapshots(meta_antiga.snapshots));
  }
  upds.push_back(upd_add_schema(schema_id, wc.cols,
                                std::max(meta_antiga.last_column_id,
                                         static_cast<std::int64_t>(wc.cols.size()))));
  upds.push_back(upd_set_current_schema(schema_id));
  upds.push_back(upd_add_snapshot(wc.snap, schema_id));
  upds.push_back(upd_set_snapshot_ref(wc.snap.id));
  rest_commit(rc, tabela_nome,
              "[" + req_assert_current_snapshot(meta_antiga.current_snapshot) + "]",
              "[" + join_updates(upds) + "]");
}

void iceberg_append_rest(const RestCfg& rc, const std::string& dir, const Value& tabela,
                         const std::vector<std::string>& part_cols_req) {
  const std::string location = abs_path(dir);
  const std::string tabela_nome = table_name(location);

  Value resp;
  const int st = rest_load_table(rc, tabela_nome, resp, /*falhar=*/false);
  if (st == 404) {
    die("tabela '" + tabela_nome + "' nao existe no catalogo REST '" + rc.uri +
        "' (use escrever_iceberg para criar)");
  }
  TableMeta meta;
  parse_metadata(fetch_metadata_path(map_find(resp, "metadata-location")->s), meta);

  const AppendCore ac = append_core(location, dir, meta, tabela, part_cols_req);
  commit_metadata(location, ac.version, ac.json);

  std::vector<std::string> upds;
  if (ac.evolved) {
    upds.push_back(upd_add_schema(ac.current_schema_id, ac.cols, ac.last_column_id));
    upds.push_back(upd_set_current_schema(ac.current_schema_id));
  }
  upds.push_back(upd_add_snapshot(ac.snap, ac.current_schema_id));
  upds.push_back(upd_set_snapshot_ref(ac.snap.id));
  rest_commit(rc, tabela_nome, "[" + req_assert_current_snapshot(meta.current_snapshot) + "]",
              "[" + join_updates(upds) + "]");
}

Value iceberg_read_rest(const RestCfg& rc, const std::string& dir, const Value* onde) {
  const std::string location = abs_path(dir);
  const std::string tabela_nome = table_name(location);

  Value resp;
  const int st = rest_load_table(rc, tabela_nome, resp, /*falhar=*/false);
  if (st == 404) {
    die("tabela '" + tabela_nome + "' nao existe no catalogo REST '" + rc.uri + "'");
  }
  TableMeta meta;
  parse_metadata(fetch_metadata_path(map_find(resp, "metadata-location")->s), meta);
  return read_core(meta, onde);
}

}  // namespace

void iceberg_write(const std::string& dir, const Value& tabela,
                   const std::vector<std::string>& part_cols) {
  const RestCfg rc = rest_cfg();
  if (rc.ativo) {
    iceberg_write_rest(rc, dir, tabela, part_cols);
    return;
  }
  // location absoluta (mesmo criterio do modo REST): data files, manifests e
  // metadata referenciados por file:// absoluto, legiveis de qualquer cwd —
  // e nao relativos ao diretorio corrente da escrita.
  const std::string location = abs_path(dir);
  const WriteCore wc = write_core(location, tabela, part_cols, /*schema_id=*/0);
  // sobrescreve: remove metadata anterior (data avro/parquet orfao fica para
  // tras, como no delta — a leitura so enxerga o que o metadata referencia).
  for (const std::string& old : list_metadata_files(location + "/metadata")) {
    if (std::remove(old.c_str()) != 0) die("nao foi possivel limpar '" + old + "'");
  }
  commit_metadata(location, 0, wc.json);
}

void iceberg_append(const std::string& dir, const Value& tabela,
                    const std::vector<std::string>& part_cols_req) {
  const RestCfg rc = rest_cfg();
  if (rc.ativo) {
    iceberg_append_rest(rc, dir, tabela, part_cols_req);
    return;
  }
  if (!tilt_is_directory(dir)) {
    die("tabela nao existe em '" + dir + "' (use escrever_iceberg para criar)");
  }
  const std::string meta_dir = dir + "/metadata";
  if (!tilt_is_directory(meta_dir)) {
    die("tabela nao existe em '" + dir + "' (use escrever_iceberg para criar)");
  }

  const std::string location = abs_path(dir);
  TableMeta meta;
  latest_metadata_path(location, meta);
  const AppendCore ac = append_core(location, dir, meta, tabela, part_cols_req);
  commit_metadata(location, ac.version, ac.json);
}

// Apaga linhas por predicado de igualdade (`onde`, mesma semantica do
// `ler_iceberg ... onde:`): escreve um delete file + novo snapshot
// (operation "delete"). `igualdade` = false -> position deletes
// ({file_path, pos}); true -> equality deletes (linhas completas projetadas).
// Nucleo compartilhado entre Hadoop local e REST (como AppendCore).
struct DeleteAlvo {
  std::string path;  // file:// do data file
  std::int64_t pos;
  Value row;   // projetada (modo igualdade)
  Value part;  // record `partition` do data file (para a entrada do manifest)
};

struct DeleteCore {
  Snapshot snap;
  std::int64_t version = 0;
  std::string json;
  std::int64_t apagadas = 0;
};

DeleteCore delete_core(const std::string& location, const std::string& dir_display,
                       const TableMeta& meta, const Value& onde, bool igualdade) {
  (void)dir_display;  // mensagens usam `location` (absoluto, como no delete)
  DeleteCore dc;
  const OndeFilter filtro = split_onde(&onde, meta.spec);
  const ResolvedFiles resolvidos = resolve_files(meta);
  // Deletes ja commitados: nao reapaga o ja apagado.
  const LoadedDeletes vigentes = load_deletes(resolvidos.deletes);

  std::vector<DeleteAlvo> alvos;
  for (const ActiveEntry& f : resolvidos.data) {
    if (!filtro.prune.empty() && !passa_pruning(f, filtro.prune, meta.spec)) continue;
    const std::string path = strip_scheme(f.path);
    Value chunk = parquet_read(path);
    if (chunk.kind != ValueKind::Lista && chunk.kind != ValueKind::Tabela) continue;
    std::int64_t pos = 0;
    for (Value& row : *chunk.list) {
      if (row.kind != ValueKind::Mapa || !row.map) {
        die("linha de '" + path + "' nao e um mapa");
      }
      Value m = project_row(f, row, meta);
      const bool ja_apagada = apagada_por_delete(path, pos, m, vigentes);
      if (!ja_apagada && residual_match(m, filtro.residual)) {
        alvos.push_back({f.path, pos, std::move(m), f.partition});
      }
      ++pos;
    }
  }
  if (alvos.empty()) return dc;  // sem commit

  // Um delete file por particao dos data files atingidos (com o record
  // `partition` correspondente na entrada do manifest): readers que casam
  // deletes com dados pela particao (ex.: pyiceberg) aplicam o delete em
  // cada particao. Tabela sem particao: um unico arquivo.
  auto part_chave = [](const Value& part) {
    std::string k;
    if (part.kind == ValueKind::Mapa && part.map) {
      for (const auto& kv : part.map->items) {
        k += "|" + kv.first + "=";
        if (kv.second.kind == ValueKind::Texto) k += "t:" + kv.second.s;
        else if (kv.second.kind == ValueKind::Inteiro) k += "i:" + std::to_string(kv.second.i);
        else if (kv.second.kind == ValueKind::Decimal) k += "d:" + std::to_string(kv.second.d);
        else if (kv.second.kind == ValueKind::Logico) k += kv.second.b ? "b:1" : "b:0";
        else k += "n";
      }
    }
    return k;
  };
  struct GrupoDel {
    Value part;
    std::vector<const DeleteAlvo*> itens;
  };
  std::vector<GrupoDel> grupos_del;
  for (const DeleteAlvo& a : alvos) {
    const std::string k = part_chave(a.part);
    auto it = std::find_if(grupos_del.begin(), grupos_del.end(), [&](const GrupoDel& g) {
      return part_chave(g.part) == k;
    });
    if (it == grupos_del.end()) {
      grupos_del.push_back({a.part, {&a}});
    } else {
      it->itens.push_back(&a);
    }
  }
  mkdir_if_missing(location + "/data");
  std::vector<std::pair<int, std::string>> del_changes;
  std::vector<FileInfo> del_infos;
  for (const GrupoDel& g : grupos_del) {
    Value del_table = Value::tabela();
    if (!igualdade) {
      for (const DeleteAlvo* a : g.itens) {
        Value r = Value::mapa();
        r.map->set("file_path", Value::texto(a->path));
        r.map->set("pos", Value::inteiro(a->pos));
        del_table.list->push_back(std::move(r));
      }
    } else {
      for (const DeleteAlvo* a : g.itens) del_table.list->push_back(a->row);
    }
    const std::string del_name = std::string("00000-1-") + new_uuid() +
                                 (igualdade ? ".eq-deletes.parquet" : ".pos-deletes.parquet");
    const std::string del_path = location + "/data/" + del_name;
    parquet_write(del_path, del_table);
    const std::int64_t del_size = file_size(del_path);
    if (del_size <= 0) die("falha ao gravar '" + del_path + "'");
    FileInfo dinfo;
    dinfo.path = "file://" + del_path;
    dinfo.records = static_cast<std::int64_t>(g.itens.size());
    dinfo.size = del_size;
    dinfo.content = igualdade ? 2 : 1;
    dinfo.part_map = g.part;
    del_changes.emplace_back(1, dinfo.path);
    del_infos.push_back(std::move(dinfo));
  }

  const std::string meta_dir = location + "/metadata";
  const std::int64_t snapshot_id = new_snapshot_id();
  const std::int64_t ts = now_ms();
  // Como no append: os data files ja ativos entram no novo manifest como
  // EXISTING — readers como pyiceberg so enxergam os manifests do snapshot
  // corrente e nao andam a cadeia de pais.
  std::vector<std::pair<int, std::string>> changes;
  std::vector<FileInfo> infos;
  std::int64_t existing_rows = 0;
  for (const ActiveEntry& e : resolvidos.data) {
    if (e.content != 0) continue;
    changes.emplace_back(0, e.path);
    FileInfo info;
    info.path = e.path;
    info.records = e.records;
    info.size = e.size;
    info.part_map = e.partition;
    infos.push_back(std::move(info));
    existing_rows += e.records;
  }
  std::int64_t del_rows = 0;
  for (std::size_t k = 0; k < del_changes.size(); ++k) {
    changes.push_back(del_changes[k]);
    infos.push_back(del_infos[k]);
    del_rows += del_infos[k].records;
  }
  const std::string manifest_name =
      write_manifest(meta_dir, changes, snapshot_id, infos, meta.spec);
  const std::string list_path = write_manifest_list(
      meta_dir, manifest_name, snapshot_id, static_cast<int>(del_changes.size()),
      static_cast<int>(resolvidos.data.size()), 0, del_rows, existing_rows, infos, meta.spec, 1);

  dc.snap.id = snapshot_id;
  dc.snap.ts = ts;
  dc.snap.operation = "delete";
  dc.snap.manifest_list = list_path;
  dc.snap.parent = meta.current_snapshot;
  dc.snap.has_parent = meta.current_snapshot >= 0;
  std::vector<Snapshot> snapshots = meta.snapshots;
  snapshots.push_back(dc.snap);
  std::vector<std::pair<std::int64_t, std::int64_t>> log = meta.snapshot_log;
  log.emplace_back(ts, snapshot_id);
  dc.version = meta.version < 0 ? 0 : meta.version + 1;
  dc.json = build_metadata_json(location, meta.uuid.empty() ? new_uuid() : meta.uuid,
                                meta.schemas, meta.current_schema_id, meta.last_column_id,
                                meta.spec, snapshots, log, snapshot_id, ts);
  dc.apagadas = static_cast<std::int64_t>(alvos.size());
  return dc;
}

void iceberg_delete_rest(const RestCfg& rc, const std::string& dir, const Value& onde,
                         bool igualdade, std::int64_t& apagadas) {
  const std::string location = abs_path(dir);
  const std::string tabela_nome = table_name(location);

  Value resp;
  const int st = rest_load_table(rc, tabela_nome, resp, /*falhar=*/false);
  if (st == 404) {
    die("tabela '" + tabela_nome + "' nao existe no catalogo REST '" + rc.uri +
        "' (use escrever_iceberg para criar)");
  }
  TableMeta meta;
  parse_metadata(fetch_metadata_path(map_find(resp, "metadata-location")->s), meta);

  const DeleteCore dc = delete_core(location, dir, meta, onde, igualdade);
  apagadas = dc.apagadas;
  if (dc.apagadas == 0) return;  // sem match: sem commit
  commit_metadata(location, dc.version, dc.json);

  std::vector<std::string> upds;
  upds.push_back(upd_add_snapshot(dc.snap, meta.current_schema_id));
  upds.push_back(upd_set_snapshot_ref(dc.snap.id));
  rest_commit(rc, tabela_nome, "[" + req_assert_current_snapshot(meta.current_snapshot) + "]",
              "[" + join_updates(upds) + "]");
}

std::int64_t iceberg_delete(const std::string& dir, const Value& onde, bool igualdade) {
  const RestCfg rc = rest_cfg();
  if (rc.ativo) {
    std::int64_t apagadas = 0;
    iceberg_delete_rest(rc, dir, onde, igualdade, apagadas);
    return apagadas;
  }
  const std::string location = abs_path(dir);
  TableMeta meta;
  latest_metadata_path(location, meta);
  const DeleteCore dc = delete_core(location, dir, meta, onde, igualdade);
  if (dc.apagadas == 0) return 0;
  commit_metadata(location, dc.version, dc.json);
  return dc.apagadas;
}

Value iceberg_read(const std::string& dir, const Value* onde) {
  const RestCfg rc = rest_cfg();
  if (rc.ativo) return iceberg_read_rest(rc, dir, onde);
  TableMeta meta;
  latest_metadata_path(dir, meta);
  return read_core(meta, onde);
}

}  // namespace tilt::rt
