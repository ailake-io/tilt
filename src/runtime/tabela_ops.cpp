#include "runtime/tabela_ops.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "runtime/fuso.hpp"
#include "runtime/json.hpp"
#include "runtime/sorteio.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void erro(const std::string& m) { throw std::runtime_error(m); }

const ValueList& linhas_de(const Value& t, const char* metodo) {
  static const ValueList vazia;
  if (t.kind != ValueKind::Tabela && t.kind != ValueKind::Lista) {
    erro(std::string(metodo) + ": o receptor deve ser uma tabela (lista de mapas)");
  }
  return t.list ? *t.list : vazia;
}

const Value* celula(const Value& linha, const std::string& coluna) {
  return linha.kind == ValueKind::Mapa && linha.map ? linha.map->find(coluna) : nullptr;
}

// Copia rasa dos itens do mapa (as tabelas de entrada nao podem ser alteradas).
Value nova_linha(const Value& linha) {
  Value nl = Value::mapa();
  if (linha.kind == ValueKind::Mapa && linha.map) nl.map->items = linha.map->items;
  return nl;
}

// Colunas na ordem da primeira aparicao.
std::vector<std::string> colunas_de(const ValueList& linhas) {
  std::vector<std::string> cols;
  std::unordered_set<std::string> vistas;
  for (const Value& l : linhas) {
    if (l.kind != ValueKind::Mapa || !l.map) continue;
    for (const auto& kv : l.map->items) {
      if (vistas.insert(kv.first).second) cols.push_back(kv.first);
    }
  }
  return cols;
}

void exige_coluna(const std::vector<std::string>& cols, const std::string& nome,
                  const char* metodo) {
  if (std::find(cols.begin(), cols.end(), nome) != cols.end()) return;
  std::string lista;
  for (const std::string& c : cols) lista += (lista.empty() ? "" : ", ") + c;
  erro(std::string(metodo) + ": a coluna '" + nome + "' nao existe (colunas: " + lista + ")");
}

std::string aparar(const std::string& s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) --b;
  return s.substr(a, b - a);
}

// Chave de comparacao para agrupar/juntar: numeros iguais casam (1 == 1.0).
std::string chave_de(const Value& v) {
  switch (v.kind) {
    case ValueKind::Nulo:
      return "\x01n";
    case ValueKind::Logico:
      return v.b ? "b1" : "b0";
    case ValueKind::Inteiro:
      return "n" + std::to_string(v.i);
    case ValueKind::Decimal: {
      if (v.d == std::floor(v.d) && std::fabs(v.d) < 9e15) {
        return "n" + std::to_string(static_cast<std::int64_t>(v.d));
      }
      return "d" + json_dump_compacto(v);
    }
    case ValueKind::Texto:
      return "s" + v.s;
    default:
      return "j" + json_dump_compacto(v);
  }
}

// ---- conversao de tipos -------------------------------------------------------

Value para_inteiro(const Value& v) {
  if (v.kind == ValueKind::Inteiro) return v;
  if (v.kind == ValueKind::Logico) return Value::inteiro(v.b ? 1 : 0);
  if (v.kind == ValueKind::Decimal) {
    if (v.d == std::floor(v.d) && std::fabs(v.d) < 9e18) {
      return Value::inteiro(static_cast<std::int64_t>(v.d));
    }
    return Value::nulo();
  }
  if (v.kind != ValueKind::Texto) return Value::nulo();
  const std::string s = aparar(v.s);
  if (s.empty()) return Value::nulo();
  char* fim = nullptr;
  const long long i = std::strtoll(s.c_str(), &fim, 10);
  if (fim != nullptr && *fim == '\0') return Value::inteiro(i);
  const Value d = Value::decimal(std::strtod(s.c_str(), &fim));
  if (fim != nullptr && *fim == '\0') return para_inteiro(d);
  return Value::nulo();
}

Value para_decimal(const Value& v) {
  if (v.kind == ValueKind::Decimal) return v;
  if (v.kind == ValueKind::Inteiro) return Value::decimal(static_cast<double>(v.i));
  if (v.kind == ValueKind::Logico) return Value::decimal(v.b ? 1.0 : 0.0);
  if (v.kind != ValueKind::Texto) return Value::nulo();
  std::string s = aparar(v.s);
  if (s.empty()) return Value::nulo();
  // "1,5" (virgula decimal, sem ponto) vale 1.5.
  if (s.find('.') == std::string::npos && std::count(s.begin(), s.end(), ',') == 1) {
    std::replace(s.begin(), s.end(), ',', '.');
  }
  char* fim = nullptr;
  const double d = std::strtod(s.c_str(), &fim);
  if (fim != nullptr && *fim == '\0' && std::isfinite(d)) return Value::decimal(d);
  return Value::nulo();
}

Value para_logico(const Value& v) {
  if (v.kind == ValueKind::Logico) return v;
  if (v.kind == ValueKind::Inteiro)
    return v.i == 0 || v.i == 1 ? Value::logico(v.i == 1) : Value::nulo();
  if (v.kind != ValueKind::Texto) return Value::nulo();
  std::string s = aparar(v.s);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* sim : {"verdadeiro", "true", "sim", "s", "yes", "y", "1"}) {
    if (s == sim) return Value::logico(true);
  }
  for (const char* nao : {"falso", "false", "nao", "não", "n", "no", "0"}) {
    if (s == nao) return Value::logico(false);
  }
  return Value::nulo();
}

Value para_texto(const Value& v) {
  if (v.kind == ValueKind::Nulo || v.kind == ValueKind::Texto) return v;
  if (v.kind == ValueKind::Lista || v.kind == ValueKind::Mapa || v.kind == ValueKind::Tabela) {
    return Value::texto(json_dump_compacto(v));
  }
  return Value::texto(to_display(v));
}

