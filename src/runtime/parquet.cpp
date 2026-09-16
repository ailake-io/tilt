#include "runtime/parquet.hpp"

#include "runtime/compat.hpp"
#include "runtime/snappy_codec.hpp"

#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
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

enum PType : int {
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED = 7
};
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
  // Lista com elemento Nulo em alguma linha (B3): elemento OPTIONAL.
  bool elem_nullable = false;
  // Lista de listas (Marco 2 / B2a): elementos sao listas (ou Nulo);
  // escalares internos sao do tipo da coluna; `inner_nullable` = lista
  // interna Nula em alguma linha.
  bool nested_list = false;
  bool inner_nullable = false;
  // Lista de structs (Marco 2 / B2b): elementos sao mapas (ou Nulo) com
  // campos escalares; `children` = campos do elemento; `elem_struct_nullable`
  // = elemento Nulo em alguma linha.
  bool struct_list = false;
  bool elem_struct_nullable = false;
  // Coluna struct (Fase 12-5a): grupo sem anotacao; `children` sao os campos
  // (recursivo: escalar, lista de escalares ou struct aninhado).
  // `struct_defined` alinha por linha (falso = struct nulo na linha).
  bool is_struct = false;
  std::vector<Column> children;
  std::vector<bool> struct_defined;
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
    case PT_INT32: return "INT32";
    case PT_INT64: return "INT64";
    case PT_INT96: return "INT96";
    case PT_FLOAT: return "FLOAT";
    case PT_DOUBLE: return "DOUBLE";
    case PT_BYTE_ARRAY: return "BYTE_ARRAY";
    case PT_FIXED: return "FIXED_LEN_BYTE_ARRAY";
  }
  return "?";
}

// Confere um elemento de lista e devolve o tipo fisico dele. Nulo e
// permitido (B3: elemento OPTIONAL) e devolve false (sem tipo).
bool element_type_of(const std::string& col, const Value& v, PType& tipo) {
  if (v.kind == ValueKind::Nulo) return false;
  if (v.kind == ValueKind::Lista || v.kind == ValueKind::Mapa) {
    die("coluna '" + col +
        "': listas aninhadas e structs dentro de listas ainda nao suportados");
  }
  tipo = type_of(v);
  return true;
}

void note_type(Column& c, PType t) {
  if (!c.has_type) {
    c.type = t;
    c.has_type = true;
  } else if (t != c.type) {
    die("coluna '" + c.name + "' mistura tipos (parquet e tipado por coluna)");
  }
}

// Preenche uma coluna folha (escalar ou lista de escalares) a partir das
// celulas de cada linha. `cells[i]` e a celula da linha i (Nulo = ausente).
void fill_leaf(Column& c, const std::vector<const Value*>& cells) {
  for (const Value* cell : cells) {
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
        PType t = PT_BYTE_ARRAY;
        if (!element_type_of(c.name, e, t)) {
          c.elem_nullable = true;  // B3: elemento Nulo -> OPTIONAL
          continue;
        }
        note_type(c, t);
        switch (c.type) {
          case PT_BOOLEAN: c.bools.push_back(e.b); break;
          case PT_INT32:
          case PT_INT64:
          case PT_FLOAT:
          case PT_DOUBLE: c.nums.push_back(e.as_number()); break;
          case PT_BYTE_ARRAY: c.strings.push_back(e.s); break;
          case PT_INT96:
          case PT_FIXED:
            die("coluna '" + c.name + "': tipo de escrita nao suportado");
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
      case PT_INT32:
      case PT_INT64:
      case PT_FLOAT:
      case PT_DOUBLE: c.nums.push_back(cell->as_number()); break;
      case PT_BYTE_ARRAY: c.strings.push_back(cell->s); break;
      case PT_INT96:
      case PT_FIXED:
        die("coluna '" + c.name + "': tipo de escrita nao suportado");
    }
    c.defined.push_back(true);
  }
  if (!c.has_type) {
    die("coluna '" + c.name +
        "' so tem valores nulos/listas vazias; forneca ao menos um valor nao nulo para inferir "
        "o tipo");
  }
}

// Infere uma coluna (escalar, lista de escalares ou struct) a partir das
// celulas de cada linha. Recursivo para structs aninhados (Fase 12-5a).
Column infer_column(const std::string& name, const std::vector<const Value*>& cells);

void infer_struct(Column& c, const std::vector<const Value*>& cells) {
  c.is_struct = true;
  // Uniao das chaves em ordem de 1a aparicao; chave ausente numa linha =
  // Nulo (campo opcional) — mais permissivo que o top-level, que exige as
  // mesmas colunas em todas as linhas.
  std::vector<std::string> keys;
  for (const Value* cell : cells) {
    if (cell->kind != ValueKind::Mapa || !cell->map) continue;
    for (const auto& kv : cell->map->items) {
      if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
    }
  }
  if (keys.empty()) die("coluna '" + c.name + "': struct sem campos deduziveis");
  static const Value kNull = Value::nulo();
  for (const std::string& k : keys) {
    std::vector<const Value*> sub;
    sub.reserve(cells.size());
    for (const Value* cell : cells) {
      if (cell->kind == ValueKind::Mapa && cell->map) {
        if (const Value* f = cell->map->find(k)) {
          sub.push_back(f);
          continue;
        }
      }
      sub.push_back(&kNull);
    }
    c.children.push_back(infer_column(k, sub));
  }
  for (const Value* cell : cells) {
    ++c.rows;
    if (cell->kind == ValueKind::Nulo) {
      c.optional = true;
      c.struct_defined.push_back(false);
      continue;
    }
    c.struct_defined.push_back(true);
  }
}

Column infer_column(const std::string& name, const std::vector<const Value*>& cells) {
  Column c;
  c.name = name;
  // Modo pela 1a celula nao nula; demais linhas validadas no preenchimento.
  const Value* first = nullptr;
  for (const Value* cell : cells) {
    if (cell->kind != ValueKind::Nulo) {
      first = cell;
      break;
    }
  }
  if (!first) {
    die("coluna '" + name +
        "' so tem valores nulos/listas vazias; forneca ao menos um valor nao nulo para inferir "
        "o tipo");
  }
  if (first->kind == ValueKind::Mapa) {
    for (const Value* cell : cells) {
      if (cell->kind != ValueKind::Nulo &&
          (cell->kind != ValueKind::Mapa || !cell->map)) {
        die("coluna '" + name + "' mistura structs e escalares/listas em uma das linhas");
      }
    }
    infer_struct(c, cells);  // estrutura + preenchimento (recursivo)
    return c;
  }
  if (first->kind == ValueKind::Lista) {
    // B2a/B2b: elementos lista -> lista de listas; elementos mapa ->
    // lista de structs (campos escalares); resto escalar.
    bool aninhada = false;
    bool de_structs = false;
    for (const Value* cell : cells) {
      if (cell->kind != ValueKind::Lista || !cell->list) continue;
      for (const Value& e : *cell->list) {
        if (e.kind == ValueKind::Lista) aninhada = true;
        if (e.kind == ValueKind::Mapa) de_structs = true;
      }
    }
    if (aninhada && de_structs) {
      die("coluna '" + name + "': elementos lista e struct misturados");
    }
    if (de_structs) {
      c.repeated = true;
      c.struct_list = true;
      // Campos: uniao das chaves dos elementos definidos; escalares apenas.
      std::vector<std::string> keys;
      for (const Value* cell : cells) {
        if (cell->kind != ValueKind::Lista || !cell->list) continue;
        for (const Value& e : *cell->list) {
          if (e.kind != ValueKind::Mapa || !e.map) continue;
          for (const auto& kv : e.map->items) {
            if (kv.second.kind == ValueKind::Lista || kv.second.kind == ValueKind::Mapa) {
              die("coluna '" + name + "': campos compostos em elementos de lista ainda nao "
                  "suportados");
            }
            if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) {
              keys.push_back(kv.first);
            }
          }
        }
      }
      if (keys.empty()) {
        die("coluna '" + name + "': elementos struct sem campos deduziveis");
      }
      static const Value kNull = Value::nulo();
      for (const std::string& k : keys) {
        Column ch;
        ch.name = k;
        // Tipo pela 1a celula nao nula do campo (em elementos definidos).
        for (const Value* cell : cells) {
          if (cell->kind != ValueKind::Lista || !cell->list) continue;
          for (const Value& e : *cell->list) {
            if (e.kind != ValueKind::Mapa || !e.map) continue;
            if (const Value* f = e.map->find(k)) {
              if (f->kind != ValueKind::Nulo) {
                note_type(ch, type_of(*f));
                goto proximo_campo;
              }
            }
          }
        }
      proximo_campo:;
        if (!ch.has_type) {
          die("coluna '" + name + "." + k + "': so valores nulos (tipo nao deduzivel)");
        }
        // Preenche (elemento Nulo ou chave ausente -> Nulo no campo).
        for (const Value* cell : cells) {
          if (cell->kind != ValueKind::Lista || !cell->list) continue;
          for (const Value& e : *cell->list) {
            if (e.kind != ValueKind::Mapa || !e.map) continue;
            if (const Value* f = e.map->find(k)) {
              if (f->kind == ValueKind::Nulo) {
                ch.optional = true;
                continue;  // payload so dos definidos (abaixo)
              }
              note_type(ch, type_of(*f));
            } else {
              ch.optional = true;
              continue;
            }
          }
        }
        // Payload dos definidos, na ordem dos elementos.
        for (const Value* cell : cells) {
          if (cell->kind != ValueKind::Lista || !cell->list) continue;
          for (const Value& e : *cell->list) {
            if (e.kind != ValueKind::Mapa || !e.map) continue;
            const Value* f = e.map->find(k);
            if (!f || f->kind == ValueKind::Nulo) continue;
            switch (ch.type) {
              case PT_BOOLEAN: ch.bools.push_back(f->b); break;
              case PT_INT32:
              case PT_INT64:
              case PT_FLOAT:
              case PT_DOUBLE: ch.nums.push_back(f->as_number()); break;
              case PT_BYTE_ARRAY: ch.strings.push_back(f->s); break;
              default:
                die("coluna '" + name + "': tipo de escrita nao suportado");
            }
          }
        }
        ch.rows = cells.size();  // linhas da tabela (para o cabecalho v2)
        c.children.push_back(std::move(ch));
      }
      for (const Value* cell : cells) {
        ++c.rows;
        if (cell->kind == ValueKind::Nulo) {
          c.optional = true;
          c.cells.push_back(Value::nulo());
        } else if (cell->kind != ValueKind::Lista) {
          die("coluna '" + name + "' mistura listas e escalares em uma das linhas");
        } else {
          for (const Value& e : *cell->list) {
            if (e.kind == ValueKind::Nulo) c.elem_struct_nullable = true;
          }
          c.cells.push_back(*cell);
        }
      }
      return c;
    }
    c.repeated = true;
    if (aninhada) {
      c.nested_list = true;
      for (const Value* cell : cells) {
        if (cell->kind != ValueKind::Nulo && cell->kind != ValueKind::Lista) {
          die("coluna '" + name + "' mistura listas e escalares em uma das linhas");
        }
        if (cell->kind == ValueKind::Lista) {
          for (const Value& e : *cell->list) {
            if (e.kind != ValueKind::Nulo && e.kind != ValueKind::Lista) {
              die("coluna '" + name + "' mistura listas e escalares nos elementos");
            }
            if (e.kind == ValueKind::Lista) {
              for (const Value& s : *e.list) {
                if (s.kind == ValueKind::Nulo) {
                  c.elem_nullable = true;
                } else {
                  if (s.kind == ValueKind::Lista || s.kind == ValueKind::Mapa) {
                    die("coluna '" + name + "': 3 niveis de lista ainda nao suportados");
                  }
                  note_type(c, type_of(s));
                }
              }
            } else {
              c.inner_nullable = true;  // elemento interno Nulo
            }
          }
        }
      }
      // Preenche celulas (payloads vao no flatten); conta linhas/nulos.
      for (const Value* cell : cells) {
        ++c.rows;
        if (cell->kind == ValueKind::Nulo) {
          c.optional = true;
          c.cells.push_back(Value::nulo());
        } else {
          c.cells.push_back(*cell);
        }
      }
      if (!c.has_type) {
        die("coluna '" + name +
            "' so tem valores nulos/listas vazias; forneca ao menos um valor nao nulo para "
            "inferir o tipo");
      }
      return c;
    }
    for (const Value* cell : cells) {
      if (cell->kind != ValueKind::Nulo && cell->kind != ValueKind::Lista) {
        die("coluna '" + name + "' mistura listas e escalares em uma das linhas");
      }
      if (cell->kind == ValueKind::Lista) {
        for (const Value& e : *cell->list) {
          PType t = PT_BYTE_ARRAY;
          if (element_type_of(name, e, t)) {
            note_type(c, t);
          } else {
            c.elem_nullable = true;  // B3
          }
        }
      }
    }
  } else {
    for (const Value* cell : cells) {
      if (cell->kind == ValueKind::Lista || cell->kind == ValueKind::Mapa) {
        die("coluna '" + name + "' mistura listas/structs e escalares em uma das linhas");
      }
      if (cell->kind != ValueKind::Nulo) note_type(c, type_of(*cell));
    }
  }
  fill_leaf(c, cells);
  return c;
}

