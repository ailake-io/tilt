#include "runtime/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "runtime/tensor.hpp"

namespace tilt::rt {
namespace {

struct Reader {
  const std::string& s;
  std::size_t i = 0;

  [[noreturn]] void die(const char* msg) const {
    throw std::runtime_error(std::string("JSON invalido: ") + msg);
  }

  char peek() const { return i < s.size() ? s[i] : '\0'; }
  char take() { return i < s.size() ? s[i++] : '\0'; }

  void skip_ws() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i;
      } else {
        break;
      }
    }
  }

  void literal(const char* word, Value v, Value& out) {
    for (const char* p = word; *p; ++p) {
      if (take() != *p) die("literal invalido");
    }
    out = std::move(v);
  }

  void encode_utf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::string parse_string() {
    if (take() != '"') die("esperado '\"'");
    std::string out;
    while (true) {
      char c = take();
      if (c == '\0') die("string nao terminada");
      if (c == '"') break;
      if (c != '\\') {
        out += c;
        continue;
      }
      char e = take();
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            char h = take();
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else die("escape \\u invalido");
          }
          encode_utf8(cp, out);
          break;
        }
        default:
          die("escape invalido");
      }
    }
    return out;
  }

  Value parse_number() {
    std::size_t start = i;
    if (peek() == '-') ++i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                            s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    std::string tok = s.substr(start, i - start);
    if (tok.find_first_of(".eE") == std::string::npos) {
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(tok.c_str(), &end, 10);
      if (end && *end == '\0' && errno == 0) return Value::inteiro(v);
    }
    return Value::decimal(std::strtod(tok.c_str(), nullptr));
  }

  Value parse_value() {
    skip_ws();
    char c = peek();
    Value out;
    switch (c) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return Value::texto(parse_string());
      case 't': literal("true", Value::logico(true), out); return out;
      case 'f': literal("false", Value::logico(false), out); return out;
      case 'n': literal("null", Value::nulo(), out); return out;
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        die("valor inesperado");
    }
  }

  Value parse_array() {
    take();  // '['
    Value arr = Value::lista();
    skip_ws();
    if (peek() == ']') {
      take();
      return arr;
    }
    while (true) {
      arr.list->push_back(parse_value());
      skip_ws();
      char c = take();
      if (c == ']') break;
      if (c != ',') die("esperado ',' ou ']'");
    }
    return arr;
  }

  Value parse_object() {
    take();  // '{'
    Value obj = Value::mapa();
    skip_ws();
    if (peek() == '}') {
      take();
      return obj;
    }
    while (true) {
      skip_ws();
      std::string key = parse_string();
      skip_ws();
      if (take() != ':') die("esperado ':'");
      obj.map->set(key, parse_value());
      skip_ws();
      char c = take();
      if (c == '}') break;
      if (c != ',') die("esperado ',' ou '}'");
    }
    return obj;
  }
};

void dump_string(const std::string& v, std::string& out) {
  out += '"';
  for (char c : v) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c;
    }
  }
  out += '"';
}

void dump_value(const Value& v, std::string& out, int indent) {
  const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  const std::string pad_in(static_cast<std::size_t>(indent + 1) * 2, ' ');
  switch (v.kind) {
    case ValueKind::Nulo:
      out += "null";
      break;
    case ValueKind::Logico:
      out += v.b ? "true" : "false";
      break;
    case ValueKind::Inteiro:
      out += std::to_string(v.i);
      break;
    case ValueKind::Decimal: {
      std::ostringstream ss;
      ss << v.d;
      out += ss.str();
      break;
    }
    case ValueKind::Texto:
      dump_string(v.s, out);
      break;
    case ValueKind::Lista:
    case ValueKind::Tabela: {
      if (!v.list || v.list->empty()) {
        out += "[]";
        break;
      }
      out += "[\n";
      for (std::size_t k = 0; k < v.list->size(); ++k) {
        out += pad_in;
        dump_value((*v.list)[k], out, indent + 1);
        if (k + 1 < v.list->size()) out += ',';
        out += '\n';
      }
      out += pad + "]";
      break;
    }
    case ValueKind::Tensor: {
      out += "{ \"forma\": [";
      if (v.tensor) {
        for (std::size_t k = 0; k < v.tensor->shape.size(); ++k) {
          if (k) out += ", ";
          out += std::to_string(v.tensor->shape[k]);
        }
      }
      out += "], \"dados\": [";
      if (v.tensor) {
        for (std::size_t k = 0; k < v.tensor->data.size(); ++k) {
          if (k) out += ", ";
          std::ostringstream ss;
          ss << std::setprecision(9) << v.tensor->data[k];  // round-trip exato de f32
          out += ss.str();
        }
      }
      out += "] }";
      break;
    }
    case ValueKind::Funcao:
      out += "null";  // funcoes nao tem representacao JSON
      break;
    case ValueKind::Mapa: {
      if (!v.map || v.map->items.empty()) {
        out += "{}";
        break;
      }
      out += "{\n";
      for (std::size_t k = 0; k < v.map->items.size(); ++k) {
        out += pad_in;
        dump_string(v.map->items[k].first, out);
        out += ": ";
        dump_value(v.map->items[k].second, out, indent + 1);
        if (k + 1 < v.map->items.size()) out += ',';
        out += '\n';
      }
      out += pad + "}";
      break;
    }
  }
}

