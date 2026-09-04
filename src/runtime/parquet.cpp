#include "runtime/parquet.hpp"

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
enum PEncoding : int { E_PLAIN = 0, E_RLE = 3 };
enum PCodec : int { C_NONE = 0 };

struct Column {
  std::string name;
  PType type;
  // valores achatados por linha (texto em `strings`, alinhado por indice)
  std::vector<double> nums;
  std::vector<std::string> strings;
  std::vector<bool> bools;
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
          "' nao suportado em parquet (use logico, inteiro, decimal, texto)");
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
    c.type = type_of(v);
    cols.push_back(std::move(c));
  }
  for (const Value& row : *tabela.list) {
    if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
    for (Column& c : cols) {
      const Value* cell = row.map->find(c.name);
      if (!cell) die("coluna '" + c.name + "' ausente em uma das linhas (parquet: 1a passada exige colunas obrigatorias)");
      if (type_of(*cell) != c.type) {
        die("coluna '" + c.name + "' mistura tipos (parquet e tipado por coluna)");
      }
      switch (c.type) {
        case PT_BOOLEAN: c.bools.push_back(cell->b); break;
        case PT_INT64:
        case PT_DOUBLE: c.nums.push_back(cell->as_number()); break;
        case PT_BYTE_ARRAY: c.strings.push_back(cell->s); break;
      }
    }
    for (Column& c : cols) ++c.rows;
  }
  return "";
}

// Codifica os valores da coluna em encoding PLAIN.
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

}  // namespace