std::string table_to_columns(const Value& tabela, std::vector<Column>& cols) {
  if (tabela.kind != ValueKind::Tabela && tabela.kind != ValueKind::Lista) {
    die("esperada uma tabela (lista de mapas)");
  }
  if (!tabela.list || tabela.list->empty()) die("tabela vazia; parquet exige ao menos 1 linha");
  const Value& first = (*tabela.list)[0];
  if (first.kind != ValueKind::Mapa || !first.map) die("linhas devem ser mapas { campo: valor }");

  for (const auto& [k, v] : first.map->items) {
    (void)v;
    std::vector<const Value*> cells;
    cells.reserve(tabela.list->size());
    for (const Value& row : *tabela.list) {
      if (row.kind != ValueKind::Mapa || !row.map) die("linhas devem ser mapas { campo: valor }");
      const Value* cell = row.map->find(k);
      if (!cell) {
        die("coluna '" + k +
            "' ausente em uma das linhas (parquet exige as mesmas colunas em todas as linhas)");
      }
      cells.push_back(cell);
    }
    cols.push_back(infer_column(k, cells));
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
    case PT_INT32: {
      for (double d : c.nums) {
        const std::int64_t v = static_cast<std::int64_t>(d);
        if (v < -2147483648LL || v > 2147483647LL) {
          die("coluna '" + c.name + "': valor " + std::to_string(v) +
              " fora do INT32 (use tipo int64)");
        }
        const std::int32_t w = static_cast<std::int32_t>(v);
        raw(&w, 4);
      }
      break;
    }
    case PT_FLOAT: {
      for (double d : c.nums) {
        const float v = static_cast<float>(d);
        raw(&v, 4);
      }
      break;
    }
    case PT_DOUBLE: {
      for (double d : c.nums) {
        raw(&d, 8);
      }
      break;
    }
    case PT_INT96:
    case PT_FIXED:
      die("coluna '" + c.name + "': escrita " + type_name(c.type) +
          " nao suportada (leitura apenas)");
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

// Folha achatada da arvore de colunas: caminho completo, ponteiro para a
// folha (dona dos vetores de valores) e levels com a contribuicao dos
// structs ancestrais (Fase 12-5a).
struct FlatLeaf {
  std::vector<std::string> path;
  const Column* leaf = nullptr;
  ColumnLevels lv;
};

void flatten_scalar(const Column& c, std::vector<std::string> path, int def_base,
                    const std::vector<int>& null_lv, std::vector<FlatLeaf>& out) {
  FlatLeaf fl;
  fl.path = std::move(path);
  fl.leaf = &c;
  fl.lv.max_def = def_base + (c.optional ? 1 : 0);
  fl.lv.num_values = static_cast<std::int64_t>(c.rows);
  for (std::size_t r = 0; r < c.rows; ++r) {
    const int anc = r < null_lv.size() ? null_lv[r] : -1;
    if (anc >= 0) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(anc));
      ++fl.lv.num_nulls;
    } else if (r < c.defined.size() && c.defined[r]) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(fl.lv.max_def));
    } else {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(fl.lv.max_def - 1));
      ++fl.lv.num_nulls;
    }
  }
  out.push_back(std::move(fl));
}

void flatten_list(const Column& c, std::vector<std::string> path, int def_base,
                  const std::vector<int>& null_lv, std::vector<FlatLeaf>& out) {
  FlatLeaf fl;
  fl.path = std::move(path);
  fl.leaf = &c;
  fl.lv.max_rep = 1;
  const int outer = c.optional ? 1 : 0;
  // B3: elemento Nulo -> OPTIONAL (um nivel a mais; def max-1 = nulo).
  fl.lv.max_def = def_base + outer + 1 + (c.elem_nullable ? 1 : 0);
  for (std::size_t r = 0; r < c.cells.size(); ++r) {
    const int anc = r < null_lv.size() ? null_lv[r] : -1;
    if (anc >= 0) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(anc));
      fl.lv.reps.push_back(0);
      ++fl.lv.num_nulls;
      ++fl.lv.num_values;
      continue;
    }
    const Value& cell = c.cells[r];
    if (cell.kind == ValueKind::Nulo) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(def_base));
      fl.lv.reps.push_back(0);
      ++fl.lv.num_nulls;
      ++fl.lv.num_values;
      continue;
    }
    const ValueList& elems = *cell.list;
    if (elems.empty()) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(def_base + outer));
      fl.lv.reps.push_back(0);
      ++fl.lv.num_nulls;
      ++fl.lv.num_values;
      continue;
    }
    for (std::size_t k = 0; k < elems.size(); ++k) {
      const bool nulo = elems[k].kind == ValueKind::Nulo;
      fl.lv.defs.push_back(
          static_cast<std::uint32_t>(nulo ? fl.lv.max_def - 1 : fl.lv.max_def));
      fl.lv.reps.push_back(k == 0 ? 0u : 1u);
      if (nulo) ++fl.lv.num_nulls;
      ++fl.lv.num_values;
    }
  }
  out.push_back(std::move(fl));
}

void flatten_into(const Column& c, std::vector<std::string> path, int def_base,
                  const std::vector<int>& null_lv, std::deque<Column>& owned,
                  std::vector<FlatLeaf>& out);  // adiante

// Lista de structs (B2b): uma folha por campo do elemento; cada elemento
// (definido, nulo ou ausente) contribui com exatamente uma entrada por campo:
// rep j==0?0:1; def base=nula externa, +O1=vazia, +1=elemento nulo (O2),
// max-1=campo nulo (O3), max=valor. Payloads ja preenchidos na inferencia,
// consumidos em ordem por cursor.
void flatten_struct_list(const Column& c, std::vector<std::string> path, int def_base,
                         const std::vector<int>& null_lv, std::vector<FlatLeaf>& out) {
  const int o1 = c.optional ? 1 : 0;
  const int o2 = c.elem_struct_nullable ? 1 : 0;
  // Cursor de payload por campo (indice no vetor do tipo).
  // Pre-computa as entradas por campo para alinhar (mesma sequencia).
  struct Entrada {
    std::uint32_t rep;
    std::uint32_t def;
    bool tem_valor;
  };
  std::vector<std::vector<Entrada>> entradas(c.children.size());
  for (std::size_t r = 0; r < c.cells.size(); ++r) {
    const int anc = r < null_lv.size() ? null_lv[r] : -1;
    const Value& cell = c.cells[r];
    const bool nula_ext = anc >= 0 || cell.kind == ValueKind::Nulo;
    const bool vazia_ext = !nula_ext && cell.list->empty();
    for (std::size_t f = 0; f < c.children.size(); ++f) {
      const Column& ch = c.children[f];
      const int o3 = ch.optional ? 1 : 0;
      const int maxd = def_base + o1 + 1 + o2 + o3;
      if (nula_ext) {
        const int d = anc >= 0 ? anc : def_base;
        entradas[f].push_back({0u, static_cast<std::uint32_t>(d), false});
        continue;
      }
      if (vazia_ext) {
        entradas[f].push_back({0u, static_cast<std::uint32_t>(def_base + o1), false});
        continue;
      }
      const ValueList& elems = *cell.list;
      for (std::size_t j = 0; j < elems.size(); ++j) {
        const std::uint32_t rep = (j == 0) ? 0u : 1u;
        const Value& e = elems[j];
        if (e.kind != ValueKind::Mapa || !e.map) {
          entradas[f].push_back({rep, static_cast<std::uint32_t>(def_base + o1 + 1), false});
          continue;  // elemento Nulo (O2)
        }
        const Value* fv = e.map->find(ch.name);
        if (!fv || fv->kind == ValueKind::Nulo) {
          entradas[f].push_back({rep, static_cast<std::uint32_t>(maxd - 1), false});
          continue;  // campo nulo (O3)
        }
        entradas[f].push_back({rep, static_cast<std::uint32_t>(maxd), true});
      }
    }
  }
  for (std::size_t f = 0; f < c.children.size(); ++f) {
    const Column& ch = c.children[f];
    FlatLeaf fl;
    fl.path = path;
    fl.path.push_back(ch.name);
    fl.leaf = &ch;
    const int o3 = ch.optional ? 1 : 0;
    fl.lv.max_rep = 1;
    fl.lv.max_def = def_base + o1 + 1 + o2 + o3;
    for (const Entrada& e : entradas[f]) {
      fl.lv.defs.push_back(e.def);
      fl.lv.reps.push_back(e.rep);
      if (!e.tem_valor) ++fl.lv.num_nulls;
      ++fl.lv.num_values;
    }
    out.push_back(std::move(fl));
  }
}

// Lista de listas (B2a): uma folha sintetica com payload proprio; levels com
// rep 0/1/2 (linha / lista interna / elemento) e O1/O2/O3 (nulos em cada
// nivel). def: base=nula externa, base+O1=vazia, +1=interna nula (O2),
// +1+O2=vazia interna, max-1=elemento nulo (O3), max=elemento. O +1 extra
// conta a entrada do grupo `list` repetido que contem a interna.
void flatten_nested(const Column& c, std::vector<std::string> path, int def_base,
                    const std::vector<int>& null_lv, std::deque<Column>& owned,
                    std::vector<FlatLeaf>& out) {
  owned.emplace_back();
  Column& leaf = owned.back();
  leaf.name = c.name;
  leaf.type = c.type;
  leaf.has_type = true;
  leaf.repeated = true;
  leaf.rows = c.rows;
  leaf.optional = c.optional;
  leaf.elem_nullable = c.elem_nullable;

  FlatLeaf fl;
  fl.path = std::move(path);
  fl.leaf = &leaf;
  const int o1 = c.optional ? 1 : 0;
  const int o2 = c.inner_nullable ? 1 : 0;
  const int o3 = c.elem_nullable ? 1 : 0;
  fl.lv.max_rep = 2;
  fl.lv.max_def = def_base + o1 + 1 + o2 + 1 + o3;
  auto poe_valor = [&](const Value& e) {
    switch (leaf.type) {
      case PT_BOOLEAN: leaf.bools.push_back(e.b); break;
      case PT_INT32:
      case PT_INT64:
      case PT_FLOAT:
      case PT_DOUBLE: leaf.nums.push_back(e.as_number()); break;
      case PT_BYTE_ARRAY: leaf.strings.push_back(e.s); break;
      default:
        die("coluna '" + c.name + "': tipo de escrita nao suportado");
    }
  };
  for (std::size_t r = 0; r < c.cells.size(); ++r) {
    const int anc = r < null_lv.size() ? null_lv[r] : -1;
    if (anc >= 0) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(anc));
      fl.lv.reps.push_back(0);
      ++fl.lv.num_nulls;
      ++fl.lv.num_values;
      continue;
    }
    const Value& cell = c.cells[r];
    if (cell.kind == ValueKind::Nulo) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(def_base));
      fl.lv.reps.push_back(0);
      ++fl.lv.num_nulls;
      ++fl.lv.num_values;
      continue;
    }
    const ValueList& iners = *cell.list;
    if (iners.empty()) {
      fl.lv.defs.push_back(static_cast<std::uint32_t>(def_base + o1));
      fl.lv.reps.push_back(0);
      ++fl.lv.num_nulls;
      ++fl.lv.num_values;
      continue;
    }
    for (std::size_t j = 0; j < iners.size(); ++j) {
      const Value& in = iners[j];
      const std::uint32_t rep_ini = (j == 0) ? 0u : 1u;
      if (in.kind == ValueKind::Nulo) {
        fl.lv.defs.push_back(static_cast<std::uint32_t>(def_base + o1 + 1));
        fl.lv.reps.push_back(rep_ini);
        ++fl.lv.num_nulls;
        ++fl.lv.num_values;
        continue;
      }
      const ValueList& elems = *in.list;
      if (elems.empty()) {
        fl.lv.defs.push_back(static_cast<std::uint32_t>(def_base + o1 + 1 + o2));
        fl.lv.reps.push_back(rep_ini);
        ++fl.lv.num_nulls;
        ++fl.lv.num_values;
        continue;
      }
      for (std::size_t k = 0; k < elems.size(); ++k) {
        const bool nulo = elems[k].kind == ValueKind::Nulo;
        fl.lv.defs.push_back(static_cast<std::uint32_t>(
            nulo ? fl.lv.max_def - 1 : fl.lv.max_def));
        fl.lv.reps.push_back(k == 0 ? rep_ini : 2u);
        if (nulo) {
          ++fl.lv.num_nulls;
        } else {
          poe_valor(elems[k]);
        }
        ++fl.lv.num_values;
      }
    }
  }
  out.push_back(std::move(fl));
}

