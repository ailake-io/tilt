#include "runtime/parquet.hpp"

#include <dlfcn.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("parquet: " + m); }

// ---------------------------------------------------------------- thrift compact
// Subconjunto do protocolo Thrift Compact (parquet-format.org uses TCompact).
// Tipos: STOP=0 TRUE=1 FALSE=2 BYTE=3 I16=4 I32=5 I64=6 DOUBLE=7 BINARY=8
//        LIST=9 SET=10 MAP=11 STRUCT=12

enum TType : unsigned char {
  T_STOP = 0, T_TRUE = 1, T_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5,
  T_I64 = 6, TT_DOUBLE = 7, T_BINARY = 8, T_LIST = 9, T_SET = 10, T_MAP = 11,
  T_STRUCT = 12
};

struct Tw {  // writer
  std::string& out;
  std::vector<short> field_stack{0};

  void byte(unsigned char b) { out.push_back(static_cast<char>(b)); }
  void varint(std::uint64_t v) {
    while (v >= 0x80) {
      out.push_back(static_cast<char>((v & 0x7F) | 0x80));
      v >>= 7;
    }
    out.push_back(static_cast<char>(v));
  }
  void zz(std::int64_t v) { varint(static_cast<std::uint64_t>((v << 1) ^ (v >> 63))); }
  void raw(const void* p, std::size_t n) {
    out.append(static_cast<const char*>(p), n);
  }

  // Abre um campo; para bool usa field_bool(). Deve ser chamado com ids
  // ascendentes dentro de cada struct (delta encoding).
  void field(short id, TType t) {
    const short last = field_stack.back();
    const short delta = static_cast<short>(id - last);
    if (delta > 0 && delta <= 15) {
      byte(static_cast<unsigned char>((delta << 4) | t));
    } else {
      byte(static_cast<unsigned char>(t));
      zz(id);
    }
    field_stack.back() = id;
  }
  void field_bool(short id, bool v) { field(id, v ? T_TRUE : T_FALSE); }
  void field_i32(short id, std::int32_t v) {
    field(id, T_I32);
    zz(v);
  }
  void field_i64(short id, std::int64_t v) {
    field(id, T_I64);
    zz(v);
  }
  void field_double(short id, double v) {
    field(id, TT_DOUBLE);
    double le = v;
    raw(&le, 8);  // x86-64/arm64 sao little-endian
  }
  void field_str(short id, const std::string& s) {
    field(id, T_BINARY);
    varint(s.size());
    raw(s.data(), s.size());
  }
  void list_begin(short id, TType elem, std::size_t n) {
    field(id, T_LIST);
    if (n < 15) {
      byte(static_cast<unsigned char>((n << 4) | elem));
    } else {
      byte(static_cast<unsigned char>(0xF0 | elem));
      varint(n);
    }
  }
  void struct_begin() { field_stack.push_back(0); }
  void struct_end() {
    byte(T_STOP);
    field_stack.pop_back();
  }
};

struct Tr {  // reader
  const std::uint8_t* p;
  std::size_t n;
  std::size_t pos = 0;

  std::uint8_t byte() {
    if (pos >= n) die("fim inesperado ao ler metadados");
    return p[pos++];
  }
  std::uint64_t varint() {
    std::uint64_t v = 0;
    int shift = 0;
    while (true) {
      const std::uint8_t b = byte();
      v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
      shift += 7;
      if (shift > 63) die("varint invalido");
    }
  }
  std::int64_t zz() {
    const std::uint64_t v = varint();
    return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
  }
  void skip(TType t) {
    switch (t) {
      case T_TRUE:
      case T_FALSE:
      case T_STOP:
        return;
      case T_BYTE:
        pos += 1;
        return;
      case T_I16:
      case T_I32:
      case T_I64:
        varint();
        return;
      case TT_DOUBLE:
        pos += 8;
        return;
      case T_BINARY: {
        const std::uint64_t len = varint();
        if (len > n - pos) die("campo binario maior que o arquivo");
        pos += static_cast<std::size_t>(len);
        return;
      }
      case T_LIST:
      case T_SET: {
        const std::uint8_t h = byte();
        const std::size_t sz = (h >> 4) == 0xF ? static_cast<std::size_t>(varint()) : (h >> 4);
        const auto et = static_cast<TType>(h & 0xF);
        for (std::size_t k = 0; k < sz; ++k) skip(et);
        return;
      }
      case T_MAP: {
        const std::uint8_t h = byte();
        if (h == 0) return;
        const std::size_t sz = (h >> 4) == 0xF ? static_cast<std::size_t>(varint()) : (h >> 4);
        const auto et = static_cast<TType>(h & 0xF);
        for (std::size_t k = 0; k < sz; ++k) {
          skip(et);  // chaves e valores: mesmo subtipo neste subconjunto
          skip(et);
        }
        return;
      }
      case T_STRUCT: {
        short last = 0;
        while (true) {
          const std::uint8_t h = byte();
          const auto tt = static_cast<TType>(h & 0xF);
          if (tt == T_STOP) return;
          short id;
          if ((h >> 4) != 0) {
            id = static_cast<short>(last + (h >> 4));
          } else {
            id = static_cast<short>(zz());
          }
          last = id;
          skip(tt);
        }
      }
      default:
        die("tipo thrift compact desconhecido " + std::to_string(t));
    }
    if (pos > n) die("fim inesperado ao pular campo");
  }
};