void parquet_write(const std::string& path, const Value& tabela) {
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
    // REQUIRED (max_def_level = 0) nao grava definition levels na pagina;
    // o payload e so os valores em PLAIN.
    std::string payload = plain_encode(c);

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
    for (const Column& c : cols) {
      fw.struct_begin();
      fw.field_i32(1, static_cast<std::int32_t>(c.type));
      fw.field_i32(3, 0);  // REQUIRED
      fw.field_str(4, c.name);
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
  fw.field_str(6, "tilt 0.1.0 (parquet 1a passada: plain, sem compressao)");
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

  struct ColMeta {
    PType type = PT_BYTE_ARRAY;
    int codec = 0;
    std::int64_t num_values = 0;
    std::int64_t data_page_offset = -1;
    std::string name;
  };
  std::vector<ColMeta> metas;
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
                      else if (mid == 5 && mt == T_I64) cm.num_values = tr.zz();
                      else if (mid == 9 && mt == T_I64) cm.data_page_offset = tr.zz();
                      else tr.skip(mt);
                    }
                  } else {
                    tr.skip(xt);
                  }
                }
                metas.push_back(std::move(cm));
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

  if (num_rows < 0 || metas.empty()) die("metadados ausentes ou incompletos em '" + path + "'");
  for (std::size_t k = 0; k < metas.size(); ++k) {
    metas[k].type = static_cast<PType>(schema_types[k]);
    metas[k].name = schema_names[k];
  }

  // le cada coluna (data page unica, plain, sem compressao)
  std::vector<std::vector<Value>> columns(metas.size());
  for (std::size_t ci = 0; ci < metas.size(); ++ci) {
    const ColMeta& cm = metas[ci];
    if (cm.codec != C_NONE) {
      die("coluna '" + cm.name + "' usa compressao (codec " + std::to_string(cm.codec) +
          "); esta versao le apenas parquet sem compressao");
    }
    if (cm.data_page_offset < 0 ||
        static_cast<std::size_t>(cm.data_page_offset) >= file.size() - 8) {
      die("coluna '" + cm.name + "': data_page_offset invalido");
    }
    Tr pr{reinterpret_cast<const std::uint8_t*>(file.data()),
          file.size(),
          static_cast<std::size_t>(cm.data_page_offset)};
    std::int64_t page_size = -1;
    int encoding = -1;
    {
      short last = 0;
      while (true) {  // PageHeader
        const std::uint8_t h = pr.byte();
        const auto tt = static_cast<TType>(h & 0xF);
        if (tt == T_STOP) break;
        const short id = (h >> 4) ? static_cast<short>(last + (h >> 4)) : static_cast<short>(pr.zz());
        last = id;
        if (id == 1 && tt == T_I32) {
          const int ptype = static_cast<int>(pr.zz());
          if (ptype != 0) die("coluna '" + cm.name + "': apenas DATA_PAGE v1 e suportada");
        } else if ((id == 2 || id == 3) && tt == T_I32) {
          page_size = pr.zz();
        } else if (id == 5 && tt == T_STRUCT) {
          short dlast = 0;
          while (true) {  // DataPageHeader
            const std::uint8_t dh = pr.byte();
            const auto dt = static_cast<TType>(dh & 0xF);
            if (dt == T_STOP) break;
            const short did =
                (dh >> 4) ? static_cast<short>(dlast + (dh >> 4)) : static_cast<short>(pr.zz());
            dlast = did;
            if (did == 2 && dt == T_I32) encoding = static_cast<int>(pr.zz());
            else pr.skip(dt);
          }
        } else {
          pr.skip(tt);
        }
      }
    }
    if (page_size < 0) die("coluna '" + cm.name + "': tamanho de pagina ausente");
    if (encoding != E_PLAIN) {
      die("coluna '" + cm.name + "': encoding " + std::to_string(encoding) +
          " nao suportado (apenas PLAIN)");
    }
    const std::uint8_t* payload = pr.p + pr.pos;
    // Campos REQUIRED (repetition_type 0) nao tem definition levels na pagina;
    // OPTIONAL/REPEATED tem prefixo RLE de 4 bytes — ainda nao suportado.
    std::size_t off = 0;
    const int rep = ci < schema_rep.size() ? schema_rep[ci] : 0;
    if (rep != 0) {
      die("coluna '" + cm.name +
          "': campos opcionais/repetidos (definition levels) ainda nao suportados");
    }
    const std::uint8_t* data = payload + off;
    const std::size_t avail = static_cast<std::size_t>(page_size) - off;

    auto& col = columns[ci];
    col.reserve(static_cast<std::size_t>(cm.num_values));
    switch (cm.type) {
      case PT_BOOLEAN: {
        if (avail * 8 < static_cast<std::size_t>(cm.num_values)) die("pagina de boolean truncada");
        for (std::int64_t k = 0; k < cm.num_values; ++k) {
          col.push_back(Value::logico((data[k / 8] >> (k % 8)) & 1));
        }
        break;
      }
      case PT_INT64: {
        if (avail < static_cast<std::size_t>(cm.num_values) * 8) die("pagina de int64 truncada");
        for (std::int64_t k = 0; k < cm.num_values; ++k) {
          std::int64_t v;
          std::memcpy(&v, data + static_cast<std::size_t>(k) * 8, 8);
          col.push_back(Value::inteiro(v));
        }
        break;
      }
      case PT_DOUBLE: {
        if (avail < static_cast<std::size_t>(cm.num_values) * 8) die("pagina de double truncada");
        for (std::int64_t k = 0; k < cm.num_values; ++k) {
          double v;
          std::memcpy(&v, data + static_cast<std::size_t>(k) * 8, 8);
          col.push_back(Value::decimal(v));
        }
        break;
      }
      case PT_BYTE_ARRAY: {
        std::size_t off = 0;
        for (std::int64_t k = 0; k < cm.num_values; ++k) {
          if (off + 4 > avail) die("pagina de byte_array truncada");
          std::uint32_t len;
          std::memcpy(&len, data + off, 4);
          off += 4;
          if (off + len > avail) die("pagina de byte_array truncada");
          col.push_back(Value::texto(
              std::string(reinterpret_cast<const char*>(data + off), len)));
          off += len;
        }
        break;
      }
      default:
        die(std::string("tipo fisico ") + type_name(cm.type) + " da coluna '" + cm.name +
            "' nao suportado nesta versao");
    }
  }

  Value tabela = Value::tabela();
  for (std::int64_t r = 0; r < num_rows; ++r) {
    Value row = Value::mapa();
    for (std::size_t ci = 0; ci < metas.size(); ++ci) {
      const auto& col = columns[ci];
      if (static_cast<std::size_t>(r) >= col.size()) {
        die("coluna '" + metas[ci].name + "' tem menos valores que 'num_rows'");
      }
      row.map->set(metas[ci].name, col[static_cast<std::size_t>(r)]);
    }
    tabela.list->push_back(std::move(row));
  }
  return tabela;
}

}  // namespace tilt::rt