Value para_data(const Value& v) {
  if (v.kind != ValueKind::Texto) return Value::nulo();
  const std::string d = normalizar_data(v.s);
  return d.empty() ? Value::nulo() : Value::texto(d);
}

using Conversor = Value (*)(const Value&);

Conversor conversor_de(const std::string& tipo) {
  if (tipo == "inteiro" || tipo == "integer" || tipo == "int") return para_inteiro;
  if (tipo == "decimal" || tipo == "float") return para_decimal;
  if (tipo == "logico" || tipo == "boolean" || tipo == "bool") return para_logico;
  if (tipo == "texto" || tipo == "text" || tipo == "string") return para_texto;
  if (tipo == "data" || tipo == "date") return para_data;
  if (tipo == "inteiro") return para_inteiro;
  if (tipo == "decimal") return para_decimal;
  if (tipo == "logico") return para_logico;
  if (tipo == "texto") return para_texto;
  if (tipo == "data") return para_data;
  erro("converter: tipo desconhecido '" + tipo + "' (use inteiro, decimal, texto, logico ou data)");
}

// ---- ordenacao ------------------------------------------------------------------

// Ordena `ordem` (indices) por uma coluna, de forma estavel.
void ordenar_indices(const ValueList& linhas, const std::string& coluna, bool desc,
                     std::vector<std::uint32_t>& ordem) {
  const std::size_t n = linhas.size();
  std::vector<const Value*> chaves(n);
  bool todos_num = true;
  bool todos_txt = true;
  for (std::size_t i = 0; i < n; ++i) {
    const Value* k = celula(linhas[i], coluna);
    chaves[i] = k;
    if (!(k && k->is_number())) todos_num = false;
    if (!(k && k->kind == ValueKind::Texto)) todos_txt = false;
  }
  if (todos_num) {
    std::vector<double> num(n);
    for (std::size_t i = 0; i < n; ++i) num[i] = chaves[i]->as_number();
    std::stable_sort(ordem.begin(), ordem.end(), [&](std::uint32_t x, std::uint32_t y) {
      return desc ? num[y] < num[x] : num[x] < num[y];
    });
  } else if (todos_txt) {
    std::stable_sort(ordem.begin(), ordem.end(), [&](std::uint32_t x, std::uint32_t y) {
      return desc ? chaves[y]->s < chaves[x]->s : chaves[x]->s < chaves[y]->s;
    });
  } else {  // coluna mista/ausente: numero contra numero, senao texto de exibicao
    std::vector<std::string> texto(n);
    for (std::size_t i = 0; i < n; ++i) texto[i] = chaves[i] ? to_display(*chaves[i]) : "";
    const auto menor = [&](std::uint32_t x, std::uint32_t y) {
      if (chaves[x] && chaves[y] && chaves[x]->is_number() && chaves[y]->is_number()) {
        return chaves[x]->as_number() < chaves[y]->as_number();
      }
      return texto[x] < texto[y];
    };
    std::stable_sort(ordem.begin(), ordem.end(), [&](std::uint32_t x, std::uint32_t y) {
      return desc ? menor(y, x) : menor(x, y);
    });
  }
}

bool bissexto(int a) { return (a % 4 == 0 && a % 100 != 0) || a % 400 == 0; }

int dias_no_mes(int a, int m) {
  static const int dias[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && bissexto(a) ? 29 : dias[m - 1];
}

// Le `n` digitos a partir de `s[p]`; -1 se faltar algum.
int digitos(const std::string& s, std::size_t p, std::size_t n) {
  if (p + n > s.size()) return -1;
  int v = 0;
  for (std::size_t k = 0; k < n; ++k) {
    const unsigned char c = static_cast<unsigned char>(s[p + k]);
    if (std::isdigit(c) == 0) return -1;
    v = v * 10 + (c - '0');
  }
  return v;
}

}  // namespace

std::string normalizar_data(const std::string& texto) {
  const std::string s = aparar(texto);
  int a = -1;
  int m = -1;
  int d = -1;
  std::size_t p = 0;
  const auto sep = [&](std::size_t i) { return i < s.size() && (s[i] == '-' || s[i] == '/'); };
  if (digitos(s, 0, 4) >= 0 && sep(4)) {  // AAAA-MM-DD ou AAAA/MM/DD
    a = digitos(s, 0, 4);
    m = digitos(s, 5, 2);
    if (!sep(7)) return "";
    d = digitos(s, 8, 2);
    p = 10;
  } else if (digitos(s, 0, 2) >= 0 && sep(2)) {  // DD/MM/AAAA ou DD-MM-AAAA
    d = digitos(s, 0, 2);
    m = digitos(s, 3, 2);
    if (!sep(5)) return "";
    a = digitos(s, 6, 4);
    p = 10;
  } else {
    return "";
  }
  if (a < 0 || m < 1 || m > 12 || d < 1 || d > dias_no_mes(a, m)) return "";
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", a, m, d);
  std::string out = buf;
  if (p == s.size()) return out;
  if (s[p] != 'T' && s[p] != ' ') return "";
  const int hh = digitos(s, p + 1, 2);
  const int mm = s.size() > p + 3 && s[p + 3] == ':' ? digitos(s, p + 4, 2) : -1;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return "";
  int ss = 0;
  std::size_t fim = p + 6;
  if (s.size() > p + 6 && s[p + 6] == ':') {
    ss = digitos(s, p + 7, 2);
    if (ss < 0 || ss > 59) return "";
    fim = p + 9;
  }
  if (fim != s.size()) return "";
  std::snprintf(buf, sizeof buf, "T%02d:%02d:%02d", hh, mm, ss);
  return out + buf;
}

bool celula_nula(const Value* v) {
  if (v == nullptr || v->kind == ValueKind::Nulo) return true;
  if (v->kind != ValueKind::Texto) return false;
  return std::all_of(v->s.begin(), v->s.end(),
                     [](unsigned char c) { return std::isspace(c) != 0; });
}