// ---------------------------------------------------------------- dados

enum PType : int { PT_BOOLEAN = 0, PT_INT64 = 2, PT_DOUBLE = 5, PT_BYTE_ARRAY = 6 };
enum PEncoding : int {
  E_PLAIN = 0,
  E_PLAIN_DICTIONARY = 2,
  E_RLE = 3,
  E_RLE_DICTIONARY = 8,
};
enum PCodec : int { C_NONE = 0, C_SNAPPY = 1, C_GZIP = 2 };
enum PPageType : int { PG_DATA = 0, PG_DICTIONARY = 2, PG_DATA_V2 = 3 };

struct Column {
  std::string name;
  PType type = PT_BYTE_ARRAY;
  bool has_type = false;
  // vetores de valores guardam apenas os valores definidos (nao nulos);
  // `defined` se alinha por linha e marca onde ha nulo.
  std::vector<double> nums;
  std::vector<std::string> strings;
  std::vector<bool> bools;
  std::vector<bool> defined;
  bool optional = false;
  std::size_t rows = 0;
};

PType type_of(const Value& v) {
  switch (v.kind) {
    case ValueKind::Logico: return PT_BOOLEAN;
    case ValueKind::Inteiro: return PT_INT64;
    case ValueKind::Decimal: return PT_DOUBLE;
    case ValueKind::Texto: return PT_BYTE_ARRAY;
    default:
      die("tipo '" + std::string(v.type_name()) +
          "' nao suportado em parquet (use logico, inteiro, decimal, texto ou nulo)");
  }
}

const char* type_name(PType t) {
  switch (t) {
    case PT_BOOLEAN: return "BOOLEAN";
    case PT_INT64: return "INT64";
    case PT_DOUBLE: return "DOUBLE";
    case PT_BYTE_ARRAY: return "BYTE_ARRAY";
  }
  return "?";
}

std::string table_to_columns(const Value& tabela, std::vector<Column>& cols) {
  if (tabela.kind != ValueKind::Tabela && tabela.kind != ValueKind::Lista) {
    die("esperada uma tabela (lista de mapas)");
  }
  if (!tabela.list || tabela.list->empty()) die("tabela vazia; parquet exige ao menos 1 linha");
  const Value& first = (*tabela.list)[0];
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");

  for (const auto& [k, v] : first.map->items) {
    Column c;
    c.name = k;
    if (v.kind != ValueKind::Nulo) {
      c.type = type_of(v);
      c.has_type = true;
    }
    cols.push_back(std::move(c));
  }
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
    for (Column& c : cols) {
      const Value* cell = row.map->find(c.name);
      if (!cell) {
        die("coluna '" + c.name +
            "' ausente em uma das linhas (parquet exige as mesmas colunas em todas as linhas)");
      }
      if (cell->kind == ValueKind::Nulo) {
        c.defined.push_back(false);
        c.optional = true;
        continue;
      }
      if (!c.has_type) {
        c.type = type_of(*cell);
        c.has_type = true;
      } else if (type_of(*cell) != c.type) {
        die("coluna '" + c.name + "' mistura tipos (parquet e tipado por coluna)");
      }
      switch (c.type) {
        case PT_BOOLEAN: c.bools.push_back(cell->b); break;
        case PT_INT64:
        case PT_DOUBLE: c.nums.push_back(cell->as_number()); break;
        case PT_BYTE_ARRAY: c.strings.push_back(cell->s); break;
      }
      c.defined.push_back(true);
    }
    for (Column& c : cols) ++c.rows;
  }
  for (const Column& c : cols) {
    if (!c.has_type) {
      die("coluna '" + c.name +
          "' so tem valores nulos; forneca ao menos um valor nao nulo para inferir o tipo");
    }
  }
  return "";
}

