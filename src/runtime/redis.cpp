#include "runtime/redis.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("redis: " + m); }

constexpr int kTimeoutSec = 5;

struct UrlParts {
  std::string host;
  std::string port;
};

// "redis://host:porta" -> host/porta; ausente -> localhost:6379.
UrlParts parse_url(const std::string& url) {
  std::string rest = url;
  const std::string prefix = "redis://";
  if (rest.rfind(prefix, 0) == 0) rest = rest.substr(prefix.size());
  UrlParts parts{"localhost", "6379"};
  const std::size_t colon = rest.rfind(':');
  if (colon == std::string::npos) {
    if (!rest.empty()) parts.host = rest;
  } else {
    parts.host = rest.substr(0, colon);
    parts.port = rest.substr(colon + 1);
    if (parts.host.empty()) parts.host = "localhost";
    if (parts.port.empty()) parts.port = "6379";
  }
  return parts;
}

// Conexao simples: abre, usa, fecha. Bloqueante com timeout de recv/send.
class Conn {
 public:
  explicit Conn(const std::string& url) {
    const UrlParts parts = parse_url(url);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(parts.host.c_str(), parts.port.c_str(), &hints, &res);
    if (gai != 0) {
      die("nao foi possivel conectar em '" + parts.host + ":" + parts.port + "'");
    }
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
      fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd_ < 0) continue;
      if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0) break;
      ::close(fd_);
      fd_ = -1;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) die("nao foi possivel conectar em '" + parts.host + ":" + parts.port + "'");

    timeval tv{};
    tv.tv_sec = kTimeoutSec;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  ~Conn() {
    if (fd_ >= 0) ::close(fd_);
  }

  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  void send_all(const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
      if (n <= 0) die("falha ao enviar comando");
      off += static_cast<std::size_t>(n);
    }
  }

  // Le ate `n` bytes para `out`; retorna false em EOF/timeout/erro.
  bool read_exact(std::size_t n, std::string& out) {
    out.resize(n);
    std::size_t off = 0;
    while (off < n) {
      const ssize_t r = ::recv(fd_, out.data() + off, n - off, 0);
      if (r <= 0) return false;
      off += static_cast<std::size_t>(r);
    }
    return true;
  }

  // Le uma linha ate \r\n (consumindo o terminador); false em EOF/erro.
  bool read_line(std::string& out) {
    out.clear();
    char c = 0;
    while (true) {
      const ssize_t r = ::recv(fd_, &c, 1, 0);
      if (r <= 0) return false;
      if (c == '\r') {
        char lf = 0;
        if (::recv(fd_, &lf, 1, 0) != 1 || lf != '\n') return false;
        return true;
      }
      out += c;
    }
  }

 private:
  int fd_ = -1;
};

// Valor RESP generico: simple string, error, integer, bulk, array, nil.
struct Resp {
  char type = 0;               // '+', '-', ':', '$', '*'
  std::string str;             // '+' '-' '$'
  std::int64_t num = 0;        // ':', '$' (len, -1 = nil), '*' (len, -1 = nil)
  std::vector<Resp> items;     // '*'
  bool nil = false;            // '$-1' ou '*-1'
};

// Parser recursivo minimo do subconjunto RESP usado pelo cliente.
Resp read_resp(Conn& conn) {
  Resp r;
  std::string line;
  if (!conn.read_line(line) || line.empty()) die("resposta incompleta do servidor");
  r.type = line[0];

  switch (r.type) {
    case '+':
    case '-':
      r.str = line.substr(1);
      return r;
    case ':':
      r.num = std::strtoll(line.c_str() + 1, nullptr, 10);
      return r;
    case '$': {
      r.num = std::strtoll(line.c_str() + 1, nullptr, 10);
      if (r.num == -1) {
        r.nil = true;
        return r;
      }
      std::string body;
      std::string crlf;
      if (!conn.read_exact(static_cast<std::size_t>(r.num), body) || !conn.read_exact(2, crlf)) {
        die("resposta incompleta do servidor");
      }
      if (crlf != "\r\n") die("resposta malformada do servidor");
      r.str = std::move(body);
      return r;
    }
    case '*': {
      r.num = std::strtoll(line.c_str() + 1, nullptr, 10);
      if (r.num == -1) {
        r.nil = true;
        return r;
      }
      r.items.reserve(static_cast<std::size_t>(r.num));
      for (std::int64_t i = 0; i < r.num; ++i) r.items.push_back(read_resp(conn));
      return r;
    }
    default:
      die("tipo RESP desconhecido: " + std::string(1, r.type));
  }
}

// Serializa um comando como array RESP: *N\r\n$<len>\r\n<arg>\r\n...
std::string encode_cmd(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const std::string& a : args) {
    out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  }
  return out;
}

// Envia o comando e devolve a resposta; '-ERR...' vira excecao.
Resp command(const std::string& url, const std::vector<std::string>& args) {
  Conn conn(url);
  conn.send_all(encode_cmd(args));
  Resp r = read_resp(conn);
  if (r.type == '-') die(r.str);
  return r;
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

// Serializador JSON compacto inline: {"k":v,...} e [v,...].
// O json_dump do projeto pretty-printa; o Redis pede 1 linha.
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
    default: return "null";  // Tabela/Tensor: sem suporte nesta passada
  }
}

// Converte um valor tilt para a string gravada no Redis.
std::string value_to_string(const Value& v) {
  switch (v.kind) {
    case ValueKind::Texto: return v.s;
    case ValueKind::Logico: return v.b ? "true" : "false";
    case ValueKind::Inteiro: return std::to_string(v.i);
    case ValueKind::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%g", v.d);
      return buf;
    }
    case ValueKind::Mapa:
    case ValueKind::Lista:
      return json_compact(v);
    default:
      die("tipo '" + std::string(v.type_name()) + "' nao suportado em redis_set");
  }
}

}  // namespace

Value redis_get(const std::string& url, const std::string& chave) {
  Resp r = command(url, {"GET", chave});
  if (r.type != '$') die("resposta inesperada para GET (tipo '" + std::string(1, r.type) + "')");
  if (r.nil) die("chave '" + chave + "' nao encontrada");

  const std::string& raw = r.str;
  if (!raw.empty() && (raw[0] == '{' || raw[0] == '[')) {
    try {
      return json_parse(raw);
    } catch (const std::exception& e) {
      die("valor da chave '" + chave + "' nao e JSON valido: " + e.what());
    }
  }
  return Value::texto(raw);
}

void redis_set(const std::string& url, const std::string& chave, const Value& valor) {
  Resp r = command(url, {"SET", chave, value_to_string(valor)});
  if (r.type != '+') die("resposta inesperada para SET (tipo '" + std::string(1, r.type) + "')");
}

}  // namespace tilt::rt
