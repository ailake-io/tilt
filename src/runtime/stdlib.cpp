#include "runtime/stdlib.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "runtime/compat.hpp"
#include "runtime/json.hpp"
#include "runtime/sha256.hpp"
#include "runtime/vectorstore.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void erro(const std::string& mensagem) { throw std::runtime_error(mensagem); }

// Acesso tipado aos argumentos, com mensagens que dizem qual funcao e qual
// argumento estava errado.
class Args {
 public:
  Args(const std::string& nome, const std::vector<Value>& v) : nome_(nome), v_(v) {}

  void aridade(std::size_t minimo, std::size_t maximo, const char* uso) const {
    if (v_.size() < minimo || v_.size() > maximo) {
      erro(nome_ + " espera " + uso + ", recebeu " + std::to_string(v_.size()) + " argumento(s)");
    }
  }
  std::size_t tamanho() const { return v_.size(); }
  const Value& operator[](std::size_t i) const { return v_[i]; }

  double num(std::size_t i) const {
    if (!v_[i].is_number()) tipo_errado(i, "numero");
    return v_[i].as_number();
  }
  const std::string& txt(std::size_t i) const {
    if (v_[i].kind != ValueKind::Texto) tipo_errado(i, "texto");
    return v_[i].s;
  }
  const ValueList& lista(std::size_t i) const {
    if (v_[i].kind != ValueKind::Lista || !v_[i].list) tipo_errado(i, "lista");
    return *v_[i].list;
  }
  const ValueMap& mapa(std::size_t i) const {
    if (v_[i].kind != ValueKind::Mapa || !v_[i].map) tipo_errado(i, "mapa");
    return *v_[i].map;
  }
  [[noreturn]] void tipo_errado(std::size_t i, const char* esperado) const {
    erro(nome_ + ": o argumento " + std::to_string(i + 1) + " deve ser " + esperado + ", mas e " +
         v_[i].type_name());
  }
  [[noreturn]] void falha(const std::string& mensagem) const { erro(nome_ + ": " + mensagem); }

 private:
  const std::string& nome_;
  const std::vector<Value>& v_;
};

using Handler = std::function<Value(const Args&)>;

// --- numeros ---------------------------------------------------------------

constexpr double kMaxInteiroExato = 9007199254740992.0;  // 2^53

Value numero_de(double d) {
  if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < kMaxInteiroExato) {
    return Value::inteiro(static_cast<std::int64_t>(d));
  }
  return Value::decimal(d);
}

Value para_inteiro(const Args& a, double d) {
  if (!std::isfinite(d) || std::fabs(d) >= 9.2e18) a.falha("valor fora do intervalo de inteiro");
  return Value::inteiro(static_cast<std::int64_t>(d));
}

std::string aparar(const std::string& s) {
  std::size_t ini = 0;
  std::size_t fim = s.size();
  while (ini < fim && std::isspace(static_cast<unsigned char>(s[ini]))) ++ini;
  while (fim > ini && std::isspace(static_cast<unsigned char>(s[fim - 1]))) --fim;
  return s.substr(ini, fim - ini);
}

// Le o texto inteiro como numero (aceita espacos nas pontas). false se sobrar
// lixo ou nao houver digito.
bool ler_inteiro(const std::string& bruto, std::int64_t& out) {
  const std::string s = aparar(bruto);
  if (s.empty()) return false;
  char* fim = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &fim, 10);
  if (errno != 0 || *fim != '\0') return false;
  out = v;
  return true;
}

bool ler_decimal(const std::string& bruto, double& out) {
  const std::string s = aparar(bruto);
  if (s.empty()) return false;
  char* fim = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &fim);
  if (errno != 0 || *fim != '\0') return false;
  out = v;
  return true;
}

// --- texto -----------------------------------------------------------------

std::string mapear_ascii(const std::string& s, int (*f)(int)) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(f(static_cast<unsigned char>(c)));
  return out;
}

