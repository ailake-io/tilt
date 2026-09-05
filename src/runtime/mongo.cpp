#include "runtime/mongo.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("mongo: " + m); }

constexpr int kTimeoutSec = 5;
constexpr std::int32_t kOpMsg = 2013;
constexpr std::uint32_t kFlagChecksum = 1u;  // bit 0: ultimos 4 bytes = checksum (ignorado)

// ------------------------------------------------------------- codec BSON
//
// Tudo little-endian (BSON e wire protocol do MongoDB). Tipos suportados:
// 0x01 double, 0x02 string, 0x03 document, 0x04 array, 0x07 ObjectId,
// 0x08 bool, 0x09 datetime (int64 ms), 0x0A null, 0x10 int32, 0x12 int64.

void put_le32(std::string& out, std::uint32_t v) {
  for (int k = 0; k < 4; ++k) out.push_back(static_cast<char>((v >> (k * 8)) & 0xFF));
}
void put_le64(std::string& out, std::uint64_t v) {
  for (int k = 0; k < 8; ++k) out.push_back(static_cast<char>((v >> (k * 8)) & 0xFF));
}
void put_le_double(std::string& out, double v) {
  std::uint64_t u = 0;
  std::memcpy(&u, &v, 8);
  put_le64(out, u);
}
void put_cstr(std::string& out, const std::string& s) {
  out += s;
  out.push_back('\0');
}
void put_bson_str(std::string& out, const std::string& s) {
  put_le32(out, static_cast<std::uint32_t>(s.size() + 1));
  out += s;
  out.push_back('\0');
}

void bson_element(std::string& out, char type, const std::string& name, const std::string& value) {
  out.push_back(type);
  put_cstr(out, name);
  out += value;
}

// Serializa um mapa tilt como documento BSON. `object_id_gerado` (12 bytes
// brutos) vira o `_id` ObjectId quando o mapa nao o define.
void bson_encode_doc(std::string& out, const ValueMap& map, const std::string* object_id_gerado) {
  std::string body;
  if (object_id_gerado && !map.find("_id")) {
    bson_element(body, 0x07, "_id", *object_id_gerado);
  }
  for (const auto& [k, v] : map.items) {
    if (k.find('\0') != std::string::npos) {
      die("nome de campo nao pode conter '\\0' (campo '" + k + "')");
    }
    switch (v.kind) {
      case ValueKind::Decimal: {
        std::string val;
        put_le_double(val, v.d);
        bson_element(body, 0x01, k, val);
        break;
      }
      case ValueKind::Texto: {
        std::string val;
        put_bson_str(val, v.s);
        bson_element(body, 0x02, k, val);
        break;
      }
      case ValueKind::Mapa: {
        if (!v.map) die("mapa invalido (campo '" + k + "')");
        std::string val;
        bson_encode_doc(val, *v.map, nullptr);
        bson_element(body, 0x03, k, val);
        break;
      }
      case ValueKind::Lista: {
        if (!v.list) die("lista invalida (campo '" + k + "')");
        ValueMap como_mapa;
        for (std::size_t i = 0; i < v.list->size(); ++i) {
          como_mapa.set(std::to_string(i), (*v.list)[i]);
        }
        std::string val;
        bson_encode_doc(val, como_mapa, nullptr);
        bson_element(body, 0x04, k, val);
        break;
      }
      case ValueKind::Logico:
        bson_element(body, 0x08, k, std::string(1, v.b ? '\x01' : '\x00'));
        break;
      case ValueKind::Nulo:
        bson_element(body, 0x0A, k, "");
        break;
      case ValueKind::Inteiro: {
        std::string val;
        put_le64(val, static_cast<std::uint64_t>(v.i));
        bson_element(body, 0x12, k, val);
        break;
      }
      case ValueKind::Tabela:
      case ValueKind::Tensor:
        die(std::string("nao e possivel gravar ") + v.type_name() + " no MongoDB (campo '" + k +
            "')");
    }
  }
  put_le32(out, static_cast<std::uint32_t>(body.size() + 5));
  out += body;
  out.push_back('\0');
}

std::string bson_doc(const ValueMap& map) {
  std::string out;
  bson_encode_doc(out, map, nullptr);
  return out;
}

struct Reader {
  const std::string& d;
  std::size_t pos = 0;