// Codifica os valores definidos da coluna em encoding PLAIN.
std::string plain_encode(const Column& c) {
  std::string out;
  auto raw = [&](const void* p, std::size_t n) { out.append(static_cast<const char*>(p), n); };
  auto put32 = [&](std::uint32_t v) { raw(&v, 4); };

  switch (c.type) {
    case PT_BOOLEAN: {
      std::uint8_t acc = 0;
      std::uint8_t bit = 0;
      for (bool b : c.bools) {
        if (b) acc |= static_cast<std::uint8_t>(1u << bit);
        if (++bit == 8) {
          out.push_back(static_cast<char>(acc));
          acc = 0;
          bit = 0;
        }
      }
      if (bit) out.push_back(static_cast<char>(acc));
      break;
    }
    case PT_INT64: {
      for (double d : c.nums) {
        const std::int64_t v = static_cast<std::int64_t>(d);
        raw(&v, 8);  // little-endian
      }
      break;
    }
    case PT_DOUBLE: {
      for (double d : c.nums) {
        raw(&d, 8);
      }
      break;
    }
    case PT_BYTE_ARRAY: {
      for (const std::string& s : c.strings) {
        put32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
      }
      break;
    }
  }
  return out;
}

void put_uvarint(std::string& out, std::uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

// Definition levels (largura 1 bit: 0 = nulo, 1 = definido) em RLE runs,
// com o prefixo de 4 bytes little-endian exigido nas DATA_PAGE v1.
std::string encode_def_levels(const std::vector<bool>& defined) {
  std::string rle;
  std::size_t i = 0;
  while (i < defined.size()) {
    std::size_t j = i + 1;
    while (j < defined.size() && defined[j] == defined[i]) ++j;
    put_uvarint(rle, static_cast<std::uint64_t>(j - i) << 1);  // header de run RLE
    rle.push_back(defined[i] ? '\x01' : '\x00');
    i = j;
  }
  std::string out;
  const std::uint32_t len = static_cast<std::uint32_t>(rle.size());
  out.append(reinterpret_cast<const char*>(&len), 4);
  out += rle;
  return out;
}

// ---------------------------------------------------------------- leitura: RLE e zlib

unsigned bit_width(std::size_t n) {
  unsigned w = 0;
  while ((std::size_t{1} << w) < n) ++w;  // 1->0, 2->1, 3..4->2, ...
  return w;
}

struct RleIn {
  const std::uint8_t* p;
  std::size_t n;
  std::size_t pos = 0;

  std::uint64_t uvarint(const std::string& ctx) {
    std::uint64_t v = 0;
    unsigned shift = 0;
    while (true) {
      if (pos >= n) die(ctx + ": dados RLE truncados");
      const std::uint8_t b = p[pos++];
      v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
      shift += 7;
      if (shift > 63) die(ctx + ": varint RLE invalido");
    }
  }
};

std::uint64_t read_bits(const std::uint8_t* base, std::uint64_t bitpos, unsigned bw) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < bw; ++i) {
    const std::uint64_t bp = bitpos + i;
    v |= (static_cast<std::uint64_t>((base[bp / 8] >> (bp % 8)) & 1)) << i;
  }
  return v;
}

// Decodifica `count` valores no hibrido RLE/bit-pack do Parquet, largura bw.
void rle_decode(const std::uint8_t* p, std::size_t n, unsigned bw, std::size_t count,
                std::vector<std::uint32_t>& out, const std::string& ctx) {
  out.clear();
  out.reserve(count);
  RleIn in{p, n};
  while (out.size() < count) {
    const std::uint64_t header = in.uvarint(ctx);
    if ((header & 1) == 0) {  // run RLE
      const std::size_t run = static_cast<std::size_t>(header >> 1);
      const std::size_t nbytes = (bw + 7) / 8;
      if (in.pos + nbytes > n) die(ctx + ": run RLE truncado");
      std::uint32_t v = 0;
      for (std::size_t k = 0; k < nbytes; ++k) {
        v |= static_cast<std::uint32_t>(in.p[in.pos + k]) << (8 * k);
      }
      in.pos += nbytes;
      for (std::size_t k = 0; k < run && out.size() < count; ++k) out.push_back(v);
    } else {  // bit-packed: 8 valores por grupo, bw bits cada, LSB primeiro
      const std::size_t groups = static_cast<std::size_t>(header >> 1);
      const std::size_t nbytes = groups * bw;  // (8 * bw bits) / 8
      if (in.pos + nbytes > n) die(ctx + ": bloco bit-packed truncado");
      const std::uint8_t* base = in.p + in.pos;
      const std::uint64_t total_bits = static_cast<std::uint64_t>(groups) * 8 * bw;
      for (std::uint64_t bp = 0; bp < total_bits && out.size() < count; bp += bw) {
        out.push_back(static_cast<std::uint32_t>(read_bits(base, bp, bw)));
      }
      in.pos += nbytes;
    }
  }
}

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
};

template <typename F>
bool bind_zsym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(::dlsym(lib, name));
  return fn != nullptr;
}