void flatten_into(const Column& c, std::vector<std::string> path, int def_base,
                  const std::vector<int>& null_lv, std::deque<Column>& owned,
                  std::vector<FlatLeaf>& out) {
  if (c.is_struct) {
    std::vector<int> child_null(c.rows, -1);
    for (std::size_t r = 0; r < c.rows; ++r) {
      const int anc = r < null_lv.size() ? null_lv[r] : -1;
      if (anc >= 0) {
        child_null[r] = anc;
      } else if (r < c.struct_defined.size() && !c.struct_defined[r]) {
        child_null[r] = def_base;
      }
    }
    const int child_base = def_base + (c.optional ? 1 : 0);
    for (const Column& ch : c.children) {
      std::vector<std::string> cp = path;
      cp.push_back(ch.name);
      flatten_into(ch, std::move(cp), child_base, child_null, owned, out);
    }
    return;
  }
  if (c.repeated && c.nested_list) {
    flatten_nested(c, std::move(path), def_base, null_lv, owned, out);
    return;
  }
  if (c.repeated && c.struct_list) {
    flatten_struct_list(c, std::move(path), def_base, null_lv, out);
    return;
  }
  if (c.repeated) {
    flatten_list(c, std::move(path), def_base, null_lv, out);
  } else {
    flatten_scalar(c, std::move(path), def_base, null_lv, out);
  }
}

std::vector<FlatLeaf> flatten_columns(const std::vector<Column>& cols,
                                      std::deque<Column>& owned) {
  std::vector<FlatLeaf> out;
  for (const Column& c : cols) {
    std::vector<int> none(c.rows, -1);
    flatten_into(c, {c.name}, 0, none, owned, out);
  }
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
                                std::size_t count, const std::string& col, int conv = 0,
                                int dec_scale = 0, int fixed_len = 0);  // adiante