  void need(std::size_t n) {
    if (pos + n > d.size()) die("documento BSON truncado");
  }
  std::uint8_t u8() {
    need(1);
    return static_cast<std::uint8_t>(static_cast<unsigned char>(d[pos++]));
  }
  std::uint16_t le16() {
    need(2);
    std::uint16_t b = 0;
    for (int k = 1; k >= 0; --k) b = static_cast<std::uint16_t>((b << 8) | byte_at(pos + k));
    pos += 2;
    return b;
  }
  std::uint32_t le32() {
    need(4);
    std::uint32_t b = 0;
    for (int k = 3; k >= 0; --k) b = (b << 8) | byte_at(pos + k);
    pos += 4;
    return b;
  }
  std::uint64_t le64() {
    need(8);
    std::uint64_t b = 0;
    for (int k = 7; k >= 0; --k) b = (b << 8) | byte_at(pos + k);
    pos += 8;
    return b;
  }
  double dbl() {
    const std::uint64_t u = le64();
    double v = 0.0;
    std::memcpy(&v, &u, 8);
    return v;
  }
  std::uint8_t byte_at(std::size_t p) const {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(d[p]));
  }
  std::string cstr() {
    const std::size_t nul = d.find('\0', pos);
    if (nul == std::string::npos) die("documento BSON truncado");
    const std::string s = d.substr(pos, nul - pos);
    pos = nul + 1;
    return s;
  }
  std::string bson_str() {
    const std::uint32_t n = le32();
    if (n == 0) die("string BSON com tamanho 0");
    need(n);
    const std::string s = d.substr(pos, n - 1);
    if (d[pos + n - 1] != '\0') die("string BSON sem terminador nulo");
    pos += n;
    return s;
  }
  void skip(std::size_t n) {
    need(n);
    pos += n;
  }
};

std::string hex2(std::uint8_t t) {
  const char* hexd = "0123456789abcdef";
  std::string s = "0x";
  s.push_back(hexd[(t >> 4) & 0xF]);
  s.push_back(hexd[t & 0xF]);
  return s;
}

std::string hex24(const std::string& raw) {
  const char* hexd = "0123456789abcdef";
  std::string s;
  s.reserve(24);
  for (const unsigned char c : raw) {
    s.push_back(hexd[(c >> 4) & 0xF]);
    s.push_back(hexd[c & 0xF]);
  }
  return s;
}

Value bson_parse_doc(Reader& r);

Value bson_parse_value(std::uint8_t type, const std::string& campo, Reader& r) {
  switch (type) {
    case 0x01:
      return Value::decimal(r.dbl());
    case 0x02:
      return Value::texto(r.bson_str());
    case 0x03:
      return bson_parse_doc(r);
    case 0x04: {
      Value doc = bson_parse_doc(r);
      Value out = Value::lista();
      if (doc.map) {
        for (const auto& [k, v] : doc.map->items) {
          (void)k;  // arrays BSON: posicao, nao nome
          out.list->push_back(v);
        }
      }
      return out;
    }
    case 0x07: {
      r.need(12);
      const std::string raw = r.d.substr(r.pos, 12);
      r.pos += 12;
      return Value::texto(hex24(raw));
    }
    case 0x08:
      return Value::logico(r.u8() != 0);
    case 0x09:
      return Value::inteiro(static_cast<std::int64_t>(r.le64()));
    case 0x0A:
      return Value::nulo();
    case 0x10:
      return Value::inteiro(static_cast<std::int32_t>(r.le32()));
    case 0x12:
      return Value::inteiro(static_cast<std::int64_t>(r.le64()));
    default:
      die("tipo BSON nao suportado: " + hex2(type) + " (campo '" + campo + "')");
  }
}

Value bson_parse_doc(Reader& r) {
  const std::size_t inicio = r.pos;
  const std::uint32_t len = r.le32();
  if (len < 5 || inicio + len > r.d.size()) die("documento BSON com tamanho invalido");
  const std::size_t fim = inicio + len;
  Value out = Value::mapa();
  while (r.pos < fim) {
    const std::uint8_t type = r.u8();
    if (type == 0x00) {
      if (r.pos != fim) die("documento BSON com tamanho invalido");  // terminador deve ser o ultimo byte
      return out;
    }
    const std::string campo = r.cstr();
    out.map->set(campo, bson_parse_value(type, campo, r));
  }
  die("documento BSON com tamanho invalido");  // fim sem terminador
}

// -------------------------------------------------------------- ObjectId

std::string novo_object_id() {
  static const std::array<unsigned char, 5> aleatorio = [] {
    std::array<unsigned char, 5> b{};
    std::random_device rd;
    for (auto& c : b) c = static_cast<unsigned char>(rd() & 0xFF);
    return b;
  }();
  static std::uint32_t contador = std::random_device{}() & 0xFFFFFFu;
  contador = (contador + 1) & 0xFFFFFFu;

  const auto agora = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  std::string out;
  out.reserve(12);
  put_le32(out, static_cast<std::uint32_t>(agora));
  for (const unsigned char c : aleatorio) out.push_back(static_cast<char>(c));
  out.push_back(static_cast<char>(contador & 0xFF));
  out.push_back(static_cast<char>((contador >> 8) & 0xFF));
  out.push_back(static_cast<char>((contador >> 16) & 0xFF));
  return out;
}