const ZlibApi& zlib() {
  static const ZlibApi instance = [] {
    ZlibApi a;
    a.lib = ::dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!a.lib) a.lib = ::dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
    if (!a.lib) return a;
    const bool ok = bind_zsym(a.lib, a.version, "zlibVersion") &&
                    bind_zsym(a.lib, a.inflate_init2, "inflateInit2_") &&
                    bind_zsym(a.lib, a.inflate, "inflate") &&
                    bind_zsym(a.lib, a.inflate_end, "inflateEnd");
    if (!ok) {
      ::dlclose(a.lib);
      a = ZlibApi{};
    }
    return a;
  }();
  return instance;
}

constexpr int kZNoFlush = 0;
constexpr int kZStreamEnd = 1;
constexpr int kWindowBitsAuto = 15 + 32;  // aceita zlib (RFC1950) e gzip (RFC1952)

std::string gunzip_payload(const std::string& in, const std::string& col) {
  const ZlibApi& z = zlib();
  if (!z.lib) {
    die("coluna '" + col +
        "': gzip/deflate requer libz.so.1, que nao foi encontrada; instale o pacote zlib");
  }
  ZStream s{};
  s.next_in = reinterpret_cast<const std::uint8_t*>(in.data());
  s.avail_in = static_cast<unsigned int>(in.size());
  if (z.inflate_init2(&s, kWindowBitsAuto, z.version(), static_cast<int>(sizeof(ZStream))) != 0) {
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
    die("coluna '" + col + "': falha ao descomprimir pagina gzip/deflate (zlib retornou " +
        std::to_string(ret) + ")");
  }
  out.resize(s.total_out);
  return out;
}

// Decodifica `count` valores definidos em encoding PLAIN.
std::vector<Value> plain_values(PType t, const std::uint8_t* data, std::size_t avail,
                                std::size_t count, const std::string& col) {
  std::vector<Value> vals;
  vals.reserve(count);
  switch (t) {
    case PT_BOOLEAN: {
      if (avail * 8 < count) die("coluna '" + col + "': pagina de boolean truncada");
      for (std::size_t k = 0; k < count; ++k) {
        vals.push_back(Value::logico((data[k / 8] >> (k % 8)) & 1));
      }
      break;
    }
    case PT_INT64: {
      if (avail < count * 8) die("coluna '" + col + "': pagina de int64 truncada");
      for (std::size_t k = 0; k < count; ++k) {
        std::int64_t v;
        std::memcpy(&v, data + k * 8, 8);
        vals.push_back(Value::inteiro(v));
      }
      break;
    }
    case PT_DOUBLE: {
      if (avail < count * 8) die("coluna '" + col + "': pagina de double truncada");
      for (std::size_t k = 0; k < count; ++k) {
        double v;
        std::memcpy(&v, data + k * 8, 8);
        vals.push_back(Value::decimal(v));
      }
      break;
    }
    case PT_BYTE_ARRAY: {
      std::size_t off = 0;
      for (std::size_t k = 0; k < count; ++k) {
        if (off + 4 > avail) die("coluna '" + col + "': pagina de byte_array truncada");
        std::uint32_t len;
        std::memcpy(&len, data + off, 4);
        off += 4;
        if (off + len > avail) die("coluna '" + col + "': pagina de byte_array truncada");
        vals.push_back(Value::texto(
            std::string(reinterpret_cast<const char*>(data + off), len)));
        off += len;
      }
      break;
    }
    default:
      die(std::string("tipo fisico ") + type_name(t) + " da coluna '" + col +
          "' nao suportado nesta versao");
  }
  return vals;
}

struct ColMeta {
  PType type = PT_BYTE_ARRAY;
  int codec = 0;
  std::int64_t num_values = 0;
  std::int64_t data_page_offset = -1;
  std::int64_t dictionary_page_offset = -1;
  std::string name;
  int rep = 0;  // 0 = REQUIRED, 1 = OPTIONAL, 2 = REPEATED
};