// Dias desde 1970-01-01 -> (ano, mes, dia) civil (algoritmo de Hinnant).
void civil_de_dias(std::int64_t dias, int& ano, unsigned& mes, unsigned& dia) {
  dias += 719468;
  const std::int64_t era = (dias >= 0 ? dias : dias - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(dias - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  ano = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  dia = doy - (153 * mp + 2) / 5 + 1;
  mes = mp + (mp < 10 ? 3 : -9);
  ano += (mes <= 2 ? 1 : 0);
}

std::string dois(int v) {
  char b[16];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

// DATE (dias) -> "YYYY-MM-DD".
std::string data_iso(std::int64_t dias) {
  int ano;
  unsigned mes, dia;
  civil_de_dias(dias, ano, mes, dia);
  char b[16];
  std::snprintf(b, sizeof b, "%04d-%s-%s", ano, dois(static_cast<int>(mes)).c_str(),
                dois(static_cast<int>(dia)).c_str());
  return b;
}

// Hora do dia: `t` em `por_seg` unidades (modulo 1 dia; negativo satura).
std::string hora_iso(std::int64_t t, std::int64_t por_seg) {
  if (por_seg <= 1) por_seg = 1000;
  std::int64_t resto = t % (por_seg * 86400);
  if (resto < 0) resto += por_seg * 86400;
  const std::int64_t total_seg = resto / por_seg;
  const std::int64_t fracao = resto % por_seg;
  const int hh = static_cast<int>(total_seg / 3600);
  const int mm = static_cast<int>((total_seg % 3600) / 60);
  const int ss = static_cast<int>(total_seg % 60);
  std::string out = dois(hh) + ":" + dois(mm) + ":" + dois(ss);
  if (fracao > 0 && por_seg > 1) {
    char bf[24];
    std::snprintf(bf, sizeof bf, "%09lld",
                  static_cast<long long>(fracao * (1000000000LL / por_seg)));
    std::string fs = bf;
    while (fs.size() > 1 && fs.back() == '0') fs.pop_back();
    out += "." + fs;
  }
  return out;
}

// Timestamp (ms ou us desde a epoca, UTC) -> ISO. `div` = 1000 ou 1000000.
std::string ts_iso(std::int64_t v, int div) {
  const std::int64_t d = div <= 0 ? 1000 : div;
  std::int64_t dias = v / (d * 86400);
  std::int64_t resto = v % (d * 86400);
  if (resto < 0) {
    resto += d * 86400;
    --dias;
  }
  return data_iso(dias) + "T" + hora_iso(resto, d);
}

// INT96 (nanos do dia + dia juliano) -> ISO com nanos.
std::string int96_iso(std::uint64_t nanos, std::uint32_t juliano) {
  const std::int64_t dias = static_cast<std::int64_t>(juliano) - 2440588;
  return data_iso(dias) + "T" + hora_iso(static_cast<std::int64_t>(nanos), 1000000000LL);
}

// Decimal de bytes big-endian com sinal (BYTE_ARRAY/FIXED) com escala.
double decimal_bytes(const std::uint8_t* b, std::size_t n, int escala, const std::string& col) {
  if (n == 0 || n > 8) {
    die("coluna '" + col + "': decimal de " + std::to_string(n) +
        " bytes fora do suportado (1..8)");
  }
  std::int64_t v = (b[0] & 0x80) ? -1 : 0;  // extensao de sinal
  for (std::size_t k = 0; k < n; ++k) v = (v << 8) | b[k];
  double p = 1.0;
  for (int k = 0; k < escala; ++k) p *= 10.0;
  return static_cast<double>(v) / p;
}

std::vector<Value> plain_values(PType t, const std::uint8_t* data, std::size_t avail,
                                std::size_t count, const std::string& col, int conv,
                                int dec_scale, int fixed_len) {
  std::vector<Value> vals;
  vals.reserve(count);
  auto pot10 = [](int s) {
    double p = 1.0;
    for (int k = 0; k < s; ++k) p *= 10.0;
    return p;
  };
  switch (t) {
    case PT_BOOLEAN: {
      if (avail * 8 < count) die("coluna '" + col + "': pagina de boolean truncada");
      for (std::size_t k = 0; k < count; ++k) {
        vals.push_back(Value::logico((data[k / 8] >> (k % 8)) & 1));
      }
      break;
    }
    case PT_INT32: {
      if (avail < count * 4) die("coluna '" + col + "': pagina de int32 truncada");
      for (std::size_t k = 0; k < count; ++k) {
        std::int32_t v;
        std::memcpy(&v, data + k * 4, 4);
        if (conv == 1) {
          vals.push_back(Value::decimal(static_cast<double>(v) / pot10(dec_scale)));
        } else if (conv == 2) {
          vals.push_back(Value::texto(data_iso(v)));
        } else if (conv == 4) {
          vals.push_back(Value::texto(hora_iso(v, dec_scale <= 0 ? 1000 : dec_scale)));
        } else {
          vals.push_back(Value::inteiro(v));
        }
      }
      break;
    }
    case PT_INT64: {
      if (avail < count * 8) die("coluna '" + col + "': pagina de int64 truncada");
      for (std::size_t k = 0; k < count; ++k) {
        std::int64_t v;
        std::memcpy(&v, data + k * 8, 8);
        if (conv == 1) {
          vals.push_back(Value::decimal(static_cast<double>(v) / pot10(dec_scale)));
        } else if (conv == 3) {
          vals.push_back(Value::texto(ts_iso(v, dec_scale <= 0 ? 1000 : dec_scale)));
        } else if (conv == 4) {
          vals.push_back(Value::texto(hora_iso(v, dec_scale <= 0 ? 1000000 : dec_scale)));
        } else {
          vals.push_back(Value::inteiro(v));
        }
      }
      break;
    }
    case PT_INT96: {
      if (avail < count * 12) die("coluna '" + col + "': pagina de int96 truncada");
      for (std::size_t k = 0; k < count; ++k) {
        std::uint64_t nanos;
        std::uint32_t juliano;
        std::memcpy(&nanos, data + k * 12, 8);
        std::memcpy(&juliano, data + k * 12 + 8, 4);
        vals.push_back(Value::texto(int96_iso(nanos, juliano)));
      }
      break;
    }
    case PT_FLOAT: {
      if (avail < count * 4) die("coluna '" + col + "': pagina de float truncada");
      for (std::size_t k = 0; k < count; ++k) {
        float v;
        std::memcpy(&v, data + k * 4, 4);
        vals.push_back(Value::decimal(static_cast<double>(v)));
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
    case PT_FIXED: {
      if (fixed_len <= 0) die("coluna '" + col + "': FIXED sem type_length");
      if (avail < count * static_cast<std::size_t>(fixed_len)) {
        die("coluna '" + col + "': pagina de fixed truncada");
      }
      for (std::size_t k = 0; k < count; ++k) {
        const std::uint8_t* b = data + k * static_cast<std::size_t>(fixed_len);
        if (conv == 1) {
          vals.push_back(Value::decimal(decimal_bytes(b, static_cast<std::size_t>(fixed_len),
                                                       dec_scale, col)));
        } else {
          vals.push_back(Value::texto(std::string(reinterpret_cast<const char*>(b),
                                                   static_cast<std::size_t>(fixed_len))));
        }
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
        if (conv == 1) {
          vals.push_back(Value::decimal(
              decimal_bytes(data + off, len, dec_scale, col)));
        } else {
          vals.push_back(Value::texto(
              std::string(reinterpret_cast<const char*>(data + off), len)));
        }
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
  bool nested_list = false;    // lista de listas (Marco 2 / B2a)
  bool inner_nullable = false;  // grupo element interno OPTIONAL
  // Lista de structs (B2b): marcador de elemento Nulo (def exato).
  int elem_null_level = -1;
  // Lista de structs (B2b): campo de elemento; o nulo da linha externa e
  // def < def_base (sem a clausula legada def == 0).
  bool struct_elem_field = false;
  // Mapas (Fase A3): valores Nulo viram Nulo no mapa em vez de erro.
  bool allow_null_element = false;
  // Base de levels dos structs ancestrais + opcionalidade do proprio grupo
  // LIST: linha de lista e nula quando def <= def_base - 1 (struct ancestral
  // nulo) ou (outer_optional && def == def_base).
  int def_base = 0;
  bool outer_optional = false;
  // Conversao logica (Marco 1 / B1): como interpretar o fisico.
  // 0 = direto (bool/int/float/double/texto), 1 = decimal (divisor 10^escala
  // em dec_scale), 2 = data (dias -> "YYYY-MM-DD"), 3 = timestamp UTC
  // (divisor em dec_scale: 1000 ms / 1000000 us -> ISO), 4 = hora do dia
  // (divisor em dec_scale -> "HH:MM:SS.frac"), 5 = INT96 (-> ISO nanos).
  int conv = 0;
  int dec_scale = 0;
  int fixed_len = 0;
};

// Le um chunk de coluna (todas as paginas entre dictionary/data_page_offset)
// e anexa UM valor por linha em `out` (lista -> Value::lista, nulo ->
// Value::nulo). Quando `row_def` e dado, anexa tambem o definition level
// maximo da linha (para structs: distingue struct nulo de struct definido
// com todos os campos nulos). Suporta: DATA_PAGE v1 e v2, PLAIN e DICTIONARY
// (PLAIN_DICTIONARY/RLE_DICTIONARY), definition/repetition levels RLE para
// campos OPTIONAL e REPEATED (listas aninhadas de escalares, anotacao LIST
// de 3 ou 2 niveis, structs), multiplas paginas por chunk e codecs
// gzip/deflate (zlib dlopen) e snappy (codec proprio).
void decode_chunk(const std::string& file, const ColMeta& cm, const ColDesc& cd,
                  std::int64_t expected_rows, std::vector<Value>& out,
                  std::vector<int>* row_def = nullptr) {
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
      if (dict_enc != E_PLAIN && dict_enc != E_PLAIN_DICTIONARY) {
        die(ctx + ": dictionary page com encoding nao-PLAIN");
      }
      dict = plain_values(cd.type, reinterpret_cast<const std::uint8_t*>(payload.data()),
                          payload.size(), static_cast<std::size_t>(page_values), cd.name,
                          cd.conv, cd.dec_scale, cd.fixed_len);
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
      vals = plain_values(cd.type, vdata, vavail, defined_count, cd.name, cd.conv, cd.dec_scale,
                          cd.fixed_len);
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
        const int def = static_cast<int>(defs[static_cast<std::size_t>(k)]);
        if (def == max_def) {
          out.push_back(vals[vi++]);
        } else {
          out.push_back(Value::nulo());
        }
        if (row_def) row_def->push_back(def);
      }
      continue;
    }
    // REPEATED aninhado (B2a, max_rep 2): rep 0 = linha, 1 = lista interna,
    // 2 = elemento. Niveis (B = def_base, O1 = outer, O2 = inner, O3 = elem):
    // <B+O1 = nula externa; ==B+O1 (sozinha) = vazia; ==B+O1+1 = interna
    // nula (O2) — com lookahead: sozinha tambem pode ser vazia externa;
    // ==B+O1+1+O2 = interna vazia; ==max-1 = elemento nulo (O3); ==max.
    if (cd.max_rep == 2) {
      const int b = cd.def_base;
      const int o1 = cd.outer_optional ? 1 : 0;
      const int o2 = cd.inner_nullable ? 1 : 0;
      const int d_outer = b + o1;
      const int d_inner_null = b + o1 + 1;
      const int d_inner_vazia = b + o1 + 1 + o2;
      std::size_t vi = 0;
      std::size_t k = 0;
      const std::size_t npg = static_cast<std::size_t>(page_values);
      auto no_fim = [&] { return k >= npg; };
      while (!no_fim()) {
        // rep 0: nova linha (a anterior ja foi fechada)
        const std::uint32_t rep0 = reps[k];
        const int def0 = static_cast<int>(defs[k]);
        if (rep0 != 0) {
          die(ctx + ": repetition level inicial de linha != 0 em lista aninhada");
        }
        if (def0 < d_outer) {
          out.push_back(Value::nulo());
          if (row_def) row_def->push_back(def0);
          ++k;
          continue;
        }
        if (def0 == d_outer) {
          // Vazia externa se sozinha (prox rep 0 / fim); senao e a 1a
          // interna (nula ou vazia conforme O2 — tratada no loop abaixo
          // como interna com rep_ini 0).
          const bool sozinha =
              (k + 1 >= npg) || (reps[k + 1] == 0);
          if (sozinha) {
            out.push_back(Value::lista());
            if (row_def) row_def->push_back(def0);
            ++k;
            continue;
          }
        }
        Value row = Value::lista();
        int row_max = def0;
        bool primeira = true;
        while (!no_fim() && (primeira || reps[k] != 0)) {
          const int def = static_cast<int>(defs[k]);
          if (def > row_max) row_max = def;
          if (primeira && def == d_outer) {
            die(ctx + ": definition level de lista externa vazia com entradas a seguir");
          }
          if (def == d_inner_null && o2) {
            row.list->push_back(Value::nulo());  // interna nula
            primeira = false;
            ++k;
            continue;
          }
          if (def == d_inner_vazia && (o2 || def < max_def)) {
            // Interna vazia (O2=0: cai aqui com def==d_inner_vazia).
            row.list->push_back(Value::lista());
            primeira = false;
            ++k;
            continue;
          }
          if (def == max_def) {
            // Elemento: abre a interna corrente se preciso.
            if (primeira || reps[k] == 1) {
              row.list->push_back(Value::lista());
            }
            row.list->back().list->push_back(vals[vi++]);
            primeira = false;
            ++k;
            continue;
          }
          if (cd.elem_nullable && def == max_def - 1) {
            if (!cd.allow_null_element) {
              die(ctx + ": elementos nulos dentro de listas ainda nao suportados");
            }
            if (primeira || reps[k] == 1) {
              row.list->push_back(Value::lista());
            }
            row.list->back().list->push_back(Value::nulo());
            primeira = false;
            ++k;
            continue;
          }
          die(ctx + ": definition level inesperado em lista aninhada");
        }
        out.push_back(std::move(row));
        if (row_def) row_def->push_back(row_max);
      }
      continue;
    }
    std::size_t vi = 0;
    bool have_row = false;
    bool cur_null = false;
    int cur_maxdef = 0;
    Value cur = Value::lista();
    auto flush = [&] {
      out.push_back(cur_null ? Value::nulo() : cur);
      if (row_def) row_def->push_back(cur_maxdef);
    };
    for (std::int64_t k = 0; k < page_values; ++k) {
      const std::uint32_t def = defs[static_cast<std::size_t>(k)];
      if (reps[static_cast<std::size_t>(k)] == 0) {
        if (have_row) flush();
        have_row = true;
        // Lista nula: struct ancestral nulo (def abaixo da base), o
        // proprio grupo LIST nulo (outer OPTIONAL com def na base) ou o
        // caso legado flat (max_def >= 2 com def == 0).
        cur_null = static_cast<int>(def) < cd.def_base ||
                   (cd.outer_optional && static_cast<int>(def) == cd.def_base) ||
                   (!cd.struct_elem_field && max_def >= 2 && def == 0);
        cur_maxdef = static_cast<int>(def);
        cur = Value::lista();
      } else if (static_cast<int>(def) > cur_maxdef) {
        cur_maxdef = static_cast<int>(def);
      }
      if (static_cast<int>(def) == max_def) {
        cur.list->push_back(vals[vi++]);
      } else if (cd.elem_null_level >= 0 && static_cast<int>(def) == cd.elem_null_level) {
        cur.list->push_back(Value::nulo());  // marcador de elemento Nulo (B2b)
      } else if (cd.elem_nullable && static_cast<int>(def) == max_def - 1) {
        if (cd.allow_null_element) {
          cur.list->push_back(Value::nulo());  // valor Nulo em mapa
        } else {
          die(ctx + ": elementos nulos dentro de listas ainda nao suportados");
        }
      }
    }
    if (have_row) flush();
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

// Anota logica INTEGER (LogicalType.INTEGER + ConvertedType INT_*) em um
// SchemaElement INT32/INT64. `w` ja deve ter escrito os campos de id < 10.
void write_logical_integer(Tw& w, int bits, bool is_signed) {
  w.field(10, T_STRUCT);  // logicalType: union LogicalType
  w.struct_begin();
  w.field(9, T_STRUCT);  // LogicalType.INTEGER
  w.struct_begin();
  w.field_i32(1, bits);
  w.field_bool(2, is_signed);
  w.struct_end();
  w.struct_end();
}

// Contagem de SchemaElements da arvore (raiz excluida): folha = 1,
// lista = 3 (grupo LIST + list + element), lista de listas = 5,
// lista de structs = 3 + campos, struct = 1 + filhos.
std::size_t count_schema_nodes(const Column& c) {
  if (c.is_struct) {
    std::size_t n = 1;
    for (const Column& ch : c.children) n += count_schema_nodes(ch);
    return n;
  }
  if (c.repeated && c.nested_list) return 5;
  if (c.repeated && c.struct_list) return 3 + c.children.size();
  return c.repeated ? 3 : 1;
}

// Alocador de field-ids do footer: com `field_ids` (caminho Iceberg) consome
// um id por FOLHA em profundidade (mesma ordem do flatten); sem eles,
// sequencial por folha. Structs aninhados sao suportados quando os ids vêm
// de fora (Iceberg aninha); sem ids, sequencial.
struct FidAlloc {
  const std::vector<int>* ids = nullptr;
  std::size_t pos = 0;
  std::int32_t seq = 1;
  std::int32_t take() {
    if (ids) {
      if (pos >= ids->size()) die("field-ids insuficientes para as folhas");
      return (*ids)[pos++];
    }
    return seq++;
  }
};

void write_schema_tree(Tw& fw, const Column& c, FidAlloc& fa, bool is_top) {
  (void)is_top;
  if (c.is_struct) {
    fw.struct_begin();
    fw.field_i32(3, c.optional ? 1 : 0);  // 0 = REQUIRED, 1 = OPTIONAL
    fw.field_str(4, c.name);
    fw.field_i32(5, static_cast<std::int32_t>(c.children.size()));
    fw.field_i32(9, fa.take());  // field-id do grupo (Iceberg aninha por id)
    fw.struct_end();
    for (const Column& ch : c.children) write_schema_tree(fw, ch, fa, false);
    return;
  }
  const std::int32_t fid = fa.take();
  if (!c.repeated) {
    fw.struct_begin();
    fw.field_i32(1, static_cast<std::int32_t>(c.type));
    fw.field_i32(3, c.optional ? 1 : 0);  // 0 = REQUIRED, 1 = OPTIONAL
    fw.field_str(4, c.name);
    if (c.type == PT_BYTE_ARRAY) fw.field_i32(6, 0);  // ConvertedType.UTF8
    if (c.type == PT_INT32) fw.field_i32(6, 17);      // ConvertedType.INT_32
    fw.field_i32(9, fid);
    if (c.type == PT_BYTE_ARRAY) write_logical_string(fw);
    if (c.type == PT_INT32) write_logical_integer(fw, 32, true);
    fw.struct_end();
    return;
  }
  // lista aninhada (3-level, padrao parquet-mr/pyarrow):
  //   optional|required group <nome> (LIST) {
  //     repeated group list { <tipo> element; }
  //   }
  // Lista de structs (B2b, espelho do pyarrow):
  //   optional|required group <nome> (LIST) {
  //     repeated group list {
  //       optional|required group element { <campos...> }
  //     }
  //   }
  if (c.repeated && c.struct_list) {
    fw.struct_begin();
    fw.field_i32(3, c.optional ? 1 : 0);
    fw.field_str(4, c.name);
    fw.field_i32(5, 1);
    fw.field_i32(6, 3);  // ConvertedType.LIST
    fw.field(10, T_STRUCT);
    fw.struct_begin();
    fw.field(3, T_STRUCT);  // LogicalType.LIST
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
    fw.field_i32(3, c.elem_struct_nullable ? 1 : 0);
    fw.field_str(4, "element");
    fw.field_i32(5, static_cast<std::int32_t>(c.children.size()));
    fw.struct_end();
    for (const Column& ch : c.children) {
      fw.struct_begin();
      fw.field_i32(1, static_cast<std::int32_t>(ch.type));
      fw.field_i32(3, ch.optional ? 1 : 0);
      fw.field_str(4, ch.name);
      fw.field_i32(9, fa.take());
      if (ch.type == PT_BYTE_ARRAY) {
        fw.field_i32(6, 0);
        write_logical_string(fw);
      }
      if (ch.type == PT_INT32) {
        fw.field_i32(6, 17);
        write_logical_integer(fw, 32, true);
      }
      fw.struct_end();
    }
    return;
  }
  // Lista de listas (B2a, espelho do pyarrow):
  //   optional|required group <nome> (LIST) {
  //     repeated group list {
  //       optional|required group element (LIST) {
  //         repeated group list { <tipo> element; }
  //       }
  //     }
  //   }
  if (c.repeated && c.nested_list) {
    fw.struct_begin();
    fw.field_i32(3, c.optional ? 1 : 0);
    fw.field_str(4, c.name);
    fw.field_i32(5, 1);
    fw.field_i32(6, 3);  // ConvertedType.LIST
    fw.field(10, T_STRUCT);
    fw.struct_begin();
    fw.field(3, T_STRUCT);  // LogicalType.LIST
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
    fw.field_i32(3, c.inner_nullable ? 1 : 0);
    fw.field_str(4, "element");
    fw.field_i32(5, 1);
    fw.field_i32(6, 3);  // ConvertedType.LIST
    fw.field(10, T_STRUCT);
    fw.struct_begin();
    fw.field(3, T_STRUCT);  // LogicalType.LIST
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
    fw.field_i32(3, c.elem_nullable ? 1 : 0);
    fw.field_str(4, "element");
    fw.field_i32(9, fid);
    if (c.type == PT_BYTE_ARRAY) {
      fw.field_i32(6, 0);  // ConvertedType.UTF8
      write_logical_string(fw);
    }
    if (c.type == PT_INT32) {
      fw.field_i32(6, 17);  // ConvertedType.INT_32
      write_logical_integer(fw, 32, true);
    }
    fw.struct_end();
    return;
  }
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
  fw.field_i32(3, c.elem_nullable ? 1 : 0);  // element OPTIONAL (B3) ou REQUIRED
  fw.field_str(4, "element");
  fw.field_i32(9, fid);
  if (c.type == PT_BYTE_ARRAY) {
    fw.field_i32(6, 0);  // ConvertedType.UTF8
    write_logical_string(fw);
  }
  if (c.type == PT_INT32) {
    fw.field_i32(6, 17);  // ConvertedType.INT_32
    write_logical_integer(fw, 32, true);
  }
  fw.struct_end();
}

// Estreitamento fisico opt-in (`tipos:`): "int32" (de coluna inteira, com
// checagem de alcance na codificacao) e "float" (de decimal). Caminho
// pontilhado ("col" ou "struct.campo"); lista usa o caminho do grupo.
void aplicar_tipos(std::vector<Column>& cols, const std::map<std::string, std::string>& tipos) {
  if (tipos.empty()) return;
  std::function<void(Column&, const std::string&)> visita = [&](Column& c,
                                                                const std::string& path) {
    if (c.is_struct) {
      for (Column& ch : c.children) visita(ch, path + "." + ch.name);
      return;
    }
    if (c.repeated && c.struct_list) {
      const auto it0 = tipos.find(path);
      if (it0 != tipos.end()) {
        die("tipos: '" + path + "' e lista de structs (use campos: '" + path + ".campo')");
      }
      for (Column& ch : c.children) visita(ch, path + "." + ch.name);
      return;
    }
    const auto it = tipos.find(path);
    if (it == tipos.end()) return;
    if (it->second == "int32") {
      if (c.type != PT_INT64) {
        die("tipos: '" + path + "' int32 exige coluna de inteiros");
      }
      c.type = PT_INT32;
    } else if (it->second == "float") {
      if (c.type != PT_DOUBLE) {
        die("tipos: '" + path + "' float exige coluna de decimais");
      }
      c.type = PT_FLOAT;
    } else {
      die("tipos: '" + path + "' tipo '" + it->second +
          "' invalido (use \"int32\" ou \"float\")");
    }
  };
  for (Column& c : cols) visita(c, c.name);
  for (const auto& [path, tn] : tipos) {
    (void)tn;
    bool achou = false;
    std::function<void(const Column&, const std::string&)> confere =
        [&](const Column& c, const std::string& p) {
          if (p == path) achou = true;
          if (c.is_struct) {
            for (const Column& ch : c.children) confere(ch, p + "." + ch.name);
          }
          if (c.repeated && c.struct_list) {
            for (const Column& ch : c.children) confere(ch, p + "." + ch.name);
          }
        };
    for (const Column& c : cols) confere(c, c.name);
    if (!achou) die("tipos: coluna '" + path + "' nao existe na tabela");
  }
}

// Dicionario de valores para encoding DICTIONARY (Marco 2 / B4): valores
// distintos em PLAIN (ordem de 1a aparicao) + indice por valor definido.
struct DictBuild {
  std::string dict_plain;
  std::vector<std::uint32_t> indices;  // por valor definido, na ordem
  std::size_t distintas = 0;
};

// Largura em bits para enderecar `distintas` entradas (>= 1).
unsigned dict_bit_width(std::size_t distintas) {
  unsigned w = 1;
  while (distintas > (std::size_t{1} << w)) ++w;
  return w;
}

// Codificacao PLAIN de um valor numerico ja convertido (double) no fisico.
std::string dict_num_bytes(PType t, double d) {
  std::string raw;
  if (t == PT_INT32) {
    const std::int32_t v = static_cast<std::int32_t>(static_cast<std::int64_t>(d));
    raw.assign(reinterpret_cast<const char*>(&v), 4);
  } else if (t == PT_FLOAT) {
    const float v = static_cast<float>(d);
    raw.assign(reinterpret_cast<const char*>(&v), 4);
  } else if (t == PT_INT64) {
    const std::int64_t v = static_cast<std::int64_t>(d);
    raw.assign(reinterpret_cast<const char*>(&v), 8);
  } else {
    raw.assign(reinterpret_cast<const char*>(&d), 8);
  }
  return raw;
}

// Constroi o dicionario da folha (nullopt = PLAIN direto): so quando ha
// repeticao (distintos <= definidos/2, teto 1024); deterministico.
std::optional<DictBuild> build_dict(const Column& c, bool permitido) {
  if (!permitido || c.is_struct) return std::nullopt;
  if (c.type != PT_INT32 && c.type != PT_INT64 && c.type != PT_FLOAT &&
      c.type != PT_DOUBLE && c.type != PT_BYTE_ARRAY) {
    return std::nullopt;  // BOOLEAN e afins: PLAIN ja e compacto
  }
  const bool texto = c.type == PT_BYTE_ARRAY;
  const std::size_t definidos = texto ? c.strings.size() : c.nums.size();
  if (definidos < 8) return std::nullopt;
  DictBuild db;
  std::vector<std::string> chaves;  // PLAIN de cada distinto
  chaves.reserve(64);
  auto indice_de = [&](const std::string& raw, bool& novo) -> std::uint32_t {
    for (std::uint32_t k = 0; k < chaves.size(); ++k) {
      if (chaves[k] == raw) {
        novo = false;
        return k;
      }
    }
    if (chaves.size() >= 1024) return 0xFFFFFFFFu;  // teto: desiste
    chaves.push_back(raw);
    novo = true;
    return static_cast<std::uint32_t>(chaves.size() - 1);
  };
  if (texto) {
    for (const std::string& s : c.strings) {
      std::string raw;
      const std::uint32_t n = static_cast<std::uint32_t>(s.size());
      raw.append(reinterpret_cast<const char*>(&n), 4);
      raw += s;
      bool novo = false;
      const std::uint32_t ix = indice_de(raw, novo);
      if (ix == 0xFFFFFFFFu) return std::nullopt;
      if (novo) db.dict_plain += raw;
      db.indices.push_back(ix);
    }
  } else {
    for (double d : c.nums) {
      const std::string raw = dict_num_bytes(c.type, d);
      bool novo = false;
      const std::uint32_t ix = indice_de(raw, novo);
      if (ix == 0xFFFFFFFFu) return std::nullopt;
      if (novo) db.dict_plain += raw;
      db.indices.push_back(ix);
    }
  }
  if (chaves.size() * 2 > definidos) return std::nullopt;  // sem repeticao util
  db.distintas = chaves.size();
  return db;
}

void parquet_write(const std::string& path, const Value& tabela,
                   const std::vector<int>* field_ids, const ParquetWriteOpts& opts) {
  std::vector<Column> cols;
  table_to_columns(tabela, cols);
  aplicar_tipos(cols, opts.tipos);
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
    bool dict = false;                     // dictionary encoding (B4)
    int data_enc = E_PLAIN;                // encoding da data page
    std::int64_t dict_page_offset = -1;    // dictionary page (quando dict)
    std::int64_t dict_uncompressed = 0;
    std::int64_t dict_compressed = 0;
  };
  std::vector<ChunkInfo> infos;

  // Achata a arvore (structs aninhados viram uma folha por campo, listas
  // aninhadas viram uma folha sintetica, com o caminho completo e os levels
  // herdados dos grupos ancestrais).
  std::deque<Column> owned;  // folhas sinteticas (refs estaveis)
  const std::vector<FlatLeaf> leaves = flatten_columns(cols, owned);

  for (const FlatLeaf& fl : leaves) {
    const Column& c = *fl.leaf;
    const ColumnLevels& lv = fl.lv;
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

    // Dictionary encoding (B4): dicionario em PLAIN + pagina de dados com
    // indices RLE (bit width em 1 byte). Fallback para PLAIN sem repeticao.
    const std::optional<DictBuild> dict = build_dict(c, opts.dicionario);
    const int data_enc =
        dict ? (opts.paginas_v2 ? E_RLE_DICTIONARY : E_PLAIN_DICTIONARY) : E_PLAIN;
    std::string values_plain;
    std::string values_section;
    if (dict) {
      const unsigned bw = dict_bit_width(dict->distintas);
      std::string idx = rle_encode_levels(dict->indices, bw);
      values_section.push_back(static_cast<char>(bw));
      values_section += idx;
      values_plain = dict->dict_plain;  // para a dictionary page
    } else {
      values_plain = plain_encode(c);
    }
    std::string payload;
    std::int64_t payload_uncompressed;
    if (opts.paginas_v2) {
      const std::string values_comp = compress_payload(values_section.empty() ? values_plain : values_section, codec, c.name);
      payload = levels + values_comp;
      payload_uncompressed = static_cast<std::int64_t>(
          levels.size() + (values_section.empty() ? values_plain.size() : values_section.size()));
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
      raw += values_section.empty() ? values_plain : values_section;
      payload = compress_payload(raw, codec, c.name);
      payload_uncompressed = static_cast<std::int64_t>(raw.size());
    }

    ChunkInfo ci;
    ci.type = c.type;
    ci.repeated = c.repeated;
    ci.path = fl.path;
    if (c.repeated) {
      ci.path.push_back("list");
      ci.path.push_back("element");
    }
    ci.num_values = lv.num_values;
    ci.num_nulls = lv.num_nulls;
    ci.dict = dict.has_value();
    ci.data_enc = data_enc;

    // Dictionary page antes da data page (valores do dicionario em PLAIN).
    if (dict) {
      ci.dict_page_offset = static_cast<std::int64_t>(body.size());
      const std::string dict_comp = compress_payload(values_plain, codec, c.name);
      Tw dw{body};
      dw.struct_begin();  // PageHeader
      dw.field_i32(1, PG_DICTIONARY);
      dw.field_i32(2, static_cast<std::int32_t>(values_plain.size()));
      dw.field_i32(3, static_cast<std::int32_t>(dict_comp.size()));
      dw.field(7, T_STRUCT);
      dw.struct_begin();  // DictionaryPageHeader
      dw.field_i32(1, static_cast<std::int32_t>(dict->distintas));
      dw.field_i32(2, E_PLAIN);
      dw.struct_end();
      dw.struct_end();  // STOP do PageHeader
      ci.dict_uncompressed = static_cast<std::int64_t>(body.size()) - ci.dict_page_offset +
                             static_cast<std::int64_t>(values_plain.size());
      ci.dict_compressed = static_cast<std::int64_t>(body.size()) - ci.dict_page_offset +
                           static_cast<std::int64_t>(dict_comp.size());
      body += dict_comp;
    }
    ci.data_page_offset = static_cast<std::int64_t>(body.size());

    Tw hw{body};
    hw.struct_begin();  // PageHeader
    if (!opts.paginas_v2) {
      hw.field_i32(1, PG_DATA);
      hw.field_i32(2, static_cast<std::int32_t>(payload_uncompressed));
      hw.field_i32(3, static_cast<std::int32_t>(payload.size()));
      hw.field(5, T_STRUCT);
      hw.struct_begin();
      hw.field_i32(1, static_cast<std::int32_t>(lv.num_values));
      hw.field_i32(2, data_enc);
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
      hw.field_i32(4, data_enc);
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
    if (ci.dict) {
      // Tamanhos cobrem dictionary + data page (mesmo ColumnChunk).
      ci.uncompressed_size += ci.dict_uncompressed;
      ci.compressed_size += ci.dict_compressed;
    }
    infos.push_back(ci);
    body += payload;
  }

  const std::int64_t total_bytes = static_cast<std::int64_t>(body.size()) - 4;

  // FileMetaData
  std::string footer;
  Tw fw{footer};
  fw.field_i32(1, 1);  // version
  std::size_t schema_elems = 1;
  for (const Column& c : cols) schema_elems += count_schema_nodes(c);
  fw.list_begin(2, T_STRUCT, schema_elems);
  {
    fw.struct_begin();
    fw.field_str(4, "schema");
    fw.field_i32(5, static_cast<std::int32_t>(cols.size()));
    fw.struct_end();
    FidAlloc fa{field_ids, 0, 1};
    for (const Column& c : cols) write_schema_tree(fw, c, fa, true);
  }
  fw.field_i64(3, static_cast<std::int64_t>(nrows));
  fw.list_begin(4, T_STRUCT, 1);
  {
    fw.struct_begin();
    fw.list_begin(1, T_STRUCT, leaves.size());
    for (std::size_t k = 0; k < leaves.size(); ++k) {
      fw.struct_begin();
      fw.field_i64(2, infos[k].data_page_offset);  // ColumnChunk.file_offset
      fw.field(3, T_STRUCT);                        // ColumnChunk.meta_data
      fw.struct_begin();
      fw.field_i32(1, static_cast<std::int32_t>(infos[k].type));
      if (infos[k].dict) {
        fw.list_begin(2, T_I32, 3);
        fw.zz(E_PLAIN);
        fw.zz(E_RLE);
        fw.zz(infos[k].data_enc);
      } else {
        fw.list_begin(2, T_I32, 2);
        fw.zz(E_PLAIN);
        fw.zz(E_RLE);
      }
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
      if (infos[k].dict) {
        fw.field_i64(11, infos[k].dict_page_offset);  // dictionary_page_offset
      }
      fw.struct_end();
      fw.struct_end();
    }
    fw.field_i64(2, total_bytes);               // total_byte_size
    fw.field_i64(3, static_cast<std::int64_t>(nrows));
    fw.struct_end();
  }
  const char* codec_nome = codec == C_GZIP ? "gzip" : codec == C_SNAPPY ? "snappy" : "sem compressao";
  fw.field_str(6, std::string("tilt 0.1.0 (parquet: plain/dictionary, paginas ") +
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

// No de schema achatado (usa ColDesc para as folhas). Era local de
// parquet_read; içado para permitir abertura/leitura por row group.
struct RField {
  std::string name;
  bool is_struct = false;
  bool is_list = false;
  bool is_map = false;      // grupo MAP: key/value como listas zipadas
  bool nested_list = false;  // lista de listas (Marco 2 / B2a)
  bool inner_nullable = false;  // grupo element interno OPTIONAL
  bool struct_list = false;  // lista de structs (Marco 2 / B2b)
  bool elem_struct_nullable = false;  // elemento struct Nulo (OPTIONAL)
  int elem_null_level = -1;  // def do marcador de elemento Nulo
  bool optional = false;  // rep==1 no proprio nivel (struct) ou outer (lista)
  bool repeated = false;  // folha de lista
  bool elem_nullable = false;
  bool allow_null_element = false;  // valores Nulo em mapa
  bool struct_elem_field = false;  // campo de elemento de lista de structs
  PType leaf_type = PT_BYTE_ARRAY;
  int max_def = 0;
  int max_rep = 0;
  int def_base = 0;       // soma dos optionals dos structs ancestrais
  bool outer_optional = false;
  int conv = 0;           // conversao logica (ver ColDesc)
  int dec_scale = 0;      // escala (decimal) ou divisor (hora/timestamp)
  int fixed_len = 0;      // FIXED_LEN_BYTE_ARRAY
  int null_level = -1;    // struct/map OPTIONAL: def <= null_level = nulo
  int leaf_idx = -1;
  int value_idx = -1;  // mapa: folha dos valores (chave = leaf_idx)
  std::vector<int> sub_leaves;  // folhas da subarvore (structs/mapas)
  std::vector<RField> children;
};

// Arquivo parquet aberto: bytes + schema + metadados dos row groups.
// Permite decodificar grupo a grupo sem materializar o arquivo todo.
 struct LeitorParquet {
   std::string path;
   std::string file;
   std::vector<RField> top;
   std::vector<ColDesc> cols_desc;
   std::vector<std::vector<ColMeta>> row_groups;
   std::vector<std::int64_t> rg_num_rows;
   std::int64_t num_rows = 0;
 };

 LeitorParquet abrir_parquet(const std::string& path);

// Remonta um valor (folha/struct/lista/mapa) da linha `r` sobre colunas ja
// decodificadas. Era lambda local de parquet_read; virou funcao para reuso
// na leitura por row group.
Value montar_no(const RField& f, std::size_t r,
                const std::vector<std::vector<Value>>& columns,
                const std::vector<std::vector<int>>& coldefs) {
  if (f.struct_list) {
    // Linha nula (externa ou ancestral): todos os campos Nulo -> Nulo;
    // vazia: todos [] -> []; senao zipa por posicao.
    bool tudo_nulo = true;
    for (const RField& ch : f.children) {
      const auto& col = columns[static_cast<std::size_t>(ch.leaf_idx)];
      if (r < col.size() && col[r].kind == ValueKind::Lista) {
        tudo_nulo = false;
        break;
      }
    }
    if (tudo_nulo) return Value::nulo();
    Value out = Value::lista();
    std::size_t n = 0;
    for (const RField& ch : f.children) {
      const auto& col = columns[static_cast<std::size_t>(ch.leaf_idx)];
      if (r < col.size() && col[r].kind == ValueKind::Lista && col[r].list) {
        n = std::max(n, col[r].list->size());
      }
    }
    for (std::size_t j = 0; j < n; ++j) {
      bool algum = false;
      Value m = Value::mapa();
      for (const RField& ch : f.children) {
        const auto& col = columns[static_cast<std::size_t>(ch.leaf_idx)];
        Value v;
        if (r < col.size() && col[r].kind == ValueKind::Lista && col[r].list &&
            j < col[r].list->size()) {
          v = (*col[r].list)[j];
        }
        if (v.kind != ValueKind::Nulo) algum = true;
        m.map->set(ch.name, std::move(v));
      }
      if (f.elem_struct_nullable && !algum) {
        out.list->push_back(Value::nulo());
      } else {
        out.list->push_back(std::move(m));
      }
    }
    return out;
  }
  if (f.is_map) {
    const auto& keys = columns[static_cast<std::size_t>(f.leaf_idx)];
    const auto& vals = columns[static_cast<std::size_t>(f.value_idx)];
    const Value& k = r < keys.size() ? keys[r] : Value::nulo();
    const Value& v = r < vals.size() ? vals[r] : Value::nulo();
    // Mapa nulo quando as chaves sao Nulo (struct ancestral nulo ou mapa
    // nulo: def abaixo da base em ambas as folhas).
    if (k.kind != ValueKind::Lista || v.kind != ValueKind::Lista) {
      return Value::nulo();
    }
    if (k.list->size() != v.list->size()) {
      die("coluna '" + f.name + "': chaves e valores do mapa com tamanhos diferentes");
    }
    Value m = Value::mapa();
    for (std::size_t i = 0; i < k.list->size(); ++i) {
      const Value& key = (*k.list)[i];
      if (key.kind != ValueKind::Texto) {
        die("coluna '" + f.name + "': chave de mapa nao-texto");
      }
      m.map->set(key.s, (*v.list)[i]);
    }
    return m;
  }
  if (!f.is_struct) {
    const auto& col = columns[static_cast<std::size_t>(f.leaf_idx)];
    if (r >= col.size()) die("coluna '" + f.name + "' tem menos valores que 'num_rows'");
    return col[r];
  }
  if (f.optional) {
    bool definido = false;
    for (int li : f.sub_leaves) {
      const auto& dd = coldefs[static_cast<std::size_t>(li)];
      if (r < dd.size() && dd[r] > f.null_level) {
        definido = true;
        break;
      }
    }
    if (!definido) return Value::nulo();
  }
  Value m = Value::mapa();
  for (const RField& ch : f.children) m.map->set(ch.name, montar_no(ch, r, columns, coldefs));
  return m;
}

LeitorParquet abrir_parquet(const std::string& path) {
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
    int type_length = 0;  // FIXED_LEN_BYTE_ARRAY
    int scale = 0;        // DECIMAL (converted ou logico)
    int precision = 0;
    int converted = -1;
    bool logical_list = false;
    bool logical_map = false;
    bool logical_date = false;
    bool logical_ts_millis = false;
    bool logical_ts_micros = false;
    bool logical_time_micros = false;
    bool logical_integer = false;
    int logical_int_bits = 0;
    bool logical_int_signed = true;
    bool logical_decimal = false;
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
            else if (sid == 2 && st == T_I32) e.type_length = static_cast<int>(tr.zz());
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
            } else if (sid == 7 && st == T_I32) {
              e.scale = static_cast<int>(tr.zz());
            } else if (sid == 8 && st == T_I32) {
              e.precision = static_cast<int>(tr.zz());
            } else if (sid == 10 && st == T_STRUCT) {  // logicalType (union)
              short llast = 0;
              while (true) {
                const std::uint8_t lh2 = tr.byte();
                const auto lt = static_cast<TType>(lh2 & 0xF);
                if (lt == T_STOP) break;
                const short lid = (lh2 >> 4) ? static_cast<short>(llast + (lh2 >> 4))
                                             : static_cast<short>(tr.zz());
                llast = lid;
                bool consumido = false;
                if (lid == 2) e.logical_map = true;    // LogicalType.MAP
                if (lid == 3) e.logical_list = true;  // LogicalType.LIST
                if (lid == 5 && lt == T_STRUCT) {  // LogicalType.DECIMAL
                  Tr sub{tr.p, tr.n, tr.pos};
                  short dlast = 0;
                  while (true) {
                    const std::uint8_t dh = sub.byte();
                    const auto dt = static_cast<TType>(dh & 0xF);
                    if (dt == T_STOP) break;
                    const short did = (dh >> 4) ? static_cast<short>(dlast + (dh >> 4))
                                                : static_cast<short>(sub.zz());
                    dlast = did;
                    if (did == 1 && dt == T_I32) e.scale = static_cast<int>(sub.zz());
                    else if (did == 2 && dt == T_I32) e.precision = static_cast<int>(sub.zz());
                    else sub.skip(dt);
                  }
                  tr.pos = sub.pos;
                  e.logical_decimal = true;
                } else if (lid == 6) {
                  e.logical_date = true;  // LogicalType.DATE (struct vazio)
                  tr.skip(lt);
                  consumido = true;
                } else if (lid == 7 && lt == T_STRUCT) {  // LogicalType.TIME
                  Tr sub{tr.p, tr.n, tr.pos};
                  short dlast = 0;
                  // unit: I32 (antigo: 0=ms 1=us) ou union {MILLIS:1,
                  // MICROS:2, NANOS:3} (novo).
                  int unit = -1;
                  while (true) {
                    const std::uint8_t dh = sub.byte();
                    const auto dt = static_cast<TType>(dh & 0xF);
                    if (dt == T_STOP) break;
                    const short did = (dh >> 4) ? static_cast<short>(dlast + (dh >> 4))
                                                : static_cast<short>(sub.zz());
                    dlast = did;
                    if (did == 2 && dt == T_I32) {
                      unit = static_cast<int>(sub.zz());
                    } else if (did == 2 && dt == T_STRUCT) {
                      short ulast = 0;
                      while (true) {
                        const std::uint8_t uh = sub.byte();
                        const auto ut = static_cast<TType>(uh & 0xF);
                        if (ut == T_STOP) break;
                        const short uid = (uh >> 4) ? static_cast<short>(ulast + (uh >> 4))
                                                    : static_cast<short>(sub.zz());
                        ulast = uid;
                        if (uid == 1) unit = 0;
                        else if (uid == 2) unit = 1;
                        else if (uid == 3) unit = 2;
                        sub.skip(ut);
                      }
                    } else sub.skip(dt);
                  }
                  tr.pos = sub.pos;
                  consumido = true;
                  if (unit == 1) e.logical_time_micros = true;
                } else if (lid == 8 && lt == T_STRUCT) {  // LogicalType.TIMESTAMP
                  Tr sub{tr.p, tr.n, tr.pos};
                  short dlast = 0;
                  int unit = -1;
                  while (true) {
                    const std::uint8_t dh = sub.byte();
                    const auto dt = static_cast<TType>(dh & 0xF);
                    if (dt == T_STOP) break;
                    const short did = (dh >> 4) ? static_cast<short>(dlast + (dh >> 4))
                                                : static_cast<short>(sub.zz());
                    dlast = did;
                    if (did == 2 && dt == T_I32) {
                      unit = static_cast<int>(sub.zz());
                    } else if (did == 2 && dt == T_STRUCT) {
                      short ulast = 0;
                      while (true) {
                        const std::uint8_t uh = sub.byte();
                        const auto ut = static_cast<TType>(uh & 0xF);
                        if (ut == T_STOP) break;
                        const short uid = (uh >> 4) ? static_cast<short>(ulast + (uh >> 4))
                                                    : static_cast<short>(sub.zz());
                        ulast = uid;
                        if (uid == 1) unit = 0;
                        else if (uid == 2) unit = 1;
                        else if (uid == 3) unit = 2;
                        sub.skip(ut);
                      }
                    } else sub.skip(dt);
                  }
                  tr.pos = sub.pos;
                  consumido = true;
                  if (unit == 1) e.logical_ts_micros = true;
                  else e.logical_ts_millis = true;
                } else if (lid == 9 && lt == T_STRUCT) {  // LogicalType.INTEGER
                  Tr sub{tr.p, tr.n, tr.pos};
                  short dlast = 0;
                  while (true) {
                    const std::uint8_t dh = sub.byte();
                    const auto dt = static_cast<TType>(dh & 0xF);
                    if (dt == T_STOP) break;
                    const short did = (dh >> 4) ? static_cast<short>(dlast + (dh >> 4))
                                                : static_cast<short>(sub.zz());
                    dlast = did;
                    if (did == 1 && dt == T_BYTE) {
                      e.logical_int_bits = sub.byte();  // i8, nao varint
                    } else if (did == 2 && (dt == T_TRUE || dt == T_FALSE)) {
                      e.logical_int_signed = dt == T_TRUE;
                    } else sub.skip(dt);
                  }
                  tr.pos = sub.pos;
                  e.logical_integer = true;
                  consumido = true;
                }
                if (lid >= 5 && lid <= 9) consumido = true;
                if (!consumido) tr.skip(lt);
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

  // extrai os campos da arvore de schema (filhos da raiz): escalares
  // REQUIRED/OPTIONAL, listas aninhadas (anotacao LIST de 3 ou 2 niveis,
  // incluindo o legado `repeated <tipo>` direto) e structs aninhados
  // (grupos sem anotacao LIST, recursivos). Erro claro para o resto
  // (listas de structs, listas de listas, elementos opcionais em lista).
  auto children_of = [&](int idx) {
    std::vector<int> out;
    for (int k = 0; k < static_cast<int>(selem.size()); ++k) {
      if (selem[static_cast<std::size_t>(k)].parent == idx) out.push_back(k);
    }
    return out;
  };
  auto check_leaf_type = [&](const std::string& col, int t) {
    if (t != PT_BOOLEAN && t != PT_INT32 && t != PT_INT64 && t != PT_INT96 &&
        t != PT_FLOAT && t != PT_DOUBLE && t != PT_BYTE_ARRAY && t != PT_FIXED) {
      die("coluna '" + col + "': tipo fisico " + std::to_string(t) + " nao suportado");
    }
  };
  // Conversao logica de uma folha a partir do schema (converted + logical):
  // devolve (fisico, conv, escala/divisor, len fixo). Erro claro no que segue
  // fora (ex.: FIXED gigante, decimal sem escala).
  auto resolve_conv = [&](const std::string& col, const SchemaElem& e, PType& fisico, int& conv,
                          int& escala, int& flen) {
    fisico = static_cast<PType>(e.type);
    conv = 0;
    escala = 0;
    flen = e.type_length;
    const bool dec_c = e.converted == 5;
    const bool date_c = e.converted == 6;
    const bool tms_c = e.converted == 7;
    const bool tus_c = e.converted == 8;
    const bool tsms_c = e.converted == 9;
    const bool tsus_c = e.converted == 10;
    const bool is_dec = dec_c || e.logical_decimal;
    const bool is_date = date_c || e.logical_date;
    if (is_dec) {
      if (e.scale < 0) die("coluna '" + col + "': decimal sem escala");
      conv = 1;
      escala = e.scale;
      return;
    }
    if (is_date) {
      if (e.type != PT_INT32) die("coluna '" + col + "': DATE fora de INT32");
      conv = 2;
      return;
    }
    if (tms_c) {
      if (e.type != PT_INT32) die("coluna '" + col + "': TIME_MILLIS fora de INT32");
      conv = 4;
      escala = 1000;
      return;
    }
    if (tus_c || e.logical_time_micros) {
      if (e.type != PT_INT64) die("coluna '" + col + "': TIME_MICROS fora de INT64");
      conv = 4;
      escala = 1000000;
      return;
    }
    if (tsms_c || e.logical_ts_millis) {
      if (e.type != PT_INT64) die("coluna '" + col + "': TIMESTAMP_MILLIS fora de INT64");
      conv = 3;
      escala = 1000;
      return;
    }
    if (tsus_c || e.logical_ts_micros) {
      if (e.type != PT_INT64) die("coluna '" + col + "': TIMESTAMP_MICROS fora de INT64");
      conv = 3;
      escala = 1000000;
      return;
    }
    if (e.type == PT_FIXED && flen <= 0) {
      die("coluna '" + col + "': FIXED_LEN_BYTE_ARRAY sem type_length");
    }
  };
  // Arvore de campos lidos do schema (Fase 12-5a): escalares, listas e
  // structs aninhados. `max_def/max_rep` acumulam os niveis dos grupos
  // ancestrais (struct OPTIONAL soma 1, como o outer das listas).
  std::function<RField(int, int)> parse_no;
  parse_no = [&](int idx, int def_base) -> RField {
    const SchemaElem& e = selem[static_cast<std::size_t>(idx)];
    RField f;
    f.name = e.name;
    if (e.type >= 0) {  // primitivo
      if (e.rep == 2) {  // `repeated <tipo>`: lista (legado na raiz, 2-level no struct)
        check_leaf_type(e.name, e.type);
        resolve_conv(e.name, e, f.leaf_type, f.conv, f.dec_scale, f.fixed_len);
        f.is_list = true;
        f.repeated = true;
        f.max_rep = 1;
        f.max_def = def_base + 1;
        f.def_base = def_base;
        f.outer_optional = false;
        return f;
      }
      if (e.rep > 1) die("coluna '" + e.name + "': repetition_type invalido");
      check_leaf_type(e.name, e.type);
      resolve_conv(e.name, e, f.leaf_type, f.conv, f.dec_scale, f.fixed_len);
      f.optional = e.rep == 1;
      f.max_def = def_base + (e.rep == 1 ? 1 : 0);
      f.def_base = def_base;
      return f;
    }
    // grupo: LIST anotado = lista; senao, struct.
    if (e.converted == 3 || e.logical_list) {
      if (e.rep > 1) die("coluna '" + e.name + "': grupo LIST com repetition_type invalido");
      f.is_list = true;
      f.repeated = true;
      f.max_rep = 1;
      f.optional = e.rep == 1;
      f.def_base = def_base;
      f.outer_optional = e.rep == 1;
      const int outer = def_base + (e.rep == 1 ? 1 : 0);
      const std::vector<int> gk = children_of(idx);
      if (gk.size() != 1) {
        die("coluna '" + e.name + "': grupo LIST com " + std::to_string(gk.size()) +
            " filhos (esperado 1: lista de escalares)");
      }
      const SchemaElem& g = selem[static_cast<std::size_t>(gk[0])];
      if (g.type >= 0) {  // 2-level: repeated <tipo> dentro do grupo LIST
        check_leaf_type(e.name, g.type);
        resolve_conv(e.name, g, f.leaf_type, f.conv, f.dec_scale, f.fixed_len);
        f.max_def = outer + 1;
        return f;
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
      if (el.type < 0 && el.converted != 3 && !el.logical_list) {
        // Lista de structs (B2b): grupo element sem anotacao LIST; campos
        // escalares (listas/mapas/structs internos: erro claro). Cada campo
        // vira folha-lista (rep 0/1 por elemento) e a remontagem zipa.
        if (el.rep > 1) {
          die("coluna '" + e.name + "': grupo element com repetition_type invalido");
        }
        f.struct_list = true;
        f.elem_struct_nullable = el.rep == 1;
        f.repeated = true;
        f.max_rep = 1;
        f.optional = e.rep == 1;
        f.def_base = def_base;
        f.outer_optional = e.rep == 1;
        const int fbase = outer + 1 + (el.rep == 1 ? 1 : 0);
        for (int fk : children_of(ek[0])) {
          RField ch = parse_no(fk, fbase);
          if (ch.is_struct || ch.is_map || ch.is_list || ch.nested_list) {
            die("coluna '" + e.name + "': campo '" + ch.name +
                "' composto em elemento de lista ainda nao suportado");
          }
          // Decodifica como lista (uma entrada por elemento): Nulo de
          // elemento vira placeholder em todos os campos.
          ch.repeated = true;
          ch.is_list = true;
          ch.max_rep = 1;
          ch.elem_null_level = el.rep == 1 ? outer + 1 : -1;
          ch.allow_null_element = true;
          ch.struct_elem_field = true;
          ch.def_base = outer;
          ch.outer_optional = false;
          f.children.push_back(std::move(ch));
        }
        if (f.children.empty()) {
          die("coluna '" + e.name + "': elemento struct sem campos");
        }
        f.max_def = fbase;
        for (const RField& ch : f.children) f.max_def = std::max(f.max_def, ch.max_def);
        return f;
      }
      if (el.type < 0) {
        // Lista de listas (B2a): grupo element com anotacao LIST.
        if (el.rep > 1) {
          die("coluna '" + e.name + "': grupo element com repetition_type invalido");
        }
        f.nested_list = true;
        f.inner_nullable = el.rep == 1;
        const int obase = outer + 1;
        const std::vector<int> ik = children_of(ek[0]);
        if (ik.size() != 1) {
          die("coluna '" + e.name + "': lista interna com " + std::to_string(ik.size()) +
              " filhos (esperado 1)");
        }
        const SchemaElem& ig = selem[static_cast<std::size_t>(ik[0])];
        if (ig.type >= 0 || ig.rep != 2) {
          die("coluna '" + e.name + "': grupo interno da lista interna deve ser REPEATED");
        }
        const std::vector<int> lk = children_of(ik[0]);
        if (lk.size() != 1) {
          die("coluna '" + e.name + "': 3 niveis de lista ainda nao suportados");
        }
        const SchemaElem& leaf = selem[static_cast<std::size_t>(lk[0])];
        if (leaf.type < 0) {
          die("coluna '" + e.name + "': 3 niveis de lista ainda nao suportados");
        }
        if (leaf.rep > 1) {
          die("coluna '" + e.name + "': elementos REPEATED na lista interna");
        }
        check_leaf_type(e.name, leaf.type);
        resolve_conv(e.name, leaf, f.leaf_type, f.conv, f.dec_scale, f.fixed_len);
        f.elem_nullable = leaf.rep == 1;
        f.max_rep = 2;
        f.max_def = obase + (el.rep == 1 ? 1 : 0) + 1 + (f.elem_nullable ? 1 : 0);
        return f;
      }
      if (el.rep > 1) {
        die("coluna '" + e.name +
            "': elementos REPEATED dentro de lista (listas aninhadas) ainda nao suportados");
      }
      check_leaf_type(e.name, el.type);
      resolve_conv(e.name, el, f.leaf_type, f.conv, f.dec_scale, f.fixed_len);
      f.elem_nullable = el.rep == 1;  // pyarrow grava element OPTIONAL (max_def 3)
      f.max_def = outer + 1 + (f.elem_nullable ? 1 : 0);
      return f;
    }
    // grupo MAP (Fase A3): `optional group m (MAP) { repeated group
    // key_value { required <t> key; <optional|required> <t> value; } }`.
    // Vira duas folhas-lista (chaves + valores) zipadas na remontagem.
    if (e.converted == 1 || e.logical_map) {
      if (e.rep > 1) die("coluna '" + e.name + "': grupo MAP com repetition_type invalido");
      f.is_map = true;
      f.optional = e.rep == 1;
      f.null_level = e.rep == 1 ? def_base : -1;
      const int obase = def_base + (e.rep == 1 ? 1 : 0);
      const std::vector<int> gk = children_of(idx);
      if (gk.size() != 1) {
        die("coluna '" + e.name + "': grupo MAP com " + std::to_string(gk.size()) +
            " filhos (esperado 1: key_value)");
      }
      const SchemaElem& g = selem[static_cast<std::size_t>(gk[0])];
      if (g.type >= 0 || g.rep != 2) {
        die("coluna '" + e.name + "': grupo interno de MAP deve ser REPEATED");
      }
      const std::vector<int> kk = children_of(gk[0]);
      if (kk.size() != 2) {
        die("coluna '" + e.name + "': key_value com " + std::to_string(kk.size()) +
            " campos (esperados key + value)");
      }
      const SchemaElem& ke = selem[static_cast<std::size_t>(kk[0])];
      const SchemaElem& ve = selem[static_cast<std::size_t>(kk[1])];
      if (ke.type < 0 || ke.rep != 0) {
        die("coluna '" + e.name + "': chave de mapa deve ser escalar REQUIRED");
      }
      if (ve.type < 0 || ve.rep > 1) {
        die("coluna '" + e.name + "': valor de mapa deve ser escalar");
      }
      check_leaf_type(e.name + ".key", ke.type);
      check_leaf_type(e.name + ".value", ve.type);
      RField kf;
      kf.name = ke.name;
      kf.is_list = true;
      kf.repeated = true;
      kf.leaf_type = static_cast<PType>(ke.type);
      kf.max_rep = 1;
      kf.max_def = obase + 1;
      kf.def_base = obase;
      RField vf;
      vf.name = ve.name;
      vf.is_list = true;
      vf.repeated = true;
      resolve_conv(e.name + ".value", ve, vf.leaf_type, vf.conv, vf.dec_scale, vf.fixed_len);
      vf.max_rep = 1;
      vf.elem_nullable = ve.rep == 1;
      vf.allow_null_element = ve.rep == 1;
      vf.max_def = obase + 1 + (ve.rep == 1 ? 1 : 0);
      vf.def_base = obase;
      f.children.push_back(std::move(kf));
      f.children.push_back(std::move(vf));
      return f;
    }
    if (e.rep > 1) die("grupo '" + e.name + "': repetition_type invalido em struct");
    f.is_struct = true;
    f.optional = e.rep == 1;
    f.null_level = e.rep == 1 ? def_base : -1;
    const int child_base = def_base + (e.rep == 1 ? 1 : 0);
    for (int k : children_of(idx)) f.children.push_back(parse_no(k, child_base));
    if (f.children.empty()) die("grupo '" + e.name + "': struct sem campos");
    return f;
  };
  std::vector<RField> top;
  if (selem.empty()) die("schema vazio em '" + path + "'");
  for (int ci : children_of(0)) top.push_back(parse_no(ci, 0));
  // Achata as folhas em ordem de schema (1 chunk por folha) e preenche ColDesc.
  std::vector<ColDesc> cols_desc;
  std::function<void(RField&)> flat_no = [&](RField& f) {
    if (f.is_struct) {
      for (RField& ch : f.children) {
        flat_no(ch);
        f.sub_leaves.insert(f.sub_leaves.end(), ch.sub_leaves.begin(), ch.sub_leaves.end());
      }
      return;
    }
    if (f.is_map) {
      for (RField& ch : f.children) {
        flat_no(ch);
        f.sub_leaves.insert(f.sub_leaves.end(), ch.sub_leaves.begin(), ch.sub_leaves.end());
      }
      // chave = 1a folha, valor = 2a
      f.leaf_idx = f.children[0].leaf_idx;
      f.value_idx = f.children[1].leaf_idx;
      return;
    }
    if (f.struct_list) {
      for (RField& ch : f.children) {
        flat_no(ch);
        f.sub_leaves.insert(f.sub_leaves.end(), ch.sub_leaves.begin(), ch.sub_leaves.end());
      }
      return;
    }
    f.leaf_idx = static_cast<int>(cols_desc.size());
    f.sub_leaves.push_back(f.leaf_idx);
    ColDesc d;
    d.name = f.name;
    d.type = f.leaf_type;
    d.max_def = f.max_def;
    d.max_rep = f.max_rep;
    d.repeated = f.repeated;
    d.elem_nullable = f.elem_nullable;
    d.nested_list = f.nested_list;
    d.inner_nullable = f.inner_nullable;
    d.elem_null_level = f.elem_null_level;
    d.struct_elem_field = f.struct_elem_field;
    // B3: elemento Nulo vira Nulo (listas comuns e valores de mapa).
    d.allow_null_element = f.allow_null_element || f.elem_nullable;
    d.def_base = f.def_base;
    d.outer_optional = f.outer_optional;
    d.conv = f.conv;
    d.dec_scale = f.dec_scale;
    d.fixed_len = f.fixed_len;
    cols_desc.push_back(std::move(d));
  };
  for (RField& t : top) flat_no(t);

  const std::size_t ncols = cols_desc.size();
  if (ncols == 0) die("schema sem colunas em '" + path + "'");
  for (std::size_t rg = 0; rg < row_groups.size(); ++rg) {
    if (row_groups[rg].size() != ncols) {
      die("row group com " + std::to_string(row_groups[rg].size()) + " colunas, mas o schema tem " +
          std::to_string(ncols));
    }
    if (rg_num_rows[rg] < 0) rg_num_rows[rg] = num_rows;
  }
  LeitorParquet lp;
  lp.path = path;
  lp.file = std::move(file);
  lp.top = std::move(top);
  lp.cols_desc = std::move(cols_desc);
  lp.row_groups = std::move(row_groups);
  lp.rg_num_rows = std::move(rg_num_rows);
  lp.num_rows = num_rows;
  return lp;
}

Value parquet_read(const std::string& path) {
  LeitorParquet lp = abrir_parquet(path);
  const std::string& file = lp.file;
  const std::vector<RField>& top = lp.top;
  const std::vector<ColDesc>& cols_desc = lp.cols_desc;
  const std::vector<std::vector<ColMeta>>& row_groups = lp.row_groups;
  const std::vector<std::int64_t>& rg_num_rows = lp.rg_num_rows;
  const std::int64_t num_rows = lp.num_rows;
  const std::size_t ncols = cols_desc.size();

  // decodifica cada coluna: percorre os row groups concatenando os chunks
  std::vector<std::vector<Value>> columns(ncols);
  std::vector<std::vector<int>> coldefs(ncols);
  for (std::size_t ci = 0; ci < ncols; ++ci) {
    auto& col = columns[ci];
    auto& defs = coldefs[ci];
    for (std::size_t rg = 0; rg < row_groups.size(); ++rg) {
      decode_chunk(file, row_groups[rg][ci], cols_desc[ci], rg_num_rows[rg], col, &defs);
    }
  }

  // Remonta os valores: folhas viram escalares/listas; structs viram mapas.
  // Struct OPTIONAL e Nulo quando nenhum definition level da subarvore passa
  // do nivel do struct (tudo indefinido a partir dele); senao e mapa (com
  // Nulo nos campos ausentes). Struct REQUIRED e sempre mapa. Listas de
  // structs zipam os campos por posicao (elemento todo-Nulo vira Nulo
  // quando o grupo element e OPTIONAL).
  // Remontagem via montar_no (funcao de arquivo, reutilizada na leitura por grupo).

  Value tabela = Value::tabela();
  for (std::int64_t r = 0; r < num_rows; ++r) {
    Value row = Value::mapa();
    for (const RField& t : top) {
      row.map->set(t.name, montar_no(t, static_cast<std::size_t>(r), columns, coldefs));
    }
    tabela.list->push_back(std::move(row));
  }
  return tabela;
}

// ---- streaming por row group ----

struct ParquetEstado {
  LeitorParquet leitor;
};

ParquetFluxo parquet_abrir_fluxo(const std::string& path) {
  ParquetFluxo fx;
  fx.caminho = path;
  auto estado =
      std::shared_ptr<ParquetEstado>(new ParquetEstado(), [](ParquetEstado* p) { delete p; });
  estado->leitor = abrir_parquet(path);
  for (const RField& t : estado->leitor.top) fx.colunas.push_back(t.name);
  fx.grupos = static_cast<std::int64_t>(estado->leitor.row_groups.size());
  fx.linhas = estado->leitor.num_rows;
  for (std::int64_t r : estado->leitor.rg_num_rows) fx.linhas_por_grupo.push_back(r);
  fx.estado = std::move(estado);
  return fx;
}

Value parquet_ler_grupo_fluxo(ParquetFluxo& fx, std::int64_t grupo) {
  if (!fx.estado) die("fluxo parquet fechado");
  LeitorParquet& lp = fx.estado->leitor;
  if (grupo < 0 || grupo >= static_cast<std::int64_t>(lp.row_groups.size())) {
    die("row group " + std::to_string(grupo) + " fora do arquivo '" + lp.path + "'");
  }
  const std::size_t g = static_cast<std::size_t>(grupo);
  const std::size_t ncols = lp.cols_desc.size();
  std::vector<std::vector<Value>> columns(ncols);
  std::vector<std::vector<int>> coldefs(ncols);
  for (std::size_t ci = 0; ci < ncols; ++ci) {
    decode_chunk(lp.file, lp.row_groups[g][ci], lp.cols_desc[ci], lp.rg_num_rows[g], columns[ci],
                 &coldefs[ci]);
  }
  Value tabela = Value::tabela();
  for (std::int64_t r = 0; r < lp.rg_num_rows[g]; ++r) {
    Value row = Value::mapa();
    for (const RField& t : lp.top) {
      row.map->set(t.name, montar_no(t, static_cast<std::size_t>(r), columns, coldefs));
    }
    tabela.list->push_back(std::move(row));
  }
  return tabela;
}

}  // namespace tilt::rt