// ----------------------------------------------------------------- conexao

struct MongoUrl {
  std::string host;
  std::string port;
  std::string banco;
};

MongoUrl parse_url() {
  const char* env = std::getenv("MONGO_URL");
  std::string url = env && *env ? env : "mongodb://127.0.0.1:27017";
  const std::string prefixo = "mongodb://";
  if (url.rfind(prefixo, 0) == 0) url = url.substr(prefixo.size());

  MongoUrl u;
  const std::size_t barra = url.find('/');
  const std::string hostport = url.substr(0, barra);
  if (barra != std::string::npos) {
    std::string path = url.substr(barra + 1);
    const std::size_t query = path.find('?');
    if (query != std::string::npos) path = path.substr(0, query);
    u.banco = path;
  }
  const std::size_t dois_pontos = hostport.rfind(':');
  if (dois_pontos == std::string::npos) {
    u.host = hostport;
    u.port = "27017";
  } else {
    u.host = hostport.substr(0, dois_pontos);
    u.port = hostport.substr(dois_pontos + 1);
  }
  if (u.host.empty()) die("MONGO_URL sem host (esperado mongodb://host:porta/banco)");
  return u;
}

// Conexao simples: abre, usa, fecha. Bloqueante com timeout de recv/send.
class Conn {
 public:
  Conn(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0) die("nao foi possivel conectar em '" + host + ":" + port + "'");
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
      fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd_ < 0) continue;
      if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0) break;
      ::close(fd_);
      fd_ = -1;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) die("nao foi possivel conectar em '" + host + ":" + port + "'");

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

  void write_full(const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
      if (n <= 0) die("falha ao enviar requisicao");
      off += static_cast<std::size_t>(n);
    }
  }

  void read_full(std::size_t n, std::string& out) {
    out.resize(n);
    std::size_t off = 0;
    while (off < n) {
      const ssize_t r = ::recv(fd_, out.data() + off, n - off, 0);
      if (r <= 0) die("resposta incompleta do servidor");
      off += static_cast<std::size_t>(r);
    }
  }

 private:
  int fd_ = -1;
};

// --------------------------------------------------------------- OP_MSG

// ok:1 (double/int) da resposta do servidor.
bool ok_de(const Value& resp) {
  if (!resp.map) return false;
  const Value* ok = resp.map->find("ok");
  if (!ok || !ok->is_number()) return false;
  return ok->as_number() != 0.0;
}

std::string errmsg_de(const Value& resp) {
  if (resp.map) {
    if (const Value* e = resp.map->find("errmsg"); e && e->kind == ValueKind::Texto) return e->s;
  }
  return "comando falhou (ok: 0)";
}

// Sessao OP_MSG: connecta, faz o handshake isMaster e executa comandos.
class Sessao {
 public:
  explicit Sessao(const MongoUrl& url) {
    url_ = url;
    conn_ = std::make_unique<Conn>(url_.host, url_.port);

    Value handshake = Value::mapa();
    handshake.map->set("isMaster", Value::inteiro(1));
    const Value resp = comando(handshake);
    if (!ok_de(resp)) die("handshake isMaster falhou (ok != 1)");
  }

  Sessao(const Sessao&) = delete;
  Sessao& operator=(const Sessao&) = delete;

  // Envia um documento de comando {chave: valor, ...} como OP_MSG (section
  // kind 0) e devolve o documento da resposta (primeira section kind 0;
  // sections kind 1 sao puladas; bit de checksum ignorado na leitura).
  Value comando(const Value& cmd) {
    if (!cmd.map) die("comando interno invalido");
    return comando_bson(bson_doc(*cmd.map));
  }