// Le um chunk de coluna (todas as paginas entre dictionary/data_page_offset)
// e anexa os valores em `out`. Suporta: DATA_PAGE v1 PLAIN e DICTIONARY
// (PLAIN_DICTIONARY/RLE_DICTIONARY), definition levels RLE para campos
// OPTIONAL, multiplas paginas por chunk e codec gzip/deflate (zlib dlopen).
void decode_chunk(const std::string& file, const ColMeta& cm, std::vector<Value>& out) {
  if (cm.codec != C_NONE && cm.codec != C_GZIP) {
    if (cm.codec == C_SNAPPY) {
      die("coluna '" + cm.name +
          "': codec snappy nao suportado: compile com snappy ou use gzip/deflate");
    }
    die("coluna '" + cm.name + "': codec " + std::to_string(cm.codec) +
        " nao suportado (suportados: sem compressao, gzip/deflate)");
  }
  if (cm.rep == 2) {
    die("coluna '" + cm.name + "': campos REPEATED (listas aninhadas) ainda nao suportados");
  }
  const int max_def = cm.rep == 1 ? 1 : 0;

  std::size_t pos = static_cast<std::size_t>(cm.data_page_offset);
  if (cm.dictionary_page_offset >= 0 &&
      (cm.data_page_offset < 0 || cm.dictionary_page_offset < cm.data_page_offset)) {
    pos = static_cast<std::size_t>(cm.dictionary_page_offset);
  }
  if (cm.data_page_offset < 0 || pos >= file.size() - 8) {
    die("coluna '" + cm.name + "': data_page_offset invalido");
  }

  std::vector<Value> dict;
  bool has_dict = false;
  const std::int64_t target = static_cast<std::int64_t>(out.size()) + cm.num_values;
  while (static_cast<std::int64_t>(out.size()) < target) {
    Tr pr{reinterpret_cast<const std::uint8_t*>(file.data()), file.size(), pos};
    int page_type = -1;
    std::int64_t compressed = -1;
    std::int64_t page_values = -1;
    int encoding = -1, def_enc = -1, dict_enc = -1;
    {
      short last = 0;
      while (true) {  // PageHeader
        const std::uint8_t h = pr.byte();
        const auto tt = static_cast<TType>(h & 0xF);
        if (tt == T_STOP) break;
        const short id = (h >> 4) ? static_cast<short>(last + (h >> 4)) : static_cast<short>(pr.zz());
        last = id;
        if (id == 1 && tt == T_I32) {
          page_type = static_cast<int>(pr.zz());
        } else if (id == 3 && tt == T_I32) {
          compressed = pr.zz();
        } else if (id == 5 && tt == T_STRUCT) {
          short dlast = 0;
          while (true) {  // DataPageHeader
            const std::uint8_t dh = pr.byte();
            const auto dt = static_cast<TType>(dh & 0xF);
            if (dt == T_STOP) break;
            const short did =
                (dh >> 4) ? static_cast<short>(dlast + (dh >> 4)) : static_cast<short>(pr.zz());
            dlast = did;
            if (did == 1 && dt == T_I32) page_values = pr.zz();
            else if (did == 2 && dt == T_I32) encoding = static_cast<int>(pr.zz());
            else if (did == 3 && dt == T_I32) def_enc = static_cast<int>(pr.zz());
            else pr.skip(dt);
          }
        } else if (id == 7 && tt == T_STRUCT) {
          short dlast = 0;
          while (true) {  // DictionaryPageHeader
            const std::uint8_t dh = pr.byte();
            const auto dt = static_cast<TType>(dh & 0xF);
            if (dt == T_STOP) break;
            const short did =
                (dh >> 4) ? static_cast<short>(dlast + (dh >> 4)) : static_cast<short>(pr.zz());
            dlast = did;
            if (did == 1 && dt == T_I32) page_values = pr.zz();
            else if (did == 2 && dt == T_I32) dict_enc = static_cast<int>(pr.zz());
            else pr.skip(dt);
          }
        } else if (id == 8 && tt == T_STRUCT) {
          die("coluna '" + cm.name + "': DATA_PAGE_V2 ainda nao suportada nesta versao");
        } else {
          pr.skip(tt);
        }
      }
    }
    if (compressed < 0 || pr.pos + static_cast<std::size_t>(compressed) > file.size()) {
      die("coluna '" + cm.name + "': pagina truncada ou tamanho comprimido ausente");
    }
    std::string payload(file.data() + pr.pos, static_cast<std::size_t>(compressed));
    pos = pr.pos + static_cast<std::size_t>(compressed);
    if (cm.codec == C_GZIP) payload = gunzip_payload(payload, cm.name);

    if (page_type == PG_DICTIONARY) {
      if (dict_enc != E_PLAIN) {
        die("coluna '" + cm.name + "': dictionary page com encoding nao-PLAIN");
      }
      dict = plain_values(cm.type, reinterpret_cast<const std::uint8_t*>(payload.data()),
                          payload.size(), static_cast<std::size_t>(page_values), cm.name);
      has_dict = true;
      continue;
    }
    if (page_type != PG_DATA) {
      die("coluna '" + cm.name + "': tipo de pagina " + std::to_string(page_type) +
          " inesperado (apenas DATA_PAGE v1 e DICTIONARY_PAGE)");
    }
    if (page_values < 0) {
      die("coluna '" + cm.name + "': num_values da pagina ausente");
    }

    const std::uint8_t* d = reinterpret_cast<const std::uint8_t*>(payload.data());
    std::size_t avail = payload.size();
    const std::string ctx = "coluna '" + cm.name + "'";

    // definition levels (apenas campos OPTIONAL: max_def_level = 1)
    std::vector<std::uint32_t> defs;
    if (max_def > 0) {
      if (def_enc != E_RLE) {
        die(ctx + ": definition levels com encoding " + std::to_string(def_enc) +
            " (apenas RLE e suportado)");
      }
      if (avail < 4) die(ctx + ": definition levels truncados");
      std::uint32_t len;
      std::memcpy(&len, d, 4);
      d += 4;
      avail -= 4;
      if (len > avail) die(ctx + ": definition levels truncados");
      rle_decode(d, len, bit_width(static_cast<std::size_t>(max_def) + 1),
                 static_cast<std::size_t>(page_values), defs, ctx);
      d += len;
      avail -= len;
    } else {
      defs.assign(static_cast<std::size_t>(page_values), 0);
    }
    std::size_t defined_count = 0;
    for (std::uint32_t dv : defs) {
      if (static_cast<int>(dv) == max_def) ++defined_count;
    }

    std::vector<Value> vals;
    if (encoding == E_PLAIN) {
      vals = plain_values(cm.type, d, avail, defined_count, cm.name);
    } else if (encoding == E_PLAIN_DICTIONARY || encoding == E_RLE_DICTIONARY) {
      if (!has_dict) {
        die(ctx + ": pagina dictionary-encoded sem dictionary page");
      }
      if (avail < 1) die(ctx + ": indices de dictionary truncados");
      // indices RLE: 1 byte de bit width seguido do stream (sem length
      // prefix — o stream termina quando `defined_count` valores sao lidos)
      const unsigned bw = d[0];
      if (bw > 32) die(ctx + ": bit width de dictionary invalido (" + std::to_string(bw) + ")");
      d += 1;
      avail -= 1;
      std::vector<std::uint32_t> idx;
      rle_decode(d, avail, bw, defined_count, idx, ctx);
      vals.reserve(defined_count);
      for (std::uint32_t ix : idx) {
        if (ix >= dict.size()) {
          die(ctx + ": indice de dictionary " + std::to_string(ix) + " fora do intervalo (0.." +
              std::to_string(dict.size() - 1) + ")");
        }
        vals.push_back(dict[ix]);
      }
    } else {
      die(ctx + ": encoding " + std::to_string(encoding) +
          " nao suportado (apenas PLAIN e DICTIONARY)");
    }

    std::size_t vi = 0;
    for (std::int64_t k = 0; k < page_values; ++k) {
      if (static_cast<int>(defs[static_cast<std::size_t>(k)]) == max_def) {
        out.push_back(vals[vi++]);
      } else {
        out.push_back(Value::nulo());
      }
    }
  }
}

}  // namespace