Value tabela_remover_nulos(const Value& t, const std::vector<std::string>& colunas) {
  const ValueList& linhas = linhas_de(t, "remover_nulos");
  if (!colunas.empty()) exige_coluna(colunas_de(linhas), colunas[0], "remover_nulos");
  ValueList out;
  for (const Value& l : linhas) {
    bool remove = false;
    if (colunas.empty()) {
      if (l.kind == ValueKind::Mapa && l.map) {
        for (const auto& kv : l.map->items) remove = remove || celula_nula(&kv.second);
      }
    } else {
      for (const std::string& c : colunas) remove = remove || celula_nula(celula(l, c));
    }
    if (!remove) out.push_back(l);
  }
  return Value::tabela(std::move(out));
}

Value tabela_preencher_nulos(const Value& t, const Value& preenchimento) {
  const ValueList& linhas = linhas_de(t, "preencher_nulos");
  ValueList out;
  out.reserve(linhas.size());
  if (preenchimento.kind == ValueKind::Mapa && preenchimento.map) {
    for (const Value& l : linhas) {
      Value nl = nova_linha(l);
      for (const auto& [col, valor] : preenchimento.map->items) {
        if (Value* c = nl.map->find(col)) {
          if (celula_nula(c)) *c = valor;
        } else {
          nl.map->items.emplace_back(col, valor);
        }
      }
      out.push_back(std::move(nl));
    }
  } else {
    for (const Value& l : linhas) {
      Value nl = nova_linha(l);
      for (auto& kv : nl.map->items) {
        if (celula_nula(&kv.second)) kv.second = preenchimento;
      }
      out.push_back(std::move(nl));
    }
  }
  return Value::tabela(std::move(out));
}

