#include "runtime/parquet.hpp"

#include "runtime/compat.hpp"
#include "runtime/snappy_codec.hpp"

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
  // Coluna de lista (REPEATED + anotacao LIST): `cells` guarda a celula
  // original (Lista ou Nulo) de cada linha; nums/strings/bools guardam os
  // elementos achatados de todas as linhas, na ordem.
  bool repeated = false;
  std::vector<Value> cells;
  // vetores de valores guardam apenas os valores definidos (nao nulos);
  // `defined` se alinha por linha e marca onde ha nulo (colunas flat).
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
          "' nao suportado em parquet (use logico, inteiro, decimal, texto, "
          "lista de escalares ou nulo)");
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

// Confere um elemento de lista e devolve o tipo fisico dele.
PType element_type_of(const std::string& col, const Value& v) {
  if (v.kind == ValueKind::Nulo) {
    die("coluna '" + col + "': elementos nulos dentro de listas ainda nao suportados");
  }
  if (v.kind == ValueKind::Lista || v.kind == ValueKind::Mapa) {
    die("coluna '" + col +
        "': listas aninhadas e structs dentro de listas ainda nao suportados");
  }
  return type_of(v);
}

void note_type(Column& c, PType t) {
  if (!c.has_type) {
    c.type = t;
    c.has_type = true;
  } else if (t != c.type) {
    die("coluna '" + c.name + "' mistura tipos (parquet e tipado por coluna)");
  }
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
    cols.push_back(std::move(c));
  }
  // fase 1: modo (escalar ou lista) e tipo de cada coluna, pela 1a celula
  // nao nula; as demais linhas sao validadas na fase 2.
  for (Column& c : cols) {
    for (const Value& row : *tabela.list) {
      const Value* cell = row.map->find(c.name);
      if (!cell) {
        die("coluna '" + c.name +
            "' ausente em uma das linhas (parquet exige as mesmas colunas em todas as linhas)");
      }
      if (cell->kind == ValueKind::Nulo) continue;
      if (cell->kind == ValueKind::Lista) {
        c.repeated = true;
        for (const Value& e : *cell->list) note_type(c, element_type_of(c.name, e));
      } else {
        note_type(c, type_of(*cell));
      }
      break;
    }
    if (!c.has_type) {
      die("coluna '" + c.name +
          "' so tem valores nulos/listas vazias; forneca ao menos um valor nao nulo para inferir "
          "o tipo");
    }
  }
  // fase 2: valida e achata todas as linhas
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
    for (Column& c : cols) {
      const Value* cell = row.map->find(c.name);
      if (!cell) {
        die("coluna '" + c.name +
            "' ausente em uma das linhas (parquet exige as mesmas colunas em todas as linhas)");
      }
      ++c.rows;
      if (c.repeated) {
        if (cell->kind == ValueKind::Nulo) {
          c.optional = true;
          c.cells.push_back(Value::nulo());
          continue;
        }
        if (cell->kind != ValueKind::Lista) {
          die("coluna '" + c.name + "' mistura listas e escalares em uma das linhas");
        }
        for (const Value& e : *cell->list) {
          const PType t = element_type_of(c.name, e);
          note_type(c, t);
          switch (c.type) {
            case PT_BOOLEAN: c.bools.push_back(e.b); break;
            case PT_INT64:
            case PT_DOUBLE: c.nums.push_back(e.as_number()); break;
            case PT_BYTE_ARRAY: c.strings.push_back(e.s); break;
          }
        }
        c.cells.push_back(*cell);
        continue;
      }
      if (cell->kind == ValueKind::Lista) {
        die("coluna '" + c.name + "' mistura listas e escalares em uma das linhas");
      }
      if (cell->kind == ValueKind::Nulo) {
        c.defined.push_back(false);
        c.optional = true;
        continue;
      }
      note_type(c, type_of(*cell));
      switch (c.type) {
        case PT_BOOLEAN: c.bools.push_back(cell->b); break;
        case PT_INT64:
        case PT_DOUBLE: c.nums.push_back(cell->as_number()); break;
        case PT_BYTE_ARRAY: c.strings.push_back(cell->s); break;
      }
      c.defined.push_back(true);
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

// Niveis (definition/repetition) como RLE puro em runs, sem prefixo de
// tamanho. Cada run: header uvarint (count << 1) seguido do valor em
// ceil(bw/8) bytes little-endian — valido no hibrido RLE/bit-pack do Parquet.
std::string rle_encode_levels(const std::vector<std::uint32_t>& levels, unsigned bw) {
  std::string rle;
  const std::size_t nbytes = (bw + 7) / 8;
  std::size_t i = 0;
  while (i < levels.size()) {
    std::size_t j = i + 1;
    while (j < levels.size() && levels[j] == levels[i]) ++j;
    put_uvarint(rle, static_cast<std::uint64_t>(j - i) << 1);  // header de run RLE
    for (std::size_t k = 0; k < nbytes; ++k) {
      rle.push_back(static_cast<char>((levels[i] >> (8 * k)) & 0xFF));
    }
    i = j;
  }
  return rle;
}

// Definition/repetition levels de uma coluna, ja no formato de escrita.
// `num_values` conta as entradas de level (= linhas em coluna flat) e
// `num_nulls` as entradas sem valor de folha (nulos + listas vazias) —
// a aritmetica num_values - num_nulls = valores de folha do DATA_PAGE_V2.
struct ColumnLevels {
  std::vector<std::uint32_t> defs;
  std::vector<std::uint32_t> reps;
  int max_def = 0;
  int max_rep = 0;
  std::int64_t num_values = 0;
  std::int64_t num_nulls = 0;
};

ColumnLevels column_levels(const Column& c) {
  ColumnLevels lv;
  if (!c.repeated) {
    lv.max_def = c.optional ? 1 : 0;
    lv.num_values = static_cast<std::int64_t>(c.rows);
    lv.defs.reserve(c.defined.size());
    for (bool d : c.defined) {
      lv.defs.push_back(d ? 1u : 0u);
      if (!d) ++lv.num_nulls;
    }
    return lv;
  }
  lv.max_rep = 1;
  lv.max_def = c.optional ? 2 : 1;
  for (const Value& cell : c.cells) {
    if (cell.kind == ValueKind::Nulo) {  // so com grupo externo OPTIONAL
      lv.defs.push_back(0);
      lv.reps.push_back(0);
      ++lv.num_nulls;
      ++lv.num_values;
      continue;
    }
    const ValueList& elems = *cell.list;
    if (elems.empty()) {  // lista vazia: grupo externo definido, sem elemento
      lv.defs.push_back(static_cast<std::uint32_t>(lv.max_def - 1));
      lv.reps.push_back(0);
      ++lv.num_nulls;
      ++lv.num_values;
      continue;
    }
    for (std::size_t k = 0; k < elems.size(); ++k) {
      lv.defs.push_back(static_cast<std::uint32_t>(lv.max_def));
      lv.reps.push_back(k == 0 ? 0u : 1u);
      ++lv.num_values;
    }
  }
  return lv;
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
  int (*deflate_init2)(ZStream*, int, int, int, int, int, const char*, int) = nullptr;
  int (*deflate)(ZStream*, int) = nullptr;
  int (*deflate_end)(ZStream*) = nullptr;
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
                    bind_zsym(a.lib, a.deflate_end, "deflateEnd");
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
constexpr int kWindowBitsAuto = 15 + 32;      // aceita zlib (RFC1950) e gzip (RFC1952)
constexpr int kWindowBitsGzip = 15 + 16;      // emite container gzip (RFC1952)

std::string zlib_ausente(const std::string& col) {
  return "coluna '" + col +
         "': gzip/deflate requer zlib (libz.so.1 no Linux, zlib1.dll no Windows), "
         "que nao foi encontrada; instale o pacote zlib";
}

std::string gunzip_payload(const std::string& in, const std::string& col) {
  const ZlibApi& z = zlib();
  if (!z.lib) {
    die(zlib_ausente(col));
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

// Comprime `in` num container gzip (RFC1952) com a zlib via dlopen.
std::string gzip_payload(const std::string& in, const std::string& col) {
  const ZlibApi& z = zlib();
  if (!z.lib) {
    die(zlib_ausente(col));
  }
  ZStream s{};
  if (z.deflate_init2(&s, kZDefaultCompression, kZDeflated, kWindowBitsGzip, 8, 0, z.version(),
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
    die("coluna '" + col + "': falha ao comprimir pagina gzip (zlib retornou " +
        std::to_string(ret) + ")");
  }
  out.resize(s.total_out);
  return out;
}

std::string decompress_payload(std::string payload, int codec, const std::string& col) {
  if (codec == C_GZIP) return gunzip_payload(payload, col);
  if (codec == C_SNAPPY) {
    try {
      return snappy_decompress(payload);
    } catch (const std::exception& e) {
      die("coluna '" + col + "': stream snappy invalido (" + e.what() + ")");
    }
  }
  return payload;
}

std::string compress_payload(const std::string& payload, int codec, const std::string& col) {
  if (codec == C_GZIP) return gzip_payload(payload, col);
  if (codec == C_SNAPPY) return snappy_compress_literals(payload);
  return payload;
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
  int codec = 0;
  std::int64_t num_values = 0;
  std::int64_t data_page_offset = -1;
  std::int64_t dictionary_page_offset = -1;
};

// Descritor de coluna extraido da arvore de schema: nome exposto (grupo
// externo no caso de lista), tipo da folha e niveis maximos de
// definicao/repeticao (0/0 = REQUIRED flat, 1/0 = OPTIONAL flat, 1/1 ou 2/1 =
// lista com outer REQUIRED ou OPTIONAL).
struct ColDesc {
  std::string name;
  PType type = PT_BYTE_ARRAY;
  int max_def = 0;
  int max_rep = 0;
  bool repeated = false;
  bool elem_nullable = false;  // lista com element OPTIONAL (max_def 3): def max-1 = elemento nulo
};

// Le um chunk de coluna (todas as paginas entre dictionary/data_page_offset)
// e anexa UM valor por linha em `out` (lista -> Value::lista, nulo ->
// Value::nulo). Suporta: DATA_PAGE v1 e v2, PLAIN e DICTIONARY
// (PLAIN_DICTIONARY/RLE_DICTIONARY), definition/repetition levels RLE para
// campos OPTIONAL e REPEATED (listas aninhadas de escalares, anotacao LIST
// de 3 ou 2 niveis), multiplas paginas por chunk e codecs gzip/deflate
// (zlib dlopen) e snappy (codec proprio).
void decode_chunk(const std::string& file, const ColMeta& cm, const ColDesc& cd,
                  std::int64_t expected_rows, std::vector<Value>& out) {
  const std::string ctx = "coluna '" + cd.name + "'";
  if (cm.codec != C_NONE && cm.codec != C_GZIP && cm.codec != C_SNAPPY) {
    die(ctx + ": codec " + std::to_string(cm.codec) +
        " nao suportado (suportados: sem compressao, gzip/deflate, snappy)");
  }
  const int max_def = cd.max_def;
  const int max_rep = cd.max_rep;

  std::size_t pos = static_cast<std::size_t>(cm.data_page_offset);
  if (cm.dictionary_page_offset >= 0 &&
      (cm.data_page_offset < 0 || cm.dictionary_page_offset < cm.data_page_offset)) {
    pos = static_cast<std::size_t>(cm.dictionary_page_offset);
  }
  if (cm.data_page_offset < 0 || pos >= file.size() - 8) {
    die(ctx + ": data_page_offset invalido");
  }

  std::vector<Value> dict;
  bool has_dict = false;
  const std::size_t start = out.size();
  while (static_cast<std::int64_t>(out.size() - start) < expected_rows) {
    Tr pr{reinterpret_cast<const std::uint8_t*>(file.data()), file.size(), pos};
    int page_type = -1;
    std::int64_t uncompressed = -1;
    std::int64_t compressed = -1;
    std::int64_t page_values = -1;
    int encoding = -1, def_enc = -1, dict_enc = -1;
    int v2_def_len = -1, v2_rep_len = -1;
    bool v2_compressed = true;
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
        } else if (id == 2 && tt == T_I32) {
          uncompressed = pr.zz();
        } else if (id == 3 && tt == T_I32) {
          compressed = pr.zz();
        } else if (id == 5 && tt == T_STRUCT) {
          short dlast = 0;
          while (true) {  // DataPageHeader (v1)
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
          page_type = PG_DATA_V2;
          short dlast = 0;
          while (true) {  // DataPageHeaderV2
            const std::uint8_t dh = pr.byte();
            const auto dt = static_cast<TType>(dh & 0xF);
            if (dt == T_STOP) break;
            const short did =
                (dh >> 4) ? static_cast<short>(dlast + (dh >> 4)) : static_cast<short>(pr.zz());
            dlast = did;
            if (did == 1 && dt == T_I32) page_values = pr.zz();
            else if (did == 4 && dt == T_I32) encoding = static_cast<int>(pr.zz());
            else if (did == 5 && dt == T_I32) v2_def_len = static_cast<int>(pr.zz());
            else if (did == 6 && dt == T_I32) v2_rep_len = static_cast<int>(pr.zz());
            else if (did == 7 && (dt == T_TRUE || dt == T_FALSE)) v2_compressed = dt == T_TRUE;
            else pr.skip(dt);
          }
        } else {
          pr.skip(tt);
        }
      }
    }
    (void)uncompressed;
    if (compressed < 0 || pr.pos + static_cast<std::size_t>(compressed) > file.size()) {
      die(ctx + ": pagina truncada ou tamanho comprimido ausente");
    }
    std::string payload(file.data() + pr.pos, static_cast<std::size_t>(compressed));
    pos = pr.pos + static_cast<std::size_t>(compressed);
    const bool v2 = page_type == PG_DATA_V2;
    // nas v1 o payload inteiro (levels + valores) e comprimido; dictionary
    // pages tambem seguem o codec da coluna. Em v2 so a secao de valores pode
    // estar comprimida — os levels sao tratados adiante.
    if (!v2) payload = decompress_payload(payload, cm.codec, cd.name);

    if (page_type == PG_DICTIONARY) {
      if (dict_enc != E_PLAIN) {
        die(ctx + ": dictionary page com encoding nao-PLAIN");
      }
      dict = plain_values(cd.type, reinterpret_cast<const std::uint8_t*>(payload.data()),
                          payload.size(), static_cast<std::size_t>(page_values), cd.name);
      has_dict = true;
      continue;
    }
    if (page_type != PG_DATA && page_type != PG_DATA_V2) {
      die(ctx + ": tipo de pagina " + std::to_string(page_type) +
          " inesperado (apenas DATA_PAGE v1/v2 e DICTIONARY_PAGE)");
    }
    if (page_values < 0) {
      die(ctx + ": num_values da pagina ausente");
    }

    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(payload.data());
    const std::size_t psize = payload.size();

    // repetition levels (apenas campos REPEATED: max_rep_level = 1) seguidas
    // dos definition levels; depois os valores. Em v1 as secoes levam prefixo
    // de 4 bytes e o restante da pagina e o payload comprimido; em v2 as
    // secoes tem comprimento explicito no header e so os valores podem estar
    // comprimidos.
    std::vector<std::uint32_t> reps, defs;
    const std::uint8_t* vdata = base;
    std::size_t vavail = psize;
    if (v2) {
      std::size_t off = 0;
      if (max_rep > 0) {
        if (v2_rep_len < 0) die(ctx + ": DATA_PAGE_V2 sem repetition_levels_byte_length");
        if (static_cast<std::size_t>(v2_rep_len) > psize - off) {
          die(ctx + ": repetition levels truncados");
        }
        rle_decode(base + off, static_cast<std::size_t>(v2_rep_len), bit_width(max_rep + 1),
                   static_cast<std::size_t>(page_values), reps, ctx);
        off += static_cast<std::size_t>(v2_rep_len);
      }
      if (max_def > 0) {
        if (v2_def_len < 0) die(ctx + ": DATA_PAGE_V2 sem definition_levels_byte_length");
        if (static_cast<std::size_t>(v2_def_len) > psize - off) {
          die(ctx + ": definition levels truncados");
        }
        rle_decode(base + off, static_cast<std::size_t>(v2_def_len), bit_width(max_def + 1),
                   static_cast<std::size_t>(page_values), defs, ctx);
        off += static_cast<std::size_t>(v2_def_len);
      }
      if (v2_compressed && cm.codec != C_NONE) {
        std::string values = decompress_payload(
            std::string(reinterpret_cast<const char*>(base + off), psize - off), cm.codec, cd.name);
        payload = std::move(values);
        base = reinterpret_cast<const std::uint8_t*>(payload.data());
        vdata = base;
        vavail = payload.size();
      } else {
        vdata = base + off;
        vavail = psize - off;
      }
    } else {
      std::size_t off = 0;
      auto read_levels = [&](int maxlevel, std::vector<std::uint32_t>& to) {
        if (psize - off < 4) die(ctx + ": levels truncados");
        std::uint32_t len;
        std::memcpy(&len, base + off, 4);
        off += 4;
        if (len > psize - off) die(ctx + ": levels truncados");
        rle_decode(base + off, len, bit_width(static_cast<std::size_t>(maxlevel) + 1),
                   static_cast<std::size_t>(page_values), to, ctx);
        off += len;
      };
      if (max_rep > 0) {
        read_levels(max_rep, reps);
      }
      if (max_def > 0) {
        if (def_enc != E_RLE && def_enc >= 0) {
          die(ctx + ": definition levels com encoding " + std::to_string(def_enc) +
              " (apenas RLE e suportado)");
        }
        read_levels(max_def, defs);
      }
      vdata = base + off;
      vavail = psize - off;
    }
    if (max_rep == 0) reps.assign(static_cast<std::size_t>(page_values), 0);
    if (max_def == 0) defs.assign(static_cast<std::size_t>(page_values), 0);

    std::size_t defined_count = 0;
    for (std::size_t k = 0; k < static_cast<std::size_t>(page_values); ++k) {
      if (defs[k] > static_cast<std::uint32_t>(max_def)) {
        die(ctx + ": definition level " + std::to_string(defs[k]) + " acima do maximo (" +
            std::to_string(max_def) + ")");
      }
      if (max_rep > 0 && reps[k] > static_cast<std::uint32_t>(max_rep)) {
        die(ctx + ": repetition level " + std::to_string(reps[k]) + " acima do maximo (" +
            std::to_string(max_rep) + ")");
      }
      if (static_cast<int>(defs[k]) == max_def) ++defined_count;
    }

    std::vector<Value> vals;
    if (encoding == E_PLAIN) {
      vals = plain_values(cd.type, vdata, vavail, defined_count, cd.name);
    } else if (encoding == E_PLAIN_DICTIONARY || encoding == E_RLE_DICTIONARY) {
      if (!has_dict) {
        die(ctx + ": pagina dictionary-encoded sem dictionary page");
      }
      if (vavail < 1) die(ctx + ": indices de dictionary truncados");
      // indices RLE: 1 byte de bit width seguido do stream (sem length
      // prefix — o stream termina quando `defined_count` valores sao lidos)
      const unsigned bw = vdata[0];
      if (bw > 32) die(ctx + ": bit width de dictionary invalido (" + std::to_string(bw) + ")");
      std::vector<std::uint32_t> idx;
      rle_decode(vdata + 1, vavail - 1, bw, defined_count, idx, ctx);
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

    if (!cd.repeated) {
      std::size_t vi = 0;
      for (std::int64_t k = 0; k < page_values; ++k) {
        if (static_cast<int>(defs[static_cast<std::size_t>(k)]) == max_def) {
          out.push_back(vals[vi++]);
        } else {
          out.push_back(Value::nulo());
        }
      }
      continue;
    }
    // REPEATED: cada linha comeca numa entrada com rep == 0; def == max_def
    // acrescenta um elemento a lista da linha; def intermediario sem rep
    // marca lista vazia (ou nula, quando o grupo externo e OPTIONAL e
    // def == 0). Elemento nulo (def == max_def-1 com element OPTIONAL, como
    // o pyarrow grava) nao tem representacao em tilt: erro claro.
    std::size_t vi = 0;
    bool have_row = false;
    bool cur_null = false;
    Value cur = Value::lista();
    for (std::int64_t k = 0; k < page_values; ++k) {
      const std::uint32_t def = defs[static_cast<std::size_t>(k)];
      if (reps[static_cast<std::size_t>(k)] == 0) {
        if (have_row) out.push_back(cur_null ? Value::nulo() : cur);
        have_row = true;
        cur_null = max_def >= 2 && def == 0;
        cur = Value::lista();
      }
      if (static_cast<int>(def) == max_def) {
        cur.list->push_back(vals[vi++]);
      } else if (cd.elem_nullable && static_cast<int>(def) == max_def - 1) {
        die(ctx + ": elementos nulos dentro de listas ainda nao suportados");
      }
    }
    if (have_row) out.push_back(cur_null ? Value::nulo() : cur);
  }
  if (static_cast<std::int64_t>(out.size() - start) != expected_rows) {
    die("coluna '" + cd.name + "': chunk com " + std::to_string(out.size() - start) +
        " linhas, esperado " + std::to_string(expected_rows));
  }
}

}  // namespace

// Anota logica UTF8 (LogicalType.STRING + ConvertedType.UTF8) em um
// SchemaElement de string. `w` ja deve ter escrito os campos de id < 10.
void write_logical_string(Tw& w) {
  w.field(10, T_STRUCT);  // logicalType: union LogicalType
  w.struct_begin();
  w.field(1, T_STRUCT);  // LogicalType.STRING (StringType vazio)
  w.struct_begin();
  w.struct_end();
  w.struct_end();
}

void parquet_write(const std::string& path, const Value& tabela,
                   const std::vector<int>* field_ids, const ParquetWriteOpts& opts) {
  std::vector<Column> cols;
  table_to_columns(tabela, cols);
  const std::size_t nrows = cols[0].rows;
  const int codec = opts.codec;
  if (codec != C_NONE && codec != C_GZIP && codec != C_SNAPPY) {
    die("codec " + std::to_string(codec) + " invalido (use 0=sem compressao, 1=snappy, 2=gzip)");
  }

  std::string body = "PAR1";
  struct ChunkInfo {
    PType type;
    bool repeated;
    std::vector<std::string> path;  // path_in_schema completo ate a folha
    std::int64_t data_page_offset = 0;
    std::int64_t compressed_size = 0;    // header + payload comprimido
    std::int64_t uncompressed_size = 0;  // header + payload sem compressao
    std::int64_t num_values = 0;         // entradas de level
    std::int64_t num_nulls = 0;
  };
  std::vector<ChunkInfo> infos;

  for (const Column& c : cols) {
    const ColumnLevels lv = column_levels(c);
    // Niveis e valores da pagina. Em v1 as secoes levam prefixo de 4 bytes e
    // o payload inteiro (levels + valores) e comprimido; em v2 so os valores
    // sao comprimidos — os levels ficam fora, sem prefixo, com o comprimento
    // de cada secao no header.
    const unsigned rep_bw = bit_width(static_cast<std::size_t>(lv.max_rep) + 1);
    const unsigned def_bw = bit_width(static_cast<std::size_t>(lv.max_def) + 1);
    const std::string rep_rle =
        lv.max_rep > 0 ? rle_encode_levels(lv.reps, rep_bw) : std::string();
    const std::string def_rle =
        lv.max_def > 0 ? rle_encode_levels(lv.defs, def_bw) : std::string();
    const std::string levels = rep_rle + def_rle;

    const std::string values_plain = plain_encode(c);
    std::string payload;
    std::int64_t payload_uncompressed;
    if (opts.paginas_v2) {
      const std::string values_comp = compress_payload(values_plain, codec, c.name);
      payload = levels + values_comp;
      payload_uncompressed = static_cast<std::int64_t>(levels.size() + values_plain.size());
    } else {
      std::string raw;
      if (lv.max_rep > 0) {
        const std::uint32_t rl = static_cast<std::uint32_t>(rep_rle.size());
        raw.append(reinterpret_cast<const char*>(&rl), 4);
        raw += rep_rle;
      }
      if (lv.max_def > 0) {
        const std::uint32_t dl = static_cast<std::uint32_t>(def_rle.size());
        raw.append(reinterpret_cast<const char*>(&dl), 4);
        raw += def_rle;
      }
      raw += values_plain;
      payload = compress_payload(raw, codec, c.name);
      payload_uncompressed = static_cast<std::int64_t>(raw.size());
    }

    ChunkInfo ci;
    ci.type = c.type;
    ci.repeated = c.repeated;
    ci.path = c.repeated ? std::vector<std::string>{c.name, "list", "element"}
                         : std::vector<std::string>{c.name};
    ci.data_page_offset = static_cast<std::int64_t>(body.size());
    ci.num_values = lv.num_values;
    ci.num_nulls = lv.num_nulls;

    Tw hw{body};
    hw.struct_begin();  // PageHeader
    if (!opts.paginas_v2) {
      hw.field_i32(1, PG_DATA);
      hw.field_i32(2, static_cast<std::int32_t>(payload_uncompressed));
      hw.field_i32(3, static_cast<std::int32_t>(payload.size()));
      hw.field(5, T_STRUCT);
      hw.struct_begin();
      hw.field_i32(1, static_cast<std::int32_t>(lv.num_values));
      hw.field_i32(2, E_PLAIN);
      hw.field_i32(3, E_RLE);  // def/rep encodings: RLE, como em parquet-cpp
      hw.field_i32(4, E_RLE);
      hw.struct_end();
    } else {
      // secoes de levels em v2 (sem prefixo): comprimento de cada uma
      const std::size_t rep_len = lv.max_rep > 0 ? rep_rle.size() : 0;
      const std::size_t def_len = lv.max_def > 0 ? def_rle.size() : 0;
      hw.field_i32(1, PG_DATA_V2);
      hw.field_i32(2, static_cast<std::int32_t>(payload_uncompressed));
      hw.field_i32(3, static_cast<std::int32_t>(payload.size()));
      hw.field(8, T_STRUCT);
      hw.struct_begin();  // DataPageHeaderV2
      hw.field_i32(1, static_cast<std::int32_t>(lv.num_values));
      hw.field_i32(2, static_cast<std::int32_t>(lv.num_nulls));
      hw.field_i32(3, static_cast<std::int32_t>(c.rows));
      hw.field_i32(4, E_PLAIN);
      hw.field_i32(5, static_cast<std::int32_t>(def_len));
      hw.field_i32(6, static_cast<std::int32_t>(rep_len));
      hw.field_bool(7, codec != C_NONE);  // is_compressed
      hw.struct_end();
    }
    hw.struct_end();  // STOP do PageHeader

    ci.uncompressed_size = static_cast<std::int64_t>(body.size()) - ci.data_page_offset +
                           payload_uncompressed;
    ci.compressed_size = static_cast<std::int64_t>(body.size()) - ci.data_page_offset +
                         static_cast<std::int64_t>(payload.size());
    infos.push_back(ci);
    body += payload;
  }

  const std::int64_t total_bytes = static_cast<std::int64_t>(body.size()) - 4;

  // FileMetaData
  std::string footer;
  Tw fw{footer};
  fw.field_i32(1, 1);  // version
  std::size_t schema_elems = 1;
  for (const Column& c : cols) schema_elems += c.repeated ? 3 : 1;
  fw.list_begin(2, T_STRUCT, schema_elems);
  {
    fw.struct_begin();
    fw.field_str(4, "schema");
    fw.field_i32(5, static_cast<std::int32_t>(cols.size()));
    fw.struct_end();
    for (std::size_t k = 0; k < cols.size(); ++k) {
      const Column& c = cols[k];
      const std::int32_t fid = field_ids ? (*field_ids)[k] : static_cast<std::int32_t>(k + 1);
      if (!c.repeated) {
        fw.struct_begin();
        fw.field_i32(1, static_cast<std::int32_t>(c.type));
        fw.field_i32(3, c.optional ? 1 : 0);  // 0 = REQUIRED, 1 = OPTIONAL
        fw.field_str(4, c.name);
        if (c.type == PT_BYTE_ARRAY) fw.field_i32(6, 0);  // ConvertedType.UTF8
        fw.field_i32(9, fid);
        if (c.type == PT_BYTE_ARRAY) write_logical_string(fw);
        fw.struct_end();
        continue;
      }
      // lista aninhada (3-level, padrao parquet-mr/pyarrow):
      //   optional|required group <nome> (LIST) {
      //     repeated group list { required <tipo> element; }
      //   }
      fw.struct_begin();
      fw.field_i32(3, c.optional ? 1 : 0);
      fw.field_str(4, c.name);
      fw.field_i32(5, 1);   // num_children
      fw.field_i32(6, 3);   // ConvertedType.LIST
      fw.field(10, T_STRUCT);
      fw.struct_begin();
      fw.field(3, T_STRUCT);  // LogicalType.LIST (ListType vazio)
      fw.struct_begin();
      fw.struct_end();
      fw.struct_end();
      fw.struct_end();
      fw.struct_begin();
      fw.field_i32(3, 2);  // REPEATED
      fw.field_str(4, "list");
      fw.field_i32(5, 1);
      fw.struct_end();
      fw.struct_begin();
      fw.field_i32(1, static_cast<std::int32_t>(c.type));
      fw.field_i32(3, 0);  // element REQUIRED (lista de escalares sem nulos)
      fw.field_str(4, "element");
      fw.field_i32(9, fid);
      if (c.type == PT_BYTE_ARRAY) {
        fw.field_i32(6, 0);  // ConvertedType.UTF8
        write_logical_string(fw);
      }
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
      fw.list_begin(2, T_I32, 2);
      fw.zz(E_PLAIN);
      fw.zz(E_RLE);
      fw.list_begin(3, T_BINARY, infos[k].path.size());
      for (const std::string& p : infos[k].path) {
        fw.varint(p.size());
        fw.raw(p.data(), p.size());
      }
      fw.field_i32(4, codec);
      fw.field_i64(5, infos[k].num_values);
      fw.field_i64(6, infos[k].uncompressed_size);
      fw.field_i64(7, infos[k].compressed_size);
      fw.field_i64(9, infos[k].data_page_offset);
      fw.struct_end();
      fw.struct_end();
    }
    fw.field_i64(2, total_bytes);               // total_byte_size
    fw.field_i64(3, static_cast<std::int64_t>(nrows));
    fw.struct_end();
  }
  const char* codec_nome = codec == C_GZIP ? "gzip" : codec == C_SNAPPY ? "snappy" : "sem compressao";
  fw.field_str(6, std::string("tilt 0.1.0 (parquet: plain, paginas ") +
                      (opts.paginas_v2 ? "v2" : "v1") + ", " + codec_nome +
                      ", opcionais com nulos, listas)");
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
  std::vector<std::int64_t> rg_num_rows;
  std::int64_t num_rows = -1;
  // arvore de schema (pre-ordem; elem 0 = raiz): tipo -1 = grupo
  struct SchemaElem {
    std::string name;
    int type = -1;
    int rep = 0;
    int converted = -1;
    bool logical_list = false;
    int num_children = 0;
    int parent = -1;
  };
  std::vector<SchemaElem> selem;

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
        std::vector<int> pend_idx;   // pilha de grupos abertos
        std::vector<int> pend_left;  // filhos restantes de cada grupo
        int parent = -1;
        for (std::size_t k = 0; k < sz; ++k) {
          short slast = 0;
          SchemaElem e;
          while (true) {
            const std::uint8_t sh = tr.byte();
            const auto st = static_cast<TType>(sh & 0xF);
            if (st == T_STOP) break;
            const short sid =
                (sh >> 4) ? static_cast<short>(slast + (sh >> 4)) : static_cast<short>(tr.zz());
            slast = sid;
            if (sid == 1 && st == T_I32) e.type = static_cast<int>(tr.zz());
            else if (sid == 3 && st == T_I32) e.rep = static_cast<int>(tr.zz());
            else if (sid == 4 && st == T_BINARY) {
              const std::uint64_t len = tr.varint();
              e.name.assign(reinterpret_cast<const char*>(tr.p + tr.pos),
                            static_cast<std::size_t>(len));
              tr.pos += static_cast<std::size_t>(len);
            } else if (sid == 5 && st == T_I32) {
              e.num_children = static_cast<int>(tr.zz());
            } else if (sid == 6 && st == T_I32) {
              e.converted = static_cast<int>(tr.zz());
            } else if (sid == 10 && st == T_STRUCT) {  // logicalType (union)
              short llast = 0;
              while (true) {
                const std::uint8_t lh2 = tr.byte();
                const auto lt = static_cast<TType>(lh2 & 0xF);
                if (lt == T_STOP) break;
                const short lid = (lh2 >> 4) ? static_cast<short>(llast + (lh2 >> 4))
                                             : static_cast<short>(tr.zz());
                llast = lid;
                if (lid == 3) e.logical_list = true;  // LogicalType.LIST
                tr.skip(lt);
              }
            } else tr.skip(st);
          }
          e.parent = parent;
          const int idx = static_cast<int>(selem.size());
          selem.push_back(std::move(e));
          if (!pend_left.empty() && --pend_left.back() == 0) {
            pend_idx.pop_back();
            pend_left.pop_back();
          }
          if (selem.back().num_children > 0) {
            pend_idx.push_back(idx);
            pend_left.push_back(selem.back().num_children);
          }
          parent = pend_idx.empty() ? -1 : pend_idx.back();
        }
      } else if (id == 3 && tt == T_I64) {
        num_rows = tr.zz();
      } else if (id == 4 && tt == T_LIST) {  // row_groups
        const std::uint8_t lh = tr.byte();
        const std::size_t rgs = (lh >> 4) == 0xF ? static_cast<std::size_t>(tr.varint()) : (lh >> 4);
        for (std::size_t rg = 0; rg < rgs; ++rg) {
          row_groups.emplace_back();
          rg_num_rows.push_back(-1);
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
                      if (mid == 4 && mt == T_I32) cm.codec = static_cast<int>(tr.zz());
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
            } else if (rid == 3 && rt == T_I64) {  // num_rows do RowGroup
              rg_num_rows.back() = tr.zz();
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

  // extrai as colunas da arvore de schema (filhos da raiz): escalares
  // REQUIRED/OPTIONAL, listas aninhadas (anotacao LIST de 3 ou 2 niveis,
  // incluindo o legado `repeated <tipo>` direto) e erro claro para o resto
  // (structs, listas de listas, elementos opcionais em lista).
  auto children_of = [&](int idx) {
    std::vector<int> out;
    for (int k = 0; k < static_cast<int>(selem.size()); ++k) {
      if (selem[static_cast<std::size_t>(k)].parent == idx) out.push_back(k);
    }
    return out;
  };
  auto check_leaf_type = [&](const std::string& col, int t) {
    if (t != PT_BOOLEAN && t != PT_INT64 && t != PT_DOUBLE && t != PT_BYTE_ARRAY) {
      die("coluna '" + col + "': tipo fisico " + std::to_string(t) + " nao suportado");
    }
  };
  std::vector<ColDesc> cols_desc;
  if (selem.empty()) die("schema vazio em '" + path + "'");
  for (int ci : children_of(0)) {
    const SchemaElem& e = selem[static_cast<std::size_t>(ci)];
    ColDesc d;
    d.name = e.name;
    if (e.type >= 0) {  // primitivo direto: coluna flat (ou lista 2-level legado)
      check_leaf_type(e.name, e.type);
      d.type = static_cast<PType>(e.type);
      if (e.rep == 2) {  // `repeated <tipo>` direto sob a raiz: lista legada
        d.repeated = true;
        d.max_rep = 1;
        d.max_def = 1;
      } else {
        if (e.rep > 1) die("coluna '" + e.name + "': repetition_type invalido");
        d.max_def = e.rep == 1 ? 1 : 0;
      }
      cols_desc.push_back(std::move(d));
      continue;
    }
    // grupo: so aceitamos anotacao LIST (senao seria struct)
    if (e.converted != 3 && !e.logical_list) {
      die("grupo '" + e.name +
          "' (struct) ainda nao suportado: apenas colunas escalares e listas de escalares");
    }
    const std::vector<int> gk = children_of(ci);
    if (gk.size() != 1) {
      die("coluna '" + e.name + "': grupo LIST com " + std::to_string(gk.size()) +
          " filhos (esperado 1: lista de escalares)");
    }
    const SchemaElem& g = selem[static_cast<std::size_t>(gk[0])];
    d.repeated = true;
    d.max_rep = 1;
    const int outer = e.rep == 1 ? 1 : 0;
    if (e.rep > 1) die("coluna '" + e.name + "': grupo LIST com repetition_type invalido");
    if (g.type >= 0) {  // 2-level: repeated <tipo> dentro do grupo LIST
      check_leaf_type(e.name, g.type);
      d.type = static_cast<PType>(g.type);
      d.max_def = outer + 1;
      cols_desc.push_back(std::move(d));
      continue;
    }
    // 3-level: repeated group <x> { <tipo> element }
    if (g.rep != 2) {
      die("coluna '" + e.name + "': grupo intermediario de LIST nao e REPEATED");
    }
    const std::vector<int> ek = children_of(gk[0]);
    if (ek.size() != 1) {
      die("coluna '" + e.name + "': listas de structs/elementos multiplos ainda nao suportadas");
    }
    const SchemaElem& el = selem[static_cast<std::size_t>(ek[0])];
    if (el.type < 0) {
      die("coluna '" + e.name + "': listas aninhadas (list<list<...>>) ainda nao suportadas");
    }
    if (el.rep > 1) {
      die("coluna '" + e.name +
          "': elementos REPEATED dentro de lista (listas aninhadas) ainda nao suportados");
    }
    check_leaf_type(e.name, el.type);
    d.type = static_cast<PType>(el.type);
    d.elem_nullable = el.rep == 1;  // pyarrow grava element OPTIONAL (max_def 3)
    d.max_def = outer + 1 + (d.elem_nullable ? 1 : 0);
    cols_desc.push_back(std::move(d));
  }

  const std::size_t ncols = cols_desc.size();
  if (ncols == 0) die("schema sem colunas em '" + path + "'");
  for (std::size_t rg = 0; rg < row_groups.size(); ++rg) {
    if (row_groups[rg].size() != ncols) {
      die("row group com " + std::to_string(row_groups[rg].size()) + " colunas, mas o schema tem " +
          std::to_string(ncols));
    }
    if (rg_num_rows[rg] < 0) rg_num_rows[rg] = num_rows;
  }

  // decodifica cada coluna: percorre os row groups concatenando os chunks
  std::vector<std::vector<Value>> columns(ncols);
  for (std::size_t ci = 0; ci < ncols; ++ci) {
    auto& col = columns[ci];
    for (std::size_t rg = 0; rg < row_groups.size(); ++rg) {
      decode_chunk(file, row_groups[rg][ci], cols_desc[ci], rg_num_rows[rg], col);
    }
  }

  Value tabela = Value::tabela();
  for (std::int64_t r = 0; r < num_rows; ++r) {
    Value row = Value::mapa();
    for (std::size_t ci = 0; ci < ncols; ++ci) {
      const auto& col = columns[ci];
      if (static_cast<std::size_t>(r) >= col.size()) {
        die("coluna '" + cols_desc[ci].name + "' tem menos valores que 'num_rows'");
      }
      row.map->set(cols_desc[ci].name, col[static_cast<std::size_t>(r)]);
    }
    tabela.list->push_back(std::move(row));
  }
  return tabela;
}

}  // namespace tilt::rt