void parquet_write(const std::string& path, const Value& tabela,
                   const std::vector<int>* field_ids) {
  std::vector<Column> cols;
  table_to_columns(tabela, cols);
  const std::size_t nrows = cols[0].rows;

  std::string body = "PAR1";
  struct ChunkInfo {
    PType type;
    std::string path_in_schema;
    std::int64_t data_page_offset = 0;
    std::int64_t total_size = 0;  // header + payload
    std::int64_t payload_size = 0;
  };
  std::vector<ChunkInfo> infos;

  for (const Column& c : cols) {
    // Colunas com nulos viram OPTIONAL (repetition_type=1, max_def_level=1):
    // a pagina leva definition levels RLE (0 = nulo, 1 = definido) seguidos
    // dos valores definidos em PLAIN. Colunas sem nulos seguem REQUIRED, sem
    // definition levels — byte a byte igual a antes.
    std::string payload;
    if (c.optional) payload = encode_def_levels(c.defined);
    payload += plain_encode(c);

    ChunkInfo ci;
    ci.type = c.type;
    ci.path_in_schema = c.name;
    ci.data_page_offset = static_cast<std::int64_t>(body.size());

    Tw hw{body};
    hw.struct_begin();  // PageHeader
    hw.field_i32(1, 0);  // PageType::DATA_PAGE
    hw.field_i32(2, static_cast<std::int32_t>(payload.size()));
    hw.field_i32(3, static_cast<std::int32_t>(payload.size()));
    hw.field(5, T_STRUCT);
    hw.struct_begin();
    hw.field_i32(1, static_cast<std::int32_t>(c.rows));
    hw.field_i32(2, E_PLAIN);
    hw.field_i32(3, E_RLE);
    hw.field_i32(4, E_RLE);
    hw.struct_end();
    hw.struct_end();  // STOP do PageHeader

    ci.total_size = static_cast<std::int64_t>(body.size()) - ci.data_page_offset +
                    static_cast<std::int64_t>(payload.size());
    infos.push_back(ci);
    body += payload;
  }

  const std::int64_t total_bytes = static_cast<std::int64_t>(body.size()) - 4;

  // FileMetaData
  std::string footer;
  Tw fw{footer};
  fw.field_i32(1, 1);  // version
  fw.list_begin(2, T_STRUCT, cols.size() + 1);
  {
    fw.struct_begin();
    fw.field_str(4, "schema");
    fw.field_i32(5, static_cast<std::int32_t>(cols.size()));
    fw.struct_end();
    for (std::size_t k = 0; k < cols.size(); ++k) {
      const Column& c = cols[k];
      fw.struct_begin();
      fw.field_i32(1, static_cast<std::int32_t>(c.type));
      fw.field_i32(3, c.optional ? 1 : 0);  // 0 = REQUIRED, 1 = OPTIONAL
      fw.field_str(4, c.name);
      fw.field_i32(9, field_ids ? (*field_ids)[k] : static_cast<std::int32_t>(k + 1));
      fw.struct_end();
    }
  }
  fw.field_i64(3, static_cast<std::int64_t>(nrows));
  fw.list_begin(4, T_STRUCT, 1);
  {
    fw.struct_begin();
    fw.list_begin(1, T_STRUCT, cols.size());
    for (std::size_t k = 0; k < cols.size(); ++k) {
      fw.struct_begin();
      fw.field_i64(2, infos[k].data_page_offset);  // ColumnChunk.file_offset
      fw.field(3, T_STRUCT);                        // ColumnChunk.meta_data
      fw.struct_begin();
      fw.field_i32(1, static_cast<std::int32_t>(infos[k].type));
      fw.list_begin(2, T_I32, 1);
      fw.zz(E_PLAIN);
      fw.list_begin(3, T_BINARY, 1);
      fw.varint(infos[k].path_in_schema.size());
      fw.raw(infos[k].path_in_schema.data(), infos[k].path_in_schema.size());
      fw.field_i32(4, C_NONE);
      fw.field_i64(5, static_cast<std::int64_t>(cols[k].rows));
      fw.field_i64(6, infos[k].total_size);
      fw.field_i64(7, infos[k].total_size);
      fw.field_i64(9, infos[k].data_page_offset);
      fw.struct_end();
      fw.struct_end();
    }
    fw.field_i64(2, total_bytes);               // total_byte_size
    fw.field_i64(3, static_cast<std::int64_t>(nrows));
    fw.struct_end();
  }
  fw.field_str(6, "tilt 0.1.0 (parquet: plain, colunas opcionais com nulos, sem compressao)");
  fw.struct_end();

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) die("nao foi possivel gravar '" + path + "'");
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  out.write(footer.data(), static_cast<std::streamsize>(footer.size()));
  const std::uint32_t flen = static_cast<std::uint32_t>(footer.size());
  out.write(reinterpret_cast<const char*>(&flen), 4);
  out.write("PAR1", 4);
}