// Codificador compacto (uma linha) para o protocolo `tilt rpc`.
void compacto_string(const std::string& v, std::string& out) {
  out += '"';
  for (const char ch : v) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += ch;
    }
  }
  out += '"';
}

void compacto_decimal(double d, std::string& out) {
  if (!std::isfinite(d)) {
    out += "null";  // JSON nao tem NaN/Infinity
    return;
  }
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", d);
  // Tenta a menor representacao que volta ao mesmo double (0.1 e nao 0.10000000000000001).
  for (int prec = 6; prec < 17; ++prec) {
    char curto[40];
    std::snprintf(curto, sizeof curto, "%.*g", prec, d);
    if (std::strtod(curto, nullptr) == d) {
      std::snprintf(buf, sizeof buf, "%s", curto);
      break;
    }
  }
  std::string txt = buf;
  // Decimal inteiro (2.0) mantem o ponto para nao virar `inteiro` do outro lado.
  if (txt.find_first_of(".eEn") == std::string::npos) txt += ".0";
  out += txt;
}

void compacto_value(const Value& v, std::string& out) {
  switch (v.kind) {
    case ValueKind::Nulo:
    case ValueKind::Funcao:
      out += "null";
      break;
    case ValueKind::Logico:
      out += v.b ? "true" : "false";
      break;
    case ValueKind::Inteiro:
      out += std::to_string(v.i);
      break;
    case ValueKind::Decimal:
      compacto_decimal(v.d, out);
      break;
    case ValueKind::Texto:
      compacto_string(v.s, out);
      break;
    case ValueKind::Lista:
    case ValueKind::Tabela: {
      out += '[';
      if (v.list) {
        for (std::size_t k = 0; k < v.list->size(); ++k) {
          if (k) out += ',';
          compacto_value((*v.list)[k], out);
        }
      }
      out += ']';
      break;
    }
    case ValueKind::Tensor: {
      out += "{\"forma\":[";
      if (v.tensor) {
        for (std::size_t k = 0; k < v.tensor->shape.size(); ++k) {
          if (k) out += ',';
          out += std::to_string(v.tensor->shape[k]);
        }
      }
      out += "],\"dados\":[";
      if (v.tensor) {
        for (std::size_t k = 0; k < v.tensor->data.size(); ++k) {
          if (k) out += ',';
          compacto_decimal(static_cast<double>(v.tensor->data[k]), out);
        }
      }
      out += "]}";
      break;
    }
    case ValueKind::Mapa: {
      out += '{';
      if (v.map) {
        for (std::size_t k = 0; k < v.map->items.size(); ++k) {
          if (k) out += ',';
          compacto_string(v.map->items[k].first, out);
          out += ':';
          compacto_value(v.map->items[k].second, out);
        }
      }
      out += '}';
      break;
    }
  }
}

}  // namespace

Value json_parse(const std::string& text) {
  Reader r{text};
  Value v = r.parse_value();
  r.skip_ws();
  if (r.i != text.size()) throw std::runtime_error("JSON invalido: lixo apos o valor");
  return v;
}

std::string json_dump_compacto(const Value& value) {
  std::string out;
  compacto_value(value, out);
  return out;
}

std::string json_dump(const Value& value) {
  std::string out;
  dump_value(value, out, 0);
  out += '\n';
  return out;
}

}  // namespace tilt::rt
