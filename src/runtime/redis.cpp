#include "runtime/redis.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/compat.hpp"
#include "runtime/json.hpp"
#include "runtime/tls.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("redis: " + m); }

constexpr int kTimeoutSec = 5;

struct UrlParts {
  std::string host = "localhost";
  std::string port = "6379";
  std::string auth;  // userinfo ":senha@" -> senha (vazio = sem AUTH)
  int db = -1;       // path "/N" (negativo = sem SELECT)
  bool tls = false;  // esquema "rediss://" (ou {tls: verdadeiro} nas opcoes)
};

// "redis://[:senha@]host[:porta][/N]" -> host/porta/auth/db; ausente ->
// localhost:6379, sem AUTH, sem SELECT. Query string nao e suportada.
// "rediss://" ativa TLS (OpenSSL via dlopen; ver runtime/tls.hpp).
UrlParts parse_url(const std::string& url) {
  std::string rest = url;
  const std::string prefix_tls = "rediss://";
  const std::string prefix = "redis://";
  UrlParts parts;
  if (rest.rfind(prefix_tls, 0) == 0) {
    parts.tls = true;
    rest = rest.substr(prefix_tls.size());
  } else if (rest.rfind(prefix, 0) == 0) {
    rest = rest.substr(prefix.size());
  }
  // userinfo: tudo antes do '@' ("usuario:senha" ou ":senha"; o usuario,
  // se houver, e ignorado — o Redis classic AUTH so usa a senha).
  const std::size_t at = rest.rfind('@');
  if (at != std::string::npos) {
    const std::string ui = rest.substr(0, at);
    rest = rest.substr(at + 1);
    const std::size_t colon = ui.find(':');
    parts.auth = colon == std::string::npos ? ui : ui.substr(colon + 1);
  }
  // path: "/N" -> numero do banco.
  const std::size_t slash = rest.find('/');
  if (slash != std::string::npos) {
    const std::string dbs = rest.substr(slash + 1);
    rest = rest.substr(0, slash);
    if (dbs.empty() || dbs.find_first_not_of("0123456789") != std::string::npos) {
      die("path da URL deve ser o numero do banco (ex.: redis://host:6379/2)");
    }
    parts.db = std::atoi(dbs.c_str());
  }
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

// Aplica as opcoes do builtin por cima do que a URL trouxe: opcao informada
// (senha nao vazia / banco >= 0) vence a URL; ausente herda a URL. TLS liga
// se a URL for rediss:// ou se {tls: verdadeiro} for informado.
RedisOpts merge_opts(const UrlParts& parts, const RedisOpts& opts) {
  RedisOpts eff;
  eff.auth = opts.auth.empty() ? parts.auth : opts.auth;
  eff.db = opts.db < 0 ? parts.db : opts.db;
  eff.tls = opts.tls || parts.tls;
  return eff;
}

// Conexao simples: abre, usa, fecha. Bloqueante com timeout de recv/send.
// Com `parts.tls`, o trafego passa por TlsStream (handshake cliente logo
// apos o connect).
class Conn {
 public:
  explicit Conn(const UrlParts& parts) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(parts.host.c_str(), parts.port.c_str(), &hints, &res);
    if (gai != 0) {
      die("nao foi possivel conectar em '" + parts.host + ":" + parts.port + "'");
    }
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
      fd_ = tilt_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd_ < 0) continue;
      if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0) break;
      tilt_close_socket(fd_);
      fd_ = -1;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) die("nao foi possivel conectar em '" + parts.host + ":" + parts.port + "'");

    tilt_set_sock_timeouts(fd_, kTimeoutSec);

    if (parts.tls) tls_.emplace(fd_, parts.host);
  }

  ~Conn() {
    if (fd_ >= 0) tilt_close_socket(fd_);
  }

  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  void send_all(const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = send_raw(data.data() + off, data.size() - off);
      if (n <= 0) die("falha ao enviar comando");
      off += static_cast<std::size_t>(n);
    }
  }

  // Le ate `n` bytes para `out`; retorna false em EOF/timeout/erro.
  bool read_exact(std::size_t n, std::string& out) {
    out.resize(n);
    std::size_t off = 0;
    while (off < n) {
      const ssize_t r = recv_raw(out.data() + off, n - off);
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
      const ssize_t r = recv_raw(&c, 1);
      if (r <= 0) return false;
      if (c == '\r') {
        char lf = 0;
        if (recv_raw(&lf, 1) != 1 || lf != '\n') return false;
        return true;
      }
      out += c;
    }
  }

 private:
  // Primitivas de transporte: TLS quando ativo, socket cru caso contrario.
  ssize_t send_raw(const char* p, std::size_t n) {
    if (tls_) {
      tls_->write_all(p, n);
      return static_cast<ssize_t>(n);
    }
    return tilt_send(fd_, p, n);
  }
  ssize_t recv_raw(char* p, std::size_t n) {
    if (tls_) return static_cast<ssize_t>(tls_->read_some(p, n));
    return tilt_recv(fd_, p, n);
  }

  int fd_ = -1;
  std::optional<TlsStream> tls_;
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

// Envia AUTH/SELECT conforme as opcoes efetivas; ambos exigem +OK.
void handshake(Conn& conn, const RedisOpts& opts) {
  if (!opts.auth.empty()) {
    conn.send_all(encode_cmd({"AUTH", opts.auth}));
    const Resp r = read_resp(conn);
    if (r.type != '+') die("auth falhou (senha invalida?)");
  }
  if (opts.db >= 0) {
    conn.send_all(encode_cmd({"SELECT", std::to_string(opts.db)}));
    const Resp r = read_resp(conn);
    if (r.type != '+') die("select db " + std::to_string(opts.db) + " falhou");
  }
}

// Abre a conexao, faz o handshake opcional, envia o comando e devolve a
// resposta; '-ERR...' vira excecao. `opts` ja e o resultado de merge_opts.
Resp command(const UrlParts& parts, const RedisOpts& opts,
             const std::vector<std::string>& args) {
  UrlParts eff = parts;
  eff.tls = opts.tls;
  Conn conn(eff);
  handshake(conn, opts);
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

Value redis_get(const std::string& url, const std::string& chave, const RedisOpts& opts) {
  const UrlParts parts = parse_url(url);
  Resp r = command(parts, merge_opts(parts, opts), {"GET", chave});
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

void redis_set(const std::string& url, const std::string& chave, const Value& valor,
               const RedisOpts& opts) {
  const UrlParts parts = parse_url(url);
  Resp r = command(parts, merge_opts(parts, opts), {"SET", chave, value_to_string(valor)});
  if (r.type != '+') die("resposta inesperada para SET (tipo '" + std::string(1, r.type) + "')");
}

}  // namespace tilt::rt