Value parquet_read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) die("nao foi possivel abrir '" + path + "'");
  std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (file.size() < 12 || file.compare(0, 4, "PAR1") != 0 ||
      file.compare(file.size() - 4, 4, "PAR1") != 0) {
    die("'" + path + "' nao e um arquivo parquet (magic PAR1 ausente)");
  }
  std::uint32_t flen;
  std::memcpy(&flen, file.data() + file.size() - 8, 4);
  if (static_cast<std::size_t>(flen) > file.size() - 8) die("footer maior que o arquivo");

  Tr tr{reinterpret_cast<const std::uint8_t*>(file.data() + file.size() - 8 - flen), flen, 0};

  std::vector<std::vector<ColMeta>> row_groups;  // [row group][coluna]
  std::int64_t num_rows = -1;
  std::vector<std::string> schema_names;
  std::vector<int> schema_types;
  std::vector<int> schema_rep;

  // le FileMetaData (apenas os campos que interessam; o resto e pulado)
  {
    short last = 0;
    while (true) {
      const std::uint8_t h = tr.byte();
      const auto tt = static_cast<TType>(h & 0xF);
      if (tt == T_STOP) break;
      const short id = (h >> 4) ? static_cast<short>(last + (h >> 4)) : static_cast<short>(tr.zz());
      last = id;
      if (id == 2 && tt == T_LIST) {  // schema
        const std::uint8_t lh = tr.byte();
        const std::size_t sz = (lh >> 4) == 0xF ? static_cast<std::size_t>(tr.varint()) : (lh >> 4);
        for (std::size_t k = 0; k < sz; ++k) {
          short slast = 0;
          int ctype = -1;
          int rep = 0;
          std::string cname;
          bool is_root = false;
          while (true) {
            const std::uint8_t sh = tr.byte();
            const auto st = static_cast<TType>(sh & 0xF);
            if (st == T_STOP) break;
            const short sid =
                (sh >> 4) ? static_cast<short>(slast + (sh >> 4)) : static_cast<short>(tr.zz());
            slast = sid;
            if (sid == 1 && st == T_I32) ctype = static_cast<int>(tr.zz());
            else if (sid == 3 && st == T_I32) rep = static_cast<int>(tr.zz());
            else if (sid == 4 && st == T_BINARY) {
              const std::uint64_t len = tr.varint();
              cname.assign(reinterpret_cast<const char*>(tr.p + tr.pos),
                           static_cast<std::size_t>(len));
              tr.pos += static_cast<std::size_t>(len);
            } else if (sid == 5 && st == T_I32) {
              is_root = true;
              tr.skip(st);
            } else tr.skip(st);
          }
          if (!is_root && ctype >= 0) {
            schema_names.push_back(cname);
            schema_types.push_back(ctype);
            schema_rep.push_back(rep);
          }
        }
      } else if (id == 3 && tt == T_I64) {
        num_rows = tr.zz();
      } else if (id == 4 && tt == T_LIST) {  // row_groups
        const std::uint8_t lh = tr.byte();
        const std::size_t rgs = (lh >> 4) == 0xF ? static_cast<std::size_t>(tr.varint()) : (lh >> 4);
        for (std::size_t rg = 0; rg < rgs; ++rg) {
          row_groups.emplace_back();
          short rlast = 0;
          while (true) {  // RowGroup
            const std::uint8_t rh = tr.byte();
            const auto rt = static_cast<TType>(rh & 0xF);
            if (rt == T_STOP) break;
            const short rid =
                (rh >> 4) ? static_cast<short>(rlast + (rh >> 4)) : static_cast<short>(tr.zz());
            rlast = rid;
            if (rid == 1 && rt == T_LIST) {  // columns
              const std::uint8_t ch = tr.byte();
              const std::size_t csz =
                  (ch >> 4) == 0xF ? static_cast<std::size_t>(tr.varint()) : (ch >> 4);
              for (std::size_t k = 0; k < csz; ++k) {
                ColMeta cm;
                short clast = 0;
                while (true) {  // ColumnChunk
                  const std::uint8_t xh = tr.byte();
                  const auto xt = static_cast<TType>(xh & 0xF);
                  if (xt == T_STOP) break;
                  const short xid =
                      (xh >> 4) ? static_cast<short>(clast + (xh >> 4)) : static_cast<short>(tr.zz());
                  clast = xid;
                  if (xid == 3 && xt == T_STRUCT) {  // meta_data
                    short mlast = 0;
                    while (true) {  // ColumnMetaData
                      const std::uint8_t mh = tr.byte();
                      const auto mt = static_cast<TType>(mh & 0xF);
                      if (mt == T_STOP) break;
                      const short mid =
                          (mh >> 4) ? static_cast<short>(mlast + (mh >> 4)) : static_cast<short>(tr.zz());
                      mlast = mid;
                      if (mid == 1 && mt == T_I32) cm.type = static_cast<PType>(tr.zz());
                      else if (mid == 4 && mt == T_I32) cm.codec = static_cast<int>(tr.zz());
                      else if (mid == 5 && (mt == T_I32 || mt == T_I64)) cm.num_values = tr.zz();
                      else if (mid == 9 && mt == T_I64) cm.data_page_offset = tr.zz();
                      else if (mid == 11 && mt == T_I64) cm.dictionary_page_offset = tr.zz();
                      else tr.skip(mt);
                    }
                  } else {
                    tr.skip(xt);
                  }
                }
                row_groups.back().push_back(std::move(cm));
              }
            } else {
              tr.skip(rt);
            }
          }
        }
      } else {
        tr.skip(tt);
      }
    }
  }

  if (num_rows < 0 || row_groups.empty()) die("metadados ausentes ou incompletos em '" + path + "'");
  const std::size_t ncols = schema_types.size();
  if (ncols == 0) die("schema sem colunas em '" + path + "'");
  for (const auto& rg : row_groups) {
    if (rg.size() != ncols) {
      die("row group com " + std::to_string(rg.size()) + " colunas, mas o schema tem " +
          std::to_string(ncols));
    }
  }

  // decodifica cada coluna: percorre os row groups concatenando os chunks
  std::vector<std::vector<Value>> columns(ncols);
  for (std::size_t ci = 0; ci < ncols; ++ci) {
    auto& col = columns[ci];
    for (std::size_t rg = 0; rg < row_groups.size(); ++rg) {
      ColMeta cm = row_groups[rg][ci];
      cm.type = static_cast<PType>(schema_types[ci]);
      cm.name = schema_names[ci];
      cm.rep = schema_rep[ci];
      decode_chunk(file, cm, col);
    }
  }

  Value tabela = Value::tabela();
  for (std::int64_t r = 0; r < num_rows; ++r) {
    Value row = Value::mapa();
    for (std::size_t ci = 0; ci < ncols; ++ci) {
      const auto& col = columns[ci];
      if (static_cast<std::size_t>(r) >= col.size()) {
        die("coluna '" + schema_names[ci] + "' tem menos valores que 'num_rows'");
      }
      row.map->set(schema_names[ci], col[static_cast<std::size_t>(r)]);
    }
    tabela.list->push_back(std::move(row));
  }
  return tabela;
}

}  // namespace tilt::rt