Value tabela_renomear(const Value& t, const Value& mapa) {
  const ValueList& linhas = linhas_de(t, "renomear");
  if (mapa.kind != ValueKind::Mapa || !mapa.map) {
    erro("renomear espera um mapa { antigo: \"novo\" }");
  }
  const std::vector<std::string> cols = colunas_de(linhas);
  for (const auto& [antigo, novo] : mapa.map->items) {
    if (novo.kind != ValueKind::Texto)
      erro("renomear: o novo nome de '" + antigo + "' deve ser texto");
    exige_coluna(cols, antigo, "renomear");
  }
  ValueList out;
  out.reserve(linhas.size());
  for (const Value& l : linhas) {
    Value nl = Value::mapa();
    if (l.kind == ValueKind::Mapa && l.map) {
      nl.map->items.reserve(l.map->items.size());
      for (const auto& kv : l.map->items) {
        const Value* novo = mapa.map->find(kv.first);
        nl.map->set(novo ? novo->s : kv.first, kv.second);
      }
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_remover_colunas(const Value& t, const std::vector<std::string>& nomes) {
  const ValueList& linhas = linhas_de(t, "remover_colunas");
  const std::vector<std::string> cols = colunas_de(linhas);
  for (const std::string& n : nomes) exige_coluna(cols, n, "remover_colunas");
  const std::unordered_set<std::string> fora(nomes.begin(), nomes.end());
  ValueList out;
  out.reserve(linhas.size());
  for (const Value& l : linhas) {
    Value nl = Value::mapa();
    if (l.kind == ValueKind::Mapa && l.map) {
      for (const auto& kv : l.map->items) {
        if (fora.count(kv.first) == 0) nl.map->items.push_back(kv);
      }
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_converter(const Value& t, const Value& tipos) {
  const ValueList& linhas = linhas_de(t, "converter");
  if (tipos.kind != ValueKind::Mapa || !tipos.map) {
    erro(
        "converter espera um mapa { coluna: \"inteiro\" | \"decimal\" | \"texto\" | \"logico\" | "
        "\"data\" }");
  }
  const std::vector<std::string> cols = colunas_de(linhas);
  std::vector<std::pair<std::string, Conversor>> plano;
  for (const auto& [col, tipo] : tipos.map->items) {
    if (tipo.kind != ValueKind::Texto) erro("converter: o tipo de '" + col + "' deve ser texto");
    exige_coluna(cols, col, "converter");
    plano.emplace_back(col, conversor_de(tipo.s));
  }
  ValueList out;
  out.reserve(linhas.size());
  for (const Value& l : linhas) {
    Value nl = nova_linha(l);
    for (const auto& [col, conv] : plano) {
      if (Value* c = nl.map->find(col)) *c = conv(*c);
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_deduplicar(const Value& t, const std::vector<std::string>& colunas) {
  const ValueList& linhas = linhas_de(t, "deduplicar");
  if (!colunas.empty()) exige_coluna(colunas_de(linhas), colunas[0], "deduplicar");
  std::unordered_set<std::string> vistos;
  vistos.reserve(linhas.size());
  ValueList out;
  for (const Value& l : linhas) {
    std::string chave;
    if (colunas.empty()) {
      chave = json_dump_compacto(l);
    } else {
      for (const std::string& c : colunas) {
        const Value* v = celula(l, c);
        chave += v ? chave_de(*v) : "\x01n";
        chave += '\x02';
      }
    }
    if (vistos.insert(std::move(chave)).second) out.push_back(l);
  }
  return Value::tabela(std::move(out));
}

Value tabela_juntar_pt(const Value& esq, const Value& dir, const Value& por,
                       const std::string& tipo);

Value tabela_juntar(const Value& esq, const Value& dir, const Value& por, const std::string& tipo) {
  return tabela_juntar_pt(esq, dir, por,
                          tipo == "inner"                       ? "interna"
                          : tipo == "left"                      ? "esquerda"
                          : tipo == "right"                     ? "direita"
                          : (tipo == "full" || tipo == "outer") ? "completa"
                                                                : tipo);
}

Value tabela_juntar_pt(const Value& esq, const Value& dir, const Value& por,
                       const std::string& tipo) {
  const ValueList& le = linhas_de(esq, "juntar");
  const ValueList& ld = linhas_de(dir, "juntar");
  if (tipo != "interna" && tipo != "esquerda" && tipo != "direita" && tipo != "completa") {
    erro("juntar: tipo desconhecido '" + tipo + "' (use interna, esquerda, direita ou completa)");
  }
  std::vector<std::pair<std::string, std::string>> chaves;  // (coluna esquerda, coluna direita)
  if (por.kind == ValueKind::Texto) {
    chaves.emplace_back(por.s, por.s);
  } else if (por.kind == ValueKind::Lista && por.list) {
    for (const Value& v : *por.list) {
      if (v.kind != ValueKind::Texto) erro("juntar: 'por' deve listar nomes de coluna (texto)");
      chaves.emplace_back(v.s, v.s);
    }
  } else if (por.kind == ValueKind::Mapa && por.map) {
    for (const auto& [e, d] : por.map->items) {
      if (d.kind != ValueKind::Texto) erro("juntar: em { esquerda: direita } os nomes sao texto");
      chaves.emplace_back(e, d.s);
    }
  }
  if (chaves.empty()) {
    erro(
        "juntar precisa de 'por:' (nome, lista ou { esquerda: direita }), ex.: a.juntar b, por: "
        "\"id\"");
  }
  const std::vector<std::string> cols_e = colunas_de(le);
  const std::vector<std::string> cols_d = colunas_de(ld);
  for (const auto& [e, d] : chaves) {
    if (!le.empty()) exige_coluna(cols_e, e, "juntar (tabela da esquerda)");
    if (!ld.empty()) exige_coluna(cols_d, d, "juntar (tabela da direita)");
  }
  std::unordered_set<std::string> chaves_d;
  for (const auto& kv : chaves) chaves_d.insert(kv.second);
  // Colunas da direita que entram no resultado (fora as chaves) e seu nome final.
  std::vector<std::pair<std::string, std::string>> extras;
  for (const std::string& c : cols_d) {
    if (chaves_d.count(c) != 0) continue;
    const bool colide = std::find(cols_e.begin(), cols_e.end(), c) != cols_e.end();
    extras.emplace_back(c, colide ? c + "_direita" : c);
  }

  const auto chave_da_linha = [&](const Value& l, bool lado_esq, bool& tem_nulo) {
    std::string k;
    for (const auto& [e, d] : chaves) {
      const Value* v = celula(l, lado_esq ? e : d);
      if (celula_nula(v)) tem_nulo = true;
      k += v ? chave_de(*v) : "\x01n";
      k += '\x02';
    }
    return k;
  };
  std::unordered_map<std::string, std::vector<std::uint32_t>> indice;  // chave -> linhas da direita
  indice.reserve(ld.size());
  for (std::size_t j = 0; j < ld.size(); ++j) {
    bool nulo = false;
    const std::string k = chave_da_linha(ld[j], false, nulo);
    if (!nulo) indice[k].push_back(static_cast<std::uint32_t>(j));
  }
  std::vector<char> direita_casou(ld.size(), 0);
  ValueList out;
  const auto acrescenta_direita = [&](Value& nl, const Value* d) {
    for (const auto& [origem, final] : extras) {
      const Value* v = d ? celula(*d, origem) : nullptr;
      nl.map->set(final, v ? *v : Value::nulo());
    }
  };
  for (const Value& l : le) {
    bool nulo = false;
    const std::string k = chave_da_linha(l, true, nulo);
    const auto it = nulo ? indice.end() : indice.find(k);
    if (it != indice.end()) {
      for (const std::uint32_t j : it->second) {
        direita_casou[j] = 1;
        Value nl = nova_linha(l);
        acrescenta_direita(nl, &ld[j]);
        out.push_back(std::move(nl));
      }
    } else if (tipo == "esquerda" || tipo == "completa") {
      Value nl = nova_linha(l);
      acrescenta_direita(nl, nullptr);
      out.push_back(std::move(nl));
    }
  }
  if (tipo == "direita" || tipo == "completa") {
    for (std::size_t j = 0; j < ld.size(); ++j) {
      if (direita_casou[j] != 0) continue;
      Value nl = Value::mapa();
      for (const std::string& c : cols_e) nl.map->set(c, Value::nulo());
      for (const auto& [e, d] : chaves) {  // a chave vem da direita
        const Value* v = celula(ld[j], d);
        nl.map->set(e, v ? *v : Value::nulo());
      }
      acrescenta_direita(nl, &ld[j]);
      out.push_back(std::move(nl));
    }
  }
  return Value::tabela(std::move(out));
}

Value tabela_empilhar(const std::vector<Value>& tabelas) {
  std::vector<const ValueList*> partes;
  for (const Value& t : tabelas) partes.push_back(&linhas_de(t, "empilhar"));
  std::vector<std::string> cols;
  std::unordered_set<std::string> vistas;
  for (const ValueList* p : partes) {
    for (const std::string& c : colunas_de(*p)) {
      if (vistas.insert(c).second) cols.push_back(c);
    }
  }
  ValueList out;
  for (const ValueList* p : partes) {
    for (const Value& l : *p) {
      Value nl = Value::mapa();
      nl.map->items.reserve(cols.size());
      for (const std::string& c : cols) {
        const Value* v = celula(l, c);
        nl.map->items.emplace_back(c, v ? *v : Value::nulo());
      }
      out.push_back(std::move(nl));
    }
  }
  return Value::tabela(std::move(out));
}

Value tabela_descrever(const Value& t) {
  const ValueList& linhas = linhas_de(t, "descrever");
  const std::vector<std::string> cols = colunas_de(linhas);
  ValueList out;
  for (const std::string& c : cols) {
    std::int64_t nulos = 0;
    std::unordered_set<std::string> distintos;
    bool todos_num = true;
    bool algum = false;
    bool tem_int = false;
    bool tem_dec = false;
    bool tem_txt = false;
    bool tem_log = false;
    bool tem_outro = false;
    double soma = 0;
    std::int64_t n_num = 0;
    const Value* menor = nullptr;
    const Value* maior = nullptr;
    const auto antes = [](const Value& a, const Value& b) {
      if (a.is_number() && b.is_number()) return a.as_number() < b.as_number();
      return to_display(a) < to_display(b);
    };
    for (const Value& l : linhas) {
      const Value* v = celula(l, c);
      if (celula_nula(v)) {
        ++nulos;
        continue;
      }
      algum = true;
      distintos.insert(chave_de(*v));
      switch (v->kind) {
        case ValueKind::Inteiro:
          tem_int = true;
          break;
        case ValueKind::Decimal:
          tem_dec = true;
          break;
        case ValueKind::Texto:
          tem_txt = true;
          break;
        case ValueKind::Logico:
          tem_log = true;
          break;
        default:
          tem_outro = true;
          break;
      }
      if (v->is_number()) {
        soma += v->as_number();
        ++n_num;
      } else {
        todos_num = false;
      }
      if (v->kind != ValueKind::Lista && v->kind != ValueKind::Mapa &&
          v->kind != ValueKind::Tabela) {
        if (menor == nullptr || antes(*v, *menor)) menor = v;
        if (maior == nullptr || antes(*maior, *v)) maior = v;
      }
    }
    const int tipos = static_cast<int>(tem_int || tem_dec) + static_cast<int>(tem_txt) +
                      static_cast<int>(tem_log) + static_cast<int>(tem_outro);
    std::string tipo = "vazio";
    if (algum) {
      if (tipos > 1) {
        tipo = "misto";
      } else if (tem_dec) {
        tipo = "decimal";
      } else if (tem_int) {
        tipo = "inteiro";
      } else if (tem_txt) {
        tipo = "texto";
      } else if (tem_log) {
        tipo = "logico";
      } else {
        tipo = "estrutura";
      }
    }
    Value r = Value::mapa();
    r.map->set("coluna", Value::texto(c));
    r.map->set("tipo", Value::texto(tipo));
    r.map->set("total", Value::inteiro(static_cast<std::int64_t>(linhas.size())));
    r.map->set("nulos", Value::inteiro(nulos));
    r.map->set("distintos", Value::inteiro(static_cast<std::int64_t>(distintos.size())));
    r.map->set("minimo", menor ? *menor : Value::nulo());
    r.map->set("maximo", maior ? *maior : Value::nulo());
    r.map->set("media", algum && todos_num && n_num > 0
                            ? Value::decimal(soma / static_cast<double>(n_num))
                            : Value::nulo());
    out.push_back(std::move(r));
  }
  return Value::tabela(std::move(out));
}

Value tabela_amostra(const Value& t, double n, std::uint64_t semente) {
  const ValueList& linhas = linhas_de(t, "amostra");
  const std::size_t total = linhas.size();
  if (!(n > 0)) erro("amostra espera um tamanho positivo (n linhas) ou uma fracao entre 0 e 1");
  std::size_t alvo = n < 1.0
                         ? static_cast<std::size_t>(std::llround(n * static_cast<double>(total)))
                         : static_cast<std::size_t>(n);
  alvo = std::min(alvo, total);
  std::vector<std::uint32_t> idx(total);
  std::iota(idx.begin(), idx.end(), std::uint32_t{0});
  std::mt19937_64 rng(semente);
  for (std::size_t i = 0; i < alvo; ++i) {  // Fisher-Yates parcial
    const std::size_t j = i + static_cast<std::size_t>(sortear_indice(rng, total - i));
    std::swap(idx[i], idx[j]);
  }
  idx.resize(alvo);
  std::sort(idx.begin(), idx.end());
  ValueList out;
  out.reserve(alvo);
  for (const std::uint32_t i : idx) out.push_back(linhas[i]);
  return Value::tabela(std::move(out));
}

Value tabela_contar_valores(const Value& t, const std::string& coluna) {
  const ValueList& linhas = linhas_de(t, "contar_valores");
  exige_coluna(colunas_de(linhas), coluna, "contar_valores");
  std::unordered_map<std::string, std::size_t> pos;
  std::vector<std::pair<Value, std::int64_t>> valores;
  for (const Value& l : linhas) {
    const Value* v = celula(l, coluna);
    const Value chave = v ? *v : Value::nulo();
    const auto [it, novo] = pos.emplace(chave_de(chave), valores.size());
    if (novo) valores.emplace_back(chave, 0);
    ++valores[it->second].second;
  }
  std::vector<std::uint32_t> ordem(valores.size());
  std::iota(ordem.begin(), ordem.end(), std::uint32_t{0});
  std::stable_sort(ordem.begin(), ordem.end(), [&](std::uint32_t a, std::uint32_t b) {
    return valores[a].second > valores[b].second;
  });
  ValueList out;
  for (const std::uint32_t i : ordem) {
    Value r = Value::mapa();
    r.map->set("valor", valores[i].first);
    r.map->set("contagem", Value::inteiro(valores[i].second));
    out.push_back(std::move(r));
  }
  return Value::tabela(std::move(out));
}

Value tabela_limpar_texto(const Value& t, const std::vector<std::string>& colunas,
                          const std::string& caixa) {
  const ValueList& linhas = linhas_de(t, "limpar_texto");
  if (!caixa.empty() && caixa != "minusculas" && caixa != "maiusculas" && caixa != "lower" &&
      caixa != "upper") {
    erro("limpar_texto: caixa deve ser \"minusculas\" ou \"maiusculas\"");
  }
  const bool minusc = caixa == "minusculas" || caixa == "lower";
  const bool maiusc = caixa == "maiusculas" || caixa == "upper";
  if (!colunas.empty()) exige_coluna(colunas_de(linhas), colunas[0], "limpar_texto");
  const std::unordered_set<std::string> alvo(colunas.begin(), colunas.end());
  const auto limpa = [&](const std::string& s) {
    std::string out;
    bool espaco = false;
    for (const char ch : s) {
      const auto c = static_cast<unsigned char>(ch);
      if (std::isspace(c) != 0) {
        espaco = !out.empty();
        continue;
      }
      if (espaco) out += ' ';
      espaco = false;
      // so ASCII: bytes UTF-8 multibyte passam intactos
      out += minusc   ? static_cast<char>(std::tolower(c))
             : maiusc ? static_cast<char>(std::toupper(c))
                      : ch;
    }
    return out;
  };
  ValueList out;
  out.reserve(linhas.size());
  for (const Value& l : linhas) {
    Value nl = nova_linha(l);
    for (auto& kv : nl.map->items) {
      if (kv.second.kind != ValueKind::Texto) continue;
      if (!alvo.empty() && alvo.count(kv.first) == 0) continue;
      kv.second = Value::texto(limpa(kv.second.s));
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_ordenar(const Value& t, const std::vector<std::string>& colunas, bool decrescente) {
  const ValueList& linhas = linhas_de(t, "ordenar_por");
  if (colunas.empty()) erro("ordenar_por espera uma ou mais colunas");
  std::vector<std::uint32_t> ordem(linhas.size());
  std::iota(ordem.begin(), ordem.end(), std::uint32_t{0});
  // Varias colunas: ordena estavelmente da ultima para a primeira.
  for (std::size_t k = colunas.size(); k-- > 0;) {
    ordenar_indices(linhas, colunas[k], decrescente, ordem);
  }
  ValueList out;
  out.reserve(linhas.size());
  for (const std::uint32_t i : ordem) out.push_back(linhas[i]);
  return Value::tabela(std::move(out));
}

namespace {

// Acumulador de uma celula do pivo.
struct Acum {
  std::int64_t n = 0;  // valores nao nulos
  double soma = 0;
  bool todos_int = true;
  const Value* menor = nullptr;
  const Value* maior = nullptr;
  const Value* primeiro = nullptr;
};

bool menor_que(const Value& a, const Value& b) {
  if (a.is_number() && b.is_number()) return a.as_number() < b.as_number();
  return to_display(a) < to_display(b);
}

}  // namespace

Value tabela_pivotar(const Value& t, const std::vector<std::string>& indice,
                     const std::string& coluna, const std::string& valor,
                     const std::string& agregacao) {
  const ValueList& linhas = linhas_de(t, "pivotar");
  if (indice.empty() || coluna.empty() || valor.empty()) {
    erro(
        "pivotar espera indice:, colunas: e valores:, ex.: t.pivotar indice: \"regiao\", colunas: "
        "\"produto\", valores: \"valor\", agregacao: \"soma\"");
  }
  if (agregacao != "soma" && agregacao != "media" && agregacao != "contar" && agregacao != "min" &&
      agregacao != "max" && agregacao != "primeiro") {
    erro("pivotar: agregacao desconhecida '" + agregacao +
         "' (use soma, media, contar, min, max ou primeiro)");
  }
  const std::vector<std::string> cols = colunas_de(linhas);
  for (const std::string& c : indice) exige_coluna(cols, c, "pivotar");
  exige_coluna(cols, coluna, "pivotar");
  exige_coluna(cols, valor, "pivotar");

  std::unordered_map<std::string, std::size_t> pos_linha;
  std::unordered_map<std::string, std::size_t> pos_coluna;
  std::vector<std::vector<Value>> chaves_indice;  // por linha do resultado
  std::vector<std::string> nomes_colunas;
  std::vector<std::vector<Acum>> celulas;  // [linha][coluna]
  for (const Value& l : linhas) {
    std::string k;
    for (const std::string& c : indice) {
      const Value* v = celula(l, c);
      k += v ? chave_de(*v) : "\x01n";
      k += '\x02';
    }
    auto [it, novo] = pos_linha.emplace(k, chaves_indice.size());
    if (novo) {
      std::vector<Value> ch;
      for (const std::string& c : indice) {
        const Value* v = celula(l, c);
        ch.push_back(v ? *v : Value::nulo());
      }
      chaves_indice.push_back(std::move(ch));
      celulas.emplace_back();
    }
    const Value* cv = celula(l, coluna);
    const std::string nome_col = cv && cv->kind != ValueKind::Nulo ? to_display(*cv) : "nulo";
    auto [ic, nova_col] = pos_coluna.emplace(nome_col, nomes_colunas.size());
    if (nova_col) nomes_colunas.push_back(nome_col);
    auto& linha_acum = celulas[it->second];
    if (linha_acum.size() <= ic->second) linha_acum.resize(ic->second + 1);
    Acum& a = linha_acum[ic->second];
    const Value* v = celula(l, valor);
    if (celula_nula(v)) continue;
    ++a.n;
    if (a.primeiro == nullptr) a.primeiro = v;
    if (v->is_number()) {
      a.soma += v->as_number();
      a.todos_int = a.todos_int && v->kind == ValueKind::Inteiro;
    } else if (agregacao == "soma" || agregacao == "media") {
      erro("pivotar: '" + valor +
           "' tem valores nao numericos; use agregacao contar, primeiro, min ou max");
    }
    if (a.menor == nullptr || menor_que(*v, *a.menor)) a.menor = v;
    if (a.maior == nullptr || menor_que(*a.maior, *v)) a.maior = v;
  }
  ValueList out;
  out.reserve(chaves_indice.size());
  for (std::size_t r = 0; r < chaves_indice.size(); ++r) {
    Value nl = Value::mapa();
    for (std::size_t k = 0; k < indice.size(); ++k) nl.map->set(indice[k], chaves_indice[r][k]);
    for (std::size_t c = 0; c < nomes_colunas.size(); ++c) {
      const Acum* a = c < celulas[r].size() ? &celulas[r][c] : nullptr;
      Value v = agregacao == "contar" ? Value::inteiro(a ? a->n : 0) : Value::nulo();
      if (a != nullptr && a->n > 0) {
        if (agregacao == "soma") {
          v = a->todos_int ? Value::inteiro(static_cast<std::int64_t>(a->soma))
                           : Value::decimal(a->soma);
        } else if (agregacao == "media") {
          v = Value::decimal(a->soma / static_cast<double>(a->n));
        } else if (agregacao == "min") {
          v = *a->menor;
        } else if (agregacao == "max") {
          v = *a->maior;
        } else if (agregacao == "primeiro") {
          v = *a->primeiro;
        }
      }
      nl.map->set(nomes_colunas[c], std::move(v));
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_despivotar(const Value& t, const std::vector<std::string>& id,
                        const std::vector<std::string>& colunas, const std::string& nome,
                        const std::string& valor, bool manter_nulos) {
  const ValueList& linhas = linhas_de(t, "despivotar");
  const std::vector<std::string> cols = colunas_de(linhas);
  for (const std::string& c : id) exige_coluna(cols, c, "despivotar");
  for (const std::string& c : colunas) exige_coluna(cols, c, "despivotar");
  std::vector<std::string> alvo = colunas;
  if (alvo.empty()) {
    for (const std::string& c : cols) {
      if (std::find(id.begin(), id.end(), c) == id.end()) alvo.push_back(c);
    }
  }
  if (alvo.empty()) erro("despivotar: nao ha colunas para transformar em linhas");
  ValueList out;
  for (const Value& l : linhas) {
    for (const std::string& c : alvo) {
      const Value* v = celula(l, c);
      if (!manter_nulos && celula_nula(v)) continue;
      Value nl = Value::mapa();
      for (const std::string& i : id) {
        const Value* iv = celula(l, i);
        nl.map->set(i, iv ? *iv : Value::nulo());
      }
      nl.map->set(nome, Value::texto(c));
      nl.map->set(valor, v ? *v : Value::nulo());
      out.push_back(std::move(nl));
    }
  }
  return Value::tabela(std::move(out));
}

Value tabela_janela(const Value& t, const JanelaOpcoes& op) {
  const ValueList& linhas = linhas_de(t, "janela");
  const std::string& f = op.funcao;
  static const char* const kFuncoes[] = {
      "numero_linha",    "ranking",    "ranking_denso", "soma_acumulada", "contagem_acumulada",
      "media_acumulada", "soma_movel", "media_movel",   "anterior",       "proximo",
      "diferenca",       "primeiro",   "ultimo"};
  bool conhecida = false;
  for (const char* k : kFuncoes) conhecida = conhecida || f == k;
  if (!conhecida) {
    std::string lista;
    for (const char* k : kFuncoes) lista += std::string(lista.empty() ? "" : ", ") + k;
    erro("janela: funcao desconhecida '" + f + "' (use " + lista + ")");
  }
  const bool sem_coluna = f == "numero_linha" || f == "ranking" || f == "ranking_denso";
  if (!sem_coluna && op.coluna.empty())
    erro("janela: a funcao '" + f + "' precisa da coluna de entrada");
  if ((f == "ranking" || f == "ranking_denso") && op.ordem.empty()) {
    erro("janela: '" + f + "' precisa de ordem: (a coluna que define o ranking)");
  }
  if ((f == "soma_movel" || f == "media_movel") && op.tamanho == 0)
    erro("janela: tamanho deve ser >= 1");
  const std::vector<std::string> cols = colunas_de(linhas);
  for (const std::string& c : op.por) exige_coluna(cols, c, "janela");
  for (const std::string& c : op.ordem) exige_coluna(cols, c, "janela");
  if (!sem_coluna) exige_coluna(cols, op.coluna, "janela");

  const std::size_t n = linhas.size();
  // Ordena tudo por `ordem` uma vez; as particoes herdam essa ordem.
  std::vector<std::uint32_t> global(n);
  std::iota(global.begin(), global.end(), std::uint32_t{0});
  for (std::size_t k = op.ordem.size(); k-- > 0;) {
    ordenar_indices(linhas, op.ordem[k], op.decrescente, global);
  }
  std::unordered_map<std::string, std::size_t> pos;
  std::vector<std::vector<std::uint32_t>> particoes;
  for (const std::uint32_t i : global) {
    std::string k;
    for (const std::string& c : op.por) {
      const Value* v = celula(linhas[i], c);
      k += v ? chave_de(*v) : "\x01n";
      k += '\x02';
    }
    auto [it, novo] = pos.emplace(k, particoes.size());
    if (novo) particoes.emplace_back();
    particoes[it->second].push_back(i);
  }
  const auto valor_de = [&](std::uint32_t i) -> const Value* {
    return celula(linhas[i], op.coluna);
  };
  const auto ordem_igual = [&](std::uint32_t a, std::uint32_t b) {
    for (const std::string& c : op.ordem) {
      const Value* va = celula(linhas[a], c);
      const Value* vb = celula(linhas[b], c);
      if ((va ? chave_de(*va) : "") != (vb ? chave_de(*vb) : "")) return false;
    }
    return true;
  };
  std::vector<Value> resultado(n);
  for (const std::vector<std::uint32_t>& p : particoes) {
    const std::size_t m = p.size();
    double soma = 0;
    std::int64_t cont = 0;
    bool todos_int = true;
    std::int64_t rank = 0;
    std::int64_t denso = 0;
    for (std::size_t k = 0; k < m; ++k) {
      const std::uint32_t i = p[k];
      const Value* v = sem_coluna ? nullptr : valor_de(i);
      Value r = Value::nulo();
      if (f == "numero_linha") {
        r = Value::inteiro(static_cast<std::int64_t>(k) + 1);
      } else if (f == "ranking" || f == "ranking_denso") {
        if (k == 0 || !ordem_igual(p[k - 1], i)) {
          rank = static_cast<std::int64_t>(k) + 1;
          ++denso;
        }
        r = Value::inteiro(f == "ranking" ? rank : denso);
      } else if (f == "soma_acumulada" || f == "contagem_acumulada" || f == "media_acumulada") {
        if (!celula_nula(v)) {
          ++cont;
          if (v->is_number()) {
            soma += v->as_number();
            todos_int = todos_int && v->kind == ValueKind::Inteiro;
          } else if (f != "contagem_acumulada") {
            erro("janela: '" + op.coluna + "' tem valores nao numericos");
          }
        }
        if (f == "contagem_acumulada") {
          r = Value::inteiro(cont);
        } else if (cont > 0) {
          r = f == "media_acumulada" ? Value::decimal(soma / static_cast<double>(cont))
              : todos_int            ? Value::inteiro(static_cast<std::int64_t>(soma))
                                     : Value::decimal(soma);
        }
      } else if (f == "soma_movel" || f == "media_movel") {
        double s = 0;
        std::int64_t c = 0;
        bool ints = true;
        const std::size_t ini = k + 1 >= op.tamanho ? k + 1 - op.tamanho : 0;
        for (std::size_t j = ini; j <= k; ++j) {
          const Value* w = valor_de(p[j]);
          if (celula_nula(w)) continue;
          if (!w->is_number()) erro("janela: '" + op.coluna + "' tem valores nao numericos");
          s += w->as_number();
          ints = ints && w->kind == ValueKind::Inteiro;
          ++c;
        }
        if (c > 0) {
          r = f == "media_movel" ? Value::decimal(s / static_cast<double>(c))
              : ints             ? Value::inteiro(static_cast<std::int64_t>(s))
                                 : Value::decimal(s);
        }
      } else if (f == "anterior" || f == "proximo") {
        const bool volta = f == "anterior";
        const bool existe = volta ? k >= op.deslocamento : k + op.deslocamento < m;
        r = existe ? *valor_de(p[volta ? k - op.deslocamento : k + op.deslocamento]) : op.padrao;
      } else if (f == "diferenca") {
        if (k > 0) {
          const Value* ant = valor_de(p[k - 1]);
          if (v && ant && v->is_number() && ant->is_number()) {
            const double d = v->as_number() - ant->as_number();
            r = v->kind == ValueKind::Inteiro && ant->kind == ValueKind::Inteiro
                    ? Value::inteiro(static_cast<std::int64_t>(d))
                    : Value::decimal(d);
          }
        }
      } else if (f == "primeiro") {
        r = *valor_de(p[0]);
      } else if (f == "ultimo") {
        r = *valor_de(p[m - 1]);
      }
      resultado[i] = std::move(r);
    }
  }
  ValueList out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    Value nl = nova_linha(linhas[i]);
    nl.map->set(op.nome, std::move(resultado[i]));
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_dividir_coluna(const Value& t, const std::string& coluna, const std::string& separador,
                            const std::vector<std::string>& nomes, bool remover) {
  const ValueList& linhas = linhas_de(t, "dividir_coluna");
  exige_coluna(colunas_de(linhas), coluna, "dividir_coluna");
  if (separador.empty()) erro("dividir_coluna: o separador nao pode ser vazio");
  const std::size_t max_partes = nomes.empty() ? 0 : nomes.size();
  const auto partes_de = [&](const std::string& txt) {
    std::vector<std::string> partes;
    std::size_t ini = 0;
    while (true) {
      if (max_partes != 0 && partes.size() + 1 == max_partes) {
        partes.push_back(txt.substr(ini));
        break;
      }
      const std::size_t f = txt.find(separador, ini);
      if (f == std::string::npos) {
        partes.push_back(txt.substr(ini));
        break;
      }
      partes.push_back(txt.substr(ini, f - ini));
      ini = f + separador.size();
    }
    return partes;
  };
  std::vector<std::vector<std::string>> por_linha(linhas.size());
  std::vector<char> tem(linhas.size(), 0);
  std::size_t n_novas = nomes.size();
  for (std::size_t i = 0; i < linhas.size(); ++i) {
    const Value* v = celula(linhas[i], coluna);
    if (celula_nula(v)) continue;
    tem[i] = 1;
    por_linha[i] = partes_de(v->kind == ValueKind::Texto ? v->s : to_display(*v));
    if (nomes.empty()) n_novas = std::max(n_novas, por_linha[i].size());
  }
  std::vector<std::string> novos = nomes;
  for (std::size_t k = novos.size(); k < n_novas; ++k)
    novos.push_back(coluna + "_" + std::to_string(k + 1));
  ValueList out;
  out.reserve(linhas.size());
  for (std::size_t i = 0; i < linhas.size(); ++i) {
    Value nl = Value::mapa();
    for (const auto& kv : linhas[i].map->items) {
      if (remover && kv.first == coluna) continue;
      nl.map->items.push_back(kv);
    }
    for (std::size_t k = 0; k < novos.size(); ++k) {
      nl.map->set(novos[k], tem[i] != 0 && k < por_linha[i].size() ? Value::texto(por_linha[i][k])
                                                                   : Value::nulo());
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

Value tabela_converter_fuso(const Value& t, const std::string& coluna, const std::string& origem,
                            const std::string& destino) {
  const ValueList& linhas = linhas_de(t, "converter_fuso");
  exige_coluna(colunas_de(linhas), coluna, "converter_fuso");
  ValueList out;
  out.reserve(linhas.size());
  for (const Value& l : linhas) {
    Value nl = nova_linha(l);
    if (Value* c = nl.map->find(coluna)) {
      const std::string r =
          c->kind == ValueKind::Texto ? converter_fuso(c->s, origem, destino) : "";
      *c = r.empty() ? Value::nulo() : Value::texto(r);
    }
    out.push_back(std::move(nl));
  }
  return Value::tabela(std::move(out));
}

}  // namespace tilt::rt