// Inverte por pontos de codigo UTF-8 (sem quebrar caracteres multibyte).
std::string reverso_utf8(const std::string& s) {
  std::vector<std::string> pontos;
  for (std::size_t i = 0; i < s.size();) {
    std::size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xF0)
      n = 4;
    else if (c >= 0xE0)
      n = 3;
    else if (c >= 0xC0)
      n = 2;
    n = std::min(n, s.size() - i);
    pontos.push_back(s.substr(i, n));
    i += n;
  }
  std::string out;
  out.reserve(s.size());
  for (auto it = pontos.rbegin(); it != pontos.rend(); ++it) out += *it;
  return out;
}

std::regex compilar_regex(const Args& a, const std::string& padrao) {
  try {
    return std::regex(padrao, std::regex::ECMAScript);
  } catch (const std::regex_error& e) {
    a.falha("expressao regular invalida '" + padrao + "': " + e.what());
  }
}

// --- listas ----------------------------------------------------------------

int comparar(const Args& a, const Value& x, const Value& y) {
  if (x.is_number() && y.is_number()) {
    const double dx = x.as_number();
    const double dy = y.as_number();
    return dx < dy ? -1 : (dx > dy ? 1 : 0);
  }
  if (x.kind == ValueKind::Texto && y.kind == ValueKind::Texto) {
    const int c = x.s.compare(y.s);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  a.falha(std::string("nao sabe comparar ") + x.type_name() + " com " + y.type_name() +
          " (use so numeros ou so textos)");
}

// --- datas (UTC) ------------------------------------------------------------

// Dias desde 1970-01-01 para uma data civil (algoritmo de Howard Hinnant).
std::int64_t dias_desde_epoca(std::int64_t ano, unsigned mes, unsigned dia) {
  ano -= mes <= 2;
  const std::int64_t era = (ano >= 0 ? ano : ano - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(ano - era * 400);
  const unsigned doy = (153 * (mes > 2 ? mes - 3 : mes + 9) + 2) / 5 + dia - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "AAAA-MM-DD", "AAAA-MM-DDTHH:MM:SS[Z]" ou "AAAA-MM-DD HH:MM:SS".
bool ler_data_iso(const std::string& bruto, std::int64_t& epoca) {
  const std::string s = aparar(bruto);
  int ano = 0, mes = 0, dia = 0, hora = 0, minuto = 0, segundo = 0;
  int lidos =
      std::sscanf(s.c_str(), "%d-%d-%d%*1[T ]%d:%d:%d", &ano, &mes, &dia, &hora, &minuto, &segundo);
  if (lidos != 3 && lidos != 6) {
    if (lidos == 5)
      lidos = 6;  // "HH:MM" sem segundos
    else
      return false;
  }
  if (mes < 1 || mes > 12 || dia < 1 || dia > 31 || hora < 0 || hora > 23 || minuto < 0 ||
      minuto > 59 || segundo < 0 || segundo > 60) {
    return false;
  }
  epoca = dias_desde_epoca(ano, static_cast<unsigned>(mes), static_cast<unsigned>(dia)) * 86400 +
          hora * 3600 + minuto * 60 + segundo;
  return true;
}

std::string formatar_epoca(std::int64_t epoca, const char* formato) {
  const std::time_t t = static_cast<std::time_t>(epoca);
  const std::tm tm = tilt_gmtime(t);
  char buf[512];
  const std::size_t n = std::strftime(buf, sizeof(buf), formato, &tm);
  return std::string(buf, n);
}

// --- arquivos, base64 ---------------------------------------------------------

std::string ler_arquivo(const Args& a, const std::string& caminho) {
  std::ifstream in(caminho, std::ios::binary);
  if (!in) a.falha("nao foi possivel abrir '" + caminho + "' para leitura");
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_codificar(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const std::uint32_t n = (static_cast<std::uint8_t>(in[i]) << 16) |
                            (static_cast<std::uint8_t>(in[i + 1]) << 8) |
                            static_cast<std::uint8_t>(in[i + 2]);
    out += kBase64[(n >> 18) & 63];
    out += kBase64[(n >> 12) & 63];
    out += kBase64[(n >> 6) & 63];
    out += kBase64[n & 63];
  }
  if (i + 1 == in.size()) {
    const std::uint32_t n = static_cast<std::uint8_t>(in[i]) << 16;
    out += kBase64[(n >> 18) & 63];
    out += kBase64[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const std::uint32_t n =
        (static_cast<std::uint8_t>(in[i]) << 16) | (static_cast<std::uint8_t>(in[i + 1]) << 8);
    out += kBase64[(n >> 18) & 63];
    out += kBase64[(n >> 12) & 63];
    out += kBase64[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

bool base64_decodificar(const std::string& in, std::string& out) {
  std::uint32_t acc = 0;
  int bits = 0;
  std::size_t fim = in.size();
  while (fim > 0 && in[fim - 1] == '=') --fim;
  for (std::size_t i = 0; i < fim; ++i) {
    const char c = in[i];
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    const char* p = std::strchr(kBase64, c);
    if (c == '\0' || !p) return false;
    acc = (acc << 6) | static_cast<std::uint32_t>(p - kBase64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

// --- tabela de funcoes ----------------------------------------------------------

const std::unordered_map<std::string, Handler>& tabela() {
  static const std::unordered_map<std::string, Handler> t = [] {
    std::unordered_map<std::string, Handler> m;

    // ---- matematica ----
    m["raiz"] = [](const Args& a) {
      a.aridade(1, 1, "(numero)");
      const double x = a.num(0);
      if (x < 0) a.falha("raiz de numero negativo (" + to_display(a[0]) + ")");
      return numero_de(std::sqrt(x));
    };
    m["abs"] = [](const Args& a) {
      a.aridade(1, 1, "(numero)");
      if (a[0].kind == ValueKind::Inteiro) return Value::inteiro(a[0].i < 0 ? -a[0].i : a[0].i);
      return Value::decimal(std::fabs(a.num(0)));
    };
    m["exp"] = [](const Args& a) {
      a.aridade(1, 1, "(numero)");
      return Value::decimal(std::exp(a.num(0)));
    };
    m["logaritmo"] = [](const Args& a) {
      a.aridade(1, 2, "(numero [, base])");
      const double x = a.num(0);
      if (x <= 0) a.falha("logaritmo de numero nao positivo (" + to_display(a[0]) + ")");
      if (a.tamanho() == 1) return Value::decimal(std::log(x));
      const double base = a.num(1);
      if (base <= 0 || base == 1) a.falha("base invalida (" + to_display(a[1]) + ")");
      return Value::decimal(std::log(x) / std::log(base));
    };
    m["potencia"] = [](const Args& a) {
      a.aridade(2, 2, "(base, expoente)");
      const double r = std::pow(a.num(0), a.num(1));
      if (a[0].kind == ValueKind::Inteiro && a[1].kind == ValueKind::Inteiro && a[1].i >= 0) {
        return numero_de(r);
      }
      return Value::decimal(r);
    };
    m["piso"] = [](const Args& a) {
      a.aridade(1, 1, "(numero)");
      return para_inteiro(a, std::floor(a.num(0)));
    };
    m["teto"] = [](const Args& a) {
      a.aridade(1, 1, "(numero)");
      return para_inteiro(a, std::ceil(a.num(0)));
    };
    m["arredondar"] = [](const Args& a) {
      a.aridade(1, 2, "(numero [, casas])");
      if (a.tamanho() == 1) return para_inteiro(a, std::round(a.num(0)));
      const double fator = std::pow(10.0, a.num(1));
      return Value::decimal(std::round(a.num(0) * fator) / fator);
    };
    m["seno"] = [](const Args& a) {
      a.aridade(1, 1, "(radianos)");
      return Value::decimal(std::sin(a.num(0)));
    };
    m["cosseno"] = [](const Args& a) {
      a.aridade(1, 1, "(radianos)");
      return Value::decimal(std::cos(a.num(0)));
    };
    m["tangente"] = [](const Args& a) {
      a.aridade(1, 1, "(radianos)");
      return Value::decimal(std::tan(a.num(0)));
    };
    m["pi"] = [](const Args& a) {
      a.aridade(0, 0, "()");
      return Value::decimal(3.14159265358979323846);
    };

    // ---- conversao de tipos ----
    m["inteiro"] = [](const Args& a) {
      a.aridade(1, 1, "(valor)");
      const Value& v = a[0];
      if (v.kind == ValueKind::Inteiro) return v;
      if (v.kind == ValueKind::Logico) return Value::inteiro(v.b ? 1 : 0);
      if (v.kind == ValueKind::Decimal) return para_inteiro(a, std::trunc(v.d));
      if (v.kind == ValueKind::Texto) {
        std::int64_t i = 0;
        if (ler_inteiro(v.s, i)) return Value::inteiro(i);
        double d = 0;
        if (ler_decimal(v.s, d)) return para_inteiro(a, std::trunc(d));
        a.falha("nao consegue converter '" + v.s + "' em inteiro");
      }
      a.tipo_errado(0, "numero, texto ou logico");
    };
    m["decimal"] = [](const Args& a) {
      a.aridade(1, 1, "(valor)");
      const Value& v = a[0];
      if (v.kind == ValueKind::Decimal) return v;
      if (v.kind == ValueKind::Inteiro || v.kind == ValueKind::Logico) {
        return Value::decimal(v.as_number());
      }
      if (v.kind == ValueKind::Texto) {
        double d = 0;
        if (ler_decimal(v.s, d)) return Value::decimal(d);
        a.falha("nao consegue converter '" + v.s + "' em decimal");
      }
      a.tipo_errado(0, "numero, texto ou logico");
    };
    m["texto"] = [](const Args& a) {
      a.aridade(1, 1, "(valor)");
      return Value::texto(to_display(a[0]));
    };
    m["logico"] = [](const Args& a) {
      a.aridade(1, 1, "(valor)");
      return Value::logico(a[0].truthy());
    };
    m["tipo_de"] = [](const Args& a) {
      a.aridade(1, 1, "(valor)");
      return Value::texto(a[0].type_name());
    };

    // ---- texto ----
    m["maiusculas"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      return Value::texto(mapear_ascii(a.txt(0), std::toupper));
    };
    m["minusculas"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      return Value::texto(mapear_ascii(a.txt(0), std::tolower));
    };
    m["aparar"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      return Value::texto(aparar(a.txt(0)));
    };
    m["substituir"] = [](const Args& a) {
      a.aridade(3, 3, "(texto, de, para)");
      const std::string& de = a.txt(1);
      if (de.empty()) a.falha("o trecho a substituir nao pode ser vazio");
      const std::string& s = a.txt(0);
      const std::string& para = a.txt(2);
      std::string out;
      std::size_t pos = 0;
      for (std::size_t achou; (achou = s.find(de, pos)) != std::string::npos;
           pos = achou + de.size()) {
        out.append(s, pos, achou - pos);
        out += para;
      }
      out.append(s, pos, std::string::npos);
      return Value::texto(std::move(out));
    };
    m["comeca_com"] = [](const Args& a) {
      a.aridade(2, 2, "(texto, prefixo)");
      return Value::logico(a.txt(0).rfind(a.txt(1), 0) == 0);
    };
    m["termina_com"] = [](const Args& a) {
      a.aridade(2, 2, "(texto, sufixo)");
      const std::string& s = a.txt(0);
      const std::string& suf = a.txt(1);
      return Value::logico(s.size() >= suf.size() &&
                           s.compare(s.size() - suf.size(), suf.size(), suf) == 0);
    };
    m["juntar"] = [](const Args& a) {
      a.aridade(1, 2, "(lista [, separador])");
      const std::string sep = a.tamanho() > 1 ? a.txt(1) : "";
      std::string out;
      bool primeiro = true;
      for (const Value& v : a.lista(0)) {
        if (!primeiro) out += sep;
        primeiro = false;
        out += to_display(v);
      }
      return Value::texto(std::move(out));
    };
    m["regex_casa"] = [](const Args& a) {
      a.aridade(2, 2, "(texto, padrao)");
      return Value::logico(std::regex_search(a.txt(0), compilar_regex(a, a.txt(1))));
    };
    m["regex_extrair"] = [](const Args& a) {
      a.aridade(2, 2, "(texto, padrao)");
      const std::regex re = compilar_regex(a, a.txt(1));
      const std::string& s = a.txt(0);
      ValueList out;
      for (std::sregex_iterator it(s.begin(), s.end(), re), fim; it != fim; ++it) {
        out.push_back(Value::texto(it->size() > 1 ? (*it)[1].str() : (*it)[0].str()));
      }
      return Value::lista(std::move(out));
    };
    m["regex_substituir"] = [](const Args& a) {
      a.aridade(3, 3, "(texto, padrao, para)");
      return Value::texto(std::regex_replace(a.txt(0), compilar_regex(a, a.txt(1)), a.txt(2)));
    };

    // ---- listas e mapas ----
    m["ordenar"] = [](const Args& a) {
      a.aridade(1, 2, "(lista [, \"decrescente\"])");
      ValueList v = a.lista(0);
      bool desc = false;
      if (a.tamanho() > 1) {
        const std::string& modo = a.txt(1);
        if (modo == "decrescente" || modo == "desc")
          desc = true;
        else if (modo != "crescente" && modo != "asc") {
          a.falha("ordem '" + modo + "' invalida (use \"crescente\" ou \"decrescente\")");
        }
      }
      std::stable_sort(v.begin(), v.end(), [&](const Value& x, const Value& y) {
        const int c = comparar(a, x, y);
        return desc ? c > 0 : c < 0;
      });
      return Value::lista(std::move(v));
    };
    m["unicos"] = [](const Args& a) {
      a.aridade(1, 1, "(lista)");
      std::unordered_set<std::string> vistos;
      ValueList out;
      for (const Value& v : a.lista(0)) {
        if (vistos.insert(std::string(v.type_name()) + ":" + to_display(v)).second) {
          out.push_back(v);
        }
      }
      return Value::lista(std::move(out));
    };
    m["reverso"] = [](const Args& a) {
      a.aridade(1, 1, "(lista ou texto)");
      if (a[0].kind == ValueKind::Texto) return Value::texto(reverso_utf8(a[0].s));
      ValueList v = a.lista(0);
      std::reverse(v.begin(), v.end());
      return Value::lista(std::move(v));
    };
    m["zip"] = [](const Args& a) {
      a.aridade(2, 2, "(lista, lista)");
      const ValueList& x = a.lista(0);
      const ValueList& y = a.lista(1);
      ValueList out;
      for (std::size_t i = 0; i < std::min(x.size(), y.size()); ++i) {
        out.push_back(Value::lista({x[i], y[i]}));
      }
      return Value::lista(std::move(out));
    };
    m["enumerar"] = [](const Args& a) {
      a.aridade(1, 1, "(lista)");
      ValueList out;
      std::int64_t i = 0;
      for (const Value& v : a.lista(0)) {
        Value par = Value::mapa();
        par.map->set("indice", Value::inteiro(i++));
        par.map->set("valor", v);
        out.push_back(std::move(par));
      }
      return Value::lista(std::move(out));
    };
    m["chaves"] = [](const Args& a) {
      a.aridade(1, 1, "(mapa)");
      ValueList out;
      for (const auto& kv : a.mapa(0).items) out.push_back(Value::texto(kv.first));
      return Value::lista(std::move(out));
    };
    m["valores"] = [](const Args& a) {
      a.aridade(1, 1, "(mapa)");
      ValueList out;
      for (const auto& kv : a.mapa(0).items) out.push_back(kv.second);
      return Value::lista(std::move(out));
    };

    // ---- testes (`teste nome:` + `tilt testar`) ----
    m["afirmar"] = [](const Args& a) {
      a.aridade(1, 2, "(condicao [, mensagem])");
      if (!a[0].truthy()) {
        a.falha("afirmacao falhou" + (a.tamanho() > 1 ? ": " + to_display(a[1]) : std::string()));
      }
      return Value::nulo();
    };
    m["afirmar_igual"] = [](const Args& a) {
      a.aridade(2, 3, "(obtido, esperado [, mensagem])");
      if (!equals(a[0], a[1])) {
        a.falha("afirmacao falhou: esperado " + to_display(a[1]) + ", obtido " + to_display(a[0]) +
                (a.tamanho() > 2 ? " (" + to_display(a[2]) + ")" : std::string()));
      }
      return Value::nulo();
    };

    // ---- RAG ----
    m["reranquear"] = [](const Args& a) {
      a.aridade(2, 3, "(consulta, itens [, top_k])");
      const std::string& consulta = a.txt(0);
      const ValueList& itens = a.lista(1);
      std::size_t k = itens.size();
      if (a.tamanho() > 2) {
        const double kd = a.num(2);
        if (kd < 1) a.falha("top_k deve ser >= 1");
        k = std::min<std::size_t>(k, static_cast<std::size_t>(kd));
      }
      // Cada item: mapa com `texto` (ex.: resultado de `buscar`) ou o proprio texto.
      std::vector<std::string> textos;
      for (const Value& it : itens) {
        if (it.kind == ValueKind::Texto) {
          textos.push_back(it.s);
        } else if (it.kind == ValueKind::Mapa && it.map && it.map->find("texto") &&
                   it.map->find("texto")->kind == ValueKind::Texto) {
          textos.push_back(it.map->find("texto")->s);
        } else {
          a.falha("cada item deve ser um texto ou um mapa com o campo 'texto'");
        }
      }
      // A ordem de entrada e o ranking original (1o = melhor): vira score decrescente.
      std::vector<double> original(itens.size());
      for (std::size_t i = 0; i < itens.size(); ++i) {
        original[i] = static_cast<double>(itens.size() - i);
      }
      const std::vector<double> fundido = fusao_rrf(original, bm25_scores(consulta, textos));
      std::vector<std::size_t> ordem(itens.size());
      for (std::size_t i = 0; i < ordem.size(); ++i) ordem[i] = i;
      std::stable_sort(ordem.begin(), ordem.end(),
                       [&](std::size_t x, std::size_t y) { return fundido[x] > fundido[y]; });
      ValueList out;
      for (std::size_t r = 0; r < k; ++r) {
        const Value& it = itens[ordem[r]];
        Value item = Value::mapa();
        if (it.kind == ValueKind::Mapa) {
          for (const auto& kv : it.map->items) item.map->set(kv.first, kv.second);
          if (const Value* s = it.map->find("score")) item.map->set("score_original", *s);
        } else {
          item.map->set("texto", it);
        }
        item.map->set("score", Value::decimal(fundido[ordem[r]]));
        out.push_back(std::move(item));
      }
      return Value::lista(std::move(out));
    };

    // ---- data e hora (UTC) ----
    m["timestamp"] = [](const Args& a) {
      a.aridade(0, 0, "()");
      return Value::inteiro(static_cast<std::int64_t>(std::time(nullptr)));
    };
    m["agora"] = [](const Args& a) {
      a.aridade(0, 0, "()");
      return Value::texto(
          formatar_epoca(static_cast<std::int64_t>(std::time(nullptr)), "%Y-%m-%dT%H:%M:%SZ"));
    };
    m["formatar_data"] = [](const Args& a) {
      a.aridade(1, 2, "(data [, formato])");
      std::int64_t epoca = 0;
      if (a[0].kind == ValueKind::Inteiro) {
        epoca = a[0].i;
      } else if (a[0].kind == ValueKind::Texto) {
        if (!ler_data_iso(a[0].s, epoca)) {
          a.falha("data '" + a[0].s + "' invalida (use AAAA-MM-DD ou AAAA-MM-DDTHH:MM:SS)");
        }
      } else {
        a.tipo_errado(0, "texto ISO ou inteiro (segundos desde 1970)");
      }
      return Value::texto(
          formatar_epoca(epoca, a.tamanho() > 1 ? a.txt(1).c_str() : "%Y-%m-%dT%H:%M:%SZ"));
    };
    m["dormir"] = [](const Args& a) {
      a.aridade(1, 1, "(segundos)");
      const double s = a.num(0);
      if (s < 0) a.falha("duracao negativa");
      std::this_thread::sleep_for(std::chrono::duration<double>(s));
      return Value::nulo();
    };

    // ---- arquivos de texto ----
    m["ler_texto"] = [](const Args& a) {
      a.aridade(1, 1, "(caminho)");
      return Value::texto(ler_arquivo(a, a.txt(0)));
    };
    m["escrever_texto"] = [](const Args& a) {
      a.aridade(2, 2, "(caminho, texto)");
      std::ofstream out(a.txt(0), std::ios::binary | std::ios::trunc);
      out << a.txt(1);
      if (!out) a.falha("nao foi possivel gravar '" + a.txt(0) + "'");
      return Value::nulo();
    };
    m["anexar_texto"] = [](const Args& a) {
      a.aridade(2, 2, "(caminho, texto)");
      std::ofstream out(a.txt(0), std::ios::binary | std::ios::app);
      out << a.txt(1);
      if (!out) a.falha("nao foi possivel gravar '" + a.txt(0) + "'");
      return Value::nulo();
    };
    m["listar_arquivos"] = [](const Args& a) {
      a.aridade(1, 1, "(diretorio)");
      std::vector<std::string> nomes;
      if (!tilt_listdir(a.txt(0), nomes)) {
        a.falha("nao foi possivel listar '" + a.txt(0) + "'");
      }
      std::sort(nomes.begin(), nomes.end());
      ValueList out;
      for (auto& n : nomes) out.push_back(Value::texto(std::move(n)));
      return Value::lista(std::move(out));
    };
    m["remover_arquivo"] = [](const Args& a) {
      a.aridade(1, 1, "(caminho)");
      return Value::logico(std::remove(a.txt(0).c_str()) == 0);
    };

    // ---- hash, base64, JSON ----
    m["sha256"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      return Value::texto(sha256_hex(a.txt(0)));
    };
    m["base64_codificar"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      return Value::texto(base64_codificar(a.txt(0)));
    };
    m["base64_decodificar"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      std::string out;
      if (!base64_decodificar(a.txt(0), out)) a.falha("texto base64 invalido");
      return Value::texto(std::move(out));
    };
    m["json_texto"] = [](const Args& a) {
      a.aridade(1, 1, "(valor)");
      std::string texto = json_dump(a[0]);
      while (!texto.empty() && texto.back() == '\n') texto.pop_back();
      return Value::texto(std::move(texto));
    };
    m["json_ler"] = [](const Args& a) {
      a.aridade(1, 1, "(texto)");
      try {
        return json_parse(a.txt(0));
      } catch (const std::exception& e) {
        a.falha(e.what());  // json_parse ja diz "JSON invalido: ..."
      }
    };

    return m;
  }();
  return t;
}

}  // namespace

namespace {

bool inicio_de_caractere(unsigned char c) { return (c & 0xC0) != 0x80; }

// Janela fixa alinhada a caracteres UTF-8.
std::vector<std::string> fatiar_utf8(const std::string& s, std::size_t win, std::size_t overlap) {
  std::vector<std::string> out;
  if (win == 0) win = 1;
  if (overlap >= win) overlap = win - 1;
  std::size_t start = 0;
  while (start < s.size()) {
    std::size_t end = std::min(s.size(), start + win);
    if (end < s.size()) {
      std::size_t recuo = end;
      while (recuo > start && !inicio_de_caractere(static_cast<unsigned char>(s[recuo]))) --recuo;
      if (recuo > start) end = recuo;
    }
    out.push_back(s.substr(start, end - start));
    if (end >= s.size()) break;
    std::size_t prox = end > overlap ? end - overlap : end;
    if (prox <= start) prox = end;  // sempre avanca
    while (prox < end && !inicio_de_caractere(static_cast<unsigned char>(s[prox]))) ++prox;
    start = prox;
  }
  return out;
}

// Quebra em unidades (sentencas/paragrafos/linhas), mantendo o separador na
// unidade que termina.
std::vector<std::string> unidades_de(const std::string& s, const std::string& modo) {
  std::vector<std::string> out;
  std::string atual;
  const auto fecha = [&] {
    if (!atual.empty()) out.push_back(std::move(atual));
    atual.clear();
  };
  for (std::size_t i = 0; i < s.size(); ++i) {
    atual += s[i];
    const bool ultimo = i + 1 == s.size();
    if (modo == "linha") {
      if (s[i] == '\n') fecha();
    } else if (modo == "paragrafo") {
      if (s[i] == '\n' && i + 1 < s.size() && s[i + 1] == '\n') {
        while (i + 1 < s.size() && s[i + 1] == '\n') atual += s[++i];
        fecha();
      }
    } else {  // sentenca
      const bool pontua = s[i] == '.' || s[i] == '!' || s[i] == '?';
      const bool proximo_espaco = ultimo || std::isspace(static_cast<unsigned char>(s[i + 1])) != 0;
      if (pontua && proximo_espaco) {
        while (i + 1 < s.size() && std::isspace(static_cast<unsigned char>(s[i + 1]))) {
          atual += s[++i];
        }
        fecha();
      } else if (s[i] == '\n' && i + 1 < s.size() && s[i + 1] == '\n') {
        while (i + 1 < s.size() && s[i + 1] == '\n') atual += s[++i];
        fecha();
      }
    }
  }
  fecha();
  return out;
}

}  // namespace

std::vector<std::string> dividir_texto_em_pedacos(const std::string& texto, std::size_t tamanho,
                                                  std::size_t sobreposicao,
                                                  const std::string& modo) {
  if (modo == "tamanho") {
    std::vector<std::string> out = fatiar_utf8(texto, tamanho, sobreposicao);
    if (out.empty()) out.push_back(texto);
    return out;
  }
  if (modo != "sentenca" && modo != "paragrafo" && modo != "linha") {
    erro("dividir_texto: modo '" + modo +
         "' invalido (use \"tamanho\", \"sentenca\", \"paragrafo\" ou \"linha\")");
  }
  if (tamanho == 0) tamanho = 1;
  if (sobreposicao > tamanho / 2) sobreposicao = tamanho / 2;  // sempre avanca
  // Unidades maiores que a janela viram varias unidades de janela fixa.
  std::vector<std::string> unidades;
  for (std::string& u : unidades_de(texto, modo)) {
    if (u.size() <= tamanho) {
      unidades.push_back(std::move(u));
    } else {
      for (std::string& p : fatiar_utf8(u, tamanho, 0)) unidades.push_back(std::move(p));
    }
  }
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < unidades.size()) {
    std::string pedaco;
    std::size_t j = i;
    while (j < unidades.size() &&
           (pedaco.empty() || pedaco.size() + unidades[j].size() <= tamanho)) {
      pedaco += unidades[j++];
    }
    out.push_back(std::move(pedaco));
    if (j >= unidades.size()) break;
    // Sobreposicao: reabre com as ultimas unidades que somam ate `sobreposicao`.
    std::size_t volta = j;
    std::size_t soma = 0;
    while (volta > i + 1 && soma + unidades[volta - 1].size() <= sobreposicao) {
      soma += unidades[volta - 1].size();
      --volta;
    }
    // Se a sobreposicao impede a proxima unidade de caber, ela e descartada
    // (senao o pedaco seguinte seria so a repeticao do anterior).
    while (volta < j && soma + unidades[j].size() > tamanho) {
      soma -= unidades[volta].size();
      ++volta;
    }
    i = volta;
  }
  if (out.empty()) out.push_back(texto);
  return out;
}

bool stdlib_existe(const std::string& nome) { return tabela().count(nome) != 0; }

Value stdlib_chamar(const std::string& nome, const std::vector<Value>& args) {
  const auto it = tabela().find(nome);
  if (it == tabela().end()) erro("funcao '" + nome + "' nao definida");
  return it->second(Args(nome, args));
}

}  // namespace tilt::rt