  // Idem, mas o documento de comando ja vem codificado em BSON (uso interno
  // para tipos que o Value nao carrega, ex.: ObjectId gerado).
  Value comando_bson(const std::string& cmd_bson) {
    std::string body;
    put_le32(body, static_cast<std::uint32_t>(++req_id_));
    put_le32(body, 0);  // responseTo
    put_le32(body, kOpMsg);
    put_le32(body, 0);  // flagBits
    body.push_back(0);  // section kind 0
    body += cmd_bson;

    std::string frame;
    put_le32(frame, static_cast<std::uint32_t>(body.size() + 4));
    frame += body;
    conn_->write_full(frame);

    std::string hdr;
    conn_->read_full(4, hdr);
    Reader hr{hdr};
    const std::uint32_t len = hr.le32();
    if (len < 16 || len > 64 * 1024 * 1024) die("resposta malformada do servidor");
    std::string resp;
    conn_->read_full(len - 4, resp);

    Reader r{resp};
    r.le32();  // requestID da resposta
    r.le32();  // responseTo
    if (static_cast<std::int32_t>(r.le32()) != kOpMsg) die("opcode inesperado na resposta");
    const std::uint32_t flags = r.le32();
    std::size_t fim = resp.size();
    if (flags & kFlagChecksum) {
      if (fim < 4 + 16) die("resposta malformada do servidor");
      fim -= 4;
    }
    docs_.clear();
    while (r.pos < fim) {
      const std::uint8_t kind = r.u8();
      if (kind == 0) {
        docs_.push_back(bson_parse_doc(r));
      } else if (kind == 1) {
        const std::uint32_t section_len = r.le32();
        if (section_len < 4 || r.pos - 4 + section_len > fim) {
          die("section kind 1 com tamanho invalido");
        }
        r.pos = r.pos - 4 + section_len;
      } else {
        die("section OP_MSG nao suportada: kind " + std::to_string(kind));
      }
    }
    if (docs_.empty()) die("resposta sem documento (section kind 0)");
    return docs_.front();
  }

 private:
  std::unique_ptr<Conn> conn_;
  MongoUrl url_;
  std::int32_t req_id_ = 0;
  std::vector<Value> docs_;  // reuso de buffer entre comandos
};

// Banco efetivo: opcao `banco:` > path do MONGO_URL > erro acionavel (antes
// de tocar a rede).
std::string banco_efetivo(const MongoUrl& url, const std::string& opt) {
  if (!opt.empty()) return opt;
  if (!url.banco.empty()) return url.banco;
  die("defina o banco no MONGO_URL (mongodb://host:porta/banco) ou em {banco: ...}");
}

}  // namespace

void mongo_inserir(const std::string& colecao, const Value& doc, const std::string& banco) {
  if (doc.kind != ValueKind::Mapa || !doc.map) die("inserir espera um mapa");

  const MongoUrl url = parse_url();
  const std::string db = banco_efetivo(url, banco);
  Sessao sessao(url);

  // Codifica o comando na mao para injetar o _id ObjectId (12 bytes brutos)
  // quando o documento nao o define — pelo Value ele seria so um texto.
  std::string cmd_bson;
  {
    std::string id;
    const bool tem_id = doc.map->find("_id") != nullptr;
    if (!tem_id) id = novo_object_id();
    std::string doc_bson;
    bson_encode_doc(doc_bson, *doc.map, tem_id ? nullptr : &id);

    std::string arr_body;
    bson_element(arr_body, 0x03, "0", doc_bson);
    std::string arr;
    put_le32(arr, static_cast<std::uint32_t>(arr_body.size() + 5));
    arr += arr_body;
    arr.push_back('\0');

    std::string cmd_body;
    {
      std::string v;
      put_bson_str(v, colecao);
      bson_element(cmd_body, 0x02, "insert", v);
    }
    {
      std::string v;
      put_bson_str(v, db);
      bson_element(cmd_body, 0x02, "$db", v);
    }
    bson_element(cmd_body, 0x04, "documents", arr);
    put_le32(cmd_bson, static_cast<std::uint32_t>(cmd_body.size() + 5));
    cmd_bson += cmd_body;
    cmd_bson.push_back('\0');
  }

  const Value resp = sessao.comando_bson(cmd_bson);
  if (!ok_de(resp)) die("insert em '" + colecao + "': " + errmsg_de(resp));
}

Value mongo_buscar(const std::string& colecao, const Value& filtro, std::int64_t max,
                   const std::string& banco) {
  if (filtro.kind != ValueKind::Mapa || !filtro.map) die("buscar espera um mapa como filtro");
  if (max < 0) die("max deve ser >= 0");

  const MongoUrl url = parse_url();
  const std::string db = banco_efetivo(url, banco);
  Sessao sessao(url);

  Value cmd = Value::mapa();
  cmd.map->set("find", Value::texto(colecao));
  cmd.map->set("$db", Value::texto(db));
  cmd.map->set("filter", filtro);
  cmd.map->set("limit", Value::inteiro(max));
  cmd.map->set("batchSize", Value::inteiro(max));

  const Value resp = sessao.comando(cmd);
  if (!ok_de(resp)) die("find em '" + colecao + "': " + errmsg_de(resp));
  if (!resp.map) die("resposta de find sem documento");

  const Value* cursor = resp.map->find("cursor");
  if (!cursor || cursor->kind != ValueKind::Mapa || !cursor->map) {
    die("resposta de find sem 'cursor'");
  }
  const Value* batch = cursor->map->find("firstBatch");
  if (!batch || batch->kind != ValueKind::Lista || !batch->list) {
    die("resposta de find sem 'cursor.firstBatch'");
  }
  return *batch;
}

}  // namespace tilt::rt
