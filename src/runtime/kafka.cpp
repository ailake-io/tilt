#include "runtime/kafka.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("kafka: " + m); }

constexpr int kTimeoutSec = 5;

// ------------------------------------------------------------- CRC32 (IEEE)

const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return table;
}

std::uint32_t crc32_ieee(const std::string& data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  const auto& t = crc_table();
  for (const unsigned char c : data) crc = (crc >> 8) ^ t[(crc ^ c) & 0xFFu];
  return crc ^ 0xFFFFFFFFu;
}

// ----------------------------------------------------------- codec binario

void put_i8(std::string& out, std::int8_t v) { out.push_back(static_cast<char>(v)); }
void put_i16(std::string& out, std::int16_t v) {
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}
void put_i32(std::string& out, std::int32_t v) {
  for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((v >> (k * 8)) & 0xFF));
}
void put_i64(std::string& out, std::int64_t v) {
  for (int k = 7; k >= 0; --k) out.push_back(static_cast<char>((v >> (k * 8)) & 0xFF));
}
void put_str(std::string& out, const std::string& s) {  // STRING: int16 len
  put_i16(out, static_cast<std::int16_t>(s.size()));
  out += s;
}
void put_bytes(std::string& out, const std::string& s) {  // BYTES: int32 len
  put_i32(out, static_cast<std::int32_t>(s.size()));
  out += s;
}

struct Reader {
  const std::string& d;
  std::size_t pos = 0;

  void need(std::size_t n) {
    if (pos + n > d.size()) die("resposta truncada do broker");
  }
  std::int8_t i8() {
    need(1);
    return static_cast<std::int8_t>(static_cast<unsigned char>(d[pos++]));
  }
  std::int16_t i16() {
    need(2);
    const auto b = static_cast<std::uint16_t>(static_cast<unsigned char>(d[pos])) << 8 |
                   static_cast<std::uint16_t>(static_cast<unsigned char>(d[pos + 1]));
    pos += 2;
    return static_cast<std::int16_t>(b);
  }
  std::int32_t i32() {
    need(4);
    std::uint32_t b = 0;
    for (int k = 0; k < 4; ++k) b = (b << 8) | static_cast<unsigned char>(d[pos + k]);
    pos += 4;
    return static_cast<std::int32_t>(b);
  }
  std::int64_t i64() {
    need(8);
    std::uint64_t b = 0;
    for (int k = 0; k < 8; ++k) b = (b << 8) | static_cast<unsigned char>(d[pos + k]);
    pos += 8;
    return static_cast<std::int64_t>(b);
  }
  std::string str() {  // STRING (len int16; -1 -> vazio)
    const std::int16_t n = i16();
    if (n <= 0) return "";
    need(static_cast<std::size_t>(n));
    const std::string s = d.substr(pos, static_cast<std::size_t>(n));
    pos += static_cast<std::size_t>(n);
    return s;
  }
  std::string bytes() {  // BYTES (len int32; -1 -> vazio)
    const std::int32_t n = i32();
    if (n < 0) return "";
    need(static_cast<std::size_t>(n));
    const std::string s = d.substr(pos, static_cast<std::size_t>(n));
    pos += static_cast<std::size_t>(n);
    return s;
  }
  void skip(std::size_t n) {
    need(n);
    pos += n;
  }
};

// --------------------------------------------------------------- transporte

struct BrokerAddr {
  std::string host;
  std::string port;
};

BrokerAddr bootstrap_addr() {
  const char* env = std::getenv("KAFKA_BOOTSTRAP");
  std::string addr = env && *env ? env : "127.0.0.1:9092";
  const std::size_t colon = addr.rfind(':');
  if (colon == std::string::npos) return {addr, "9092"};
  return {addr.substr(0, colon), addr.substr(colon + 1)};
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
      if (r <= 0) die("resposta incompleta do broker");
      off += static_cast<std::size_t>(r);
    }
  }

 private:
  int fd_ = -1;
};

// ------------------------------------------------------- framing da requisicao

constexpr std::int16_t kClientIdCorrBase = 0;

// Envia [length][api_key][api_version][corr][client_id][payload] e devolve o
// payload da resposta (sem o int32 de correlation_id, ja conferido).
std::string roundtrip(Conn& conn, std::int16_t api_key, std::int16_t api_version, std::int32_t corr,
                      const std::string& payload) {
  std::string body;
  put_i16(body, api_key);
  put_i16(body, api_version);
  put_i32(body, corr);
  put_str(body, "tilt");
  body += payload;

  std::string frame;
  put_i32(frame, static_cast<std::int32_t>(body.size()));
  frame += body;
  conn.write_full(frame);

  std::string hdr;
  conn.read_full(4, hdr);
  Reader hr{hdr};
  const std::int32_t len = hr.i32();
  if (len < 4 || len > 64 * 1024 * 1024) die("resposta malformada do broker");
  std::string resp;
  conn.read_full(static_cast<std::size_t>(len), resp);
  Reader r{resp};
  if (r.i32() != corr) die("correlation_id inesperado na resposta");
  return resp.substr(r.pos);
}

// ------------------------------------------------------------------- erros

[[noreturn]] void die_code(const std::string& contexto, std::int16_t code) {
  switch (code) {
    case 1:
      die(contexto + " falhou: OffsetOutOfRange (kafka codigo 1)");
    case 2:
      die(contexto + " falhou: CorruptMessage (kafka codigo 2)");
    case 3:
      die(contexto + " falhou: UnknownTopicOrPartition (kafka codigo 3)");
    case 5:
      die(contexto + " falhou: LeaderNotAvailable (kafka codigo 5)");
    case 6:
      die(contexto + " falhou: NotLeaderForPartition (kafka codigo 6)");
    case 7:
      die(contexto + " falhou: RequestTimedOut (kafka codigo 7)");
    default:
      die(contexto + " falhou (kafka codigo " + std::to_string(code) + ")");
  }
}

// ----------------------------------------------------------------- metadata

struct BrokerInfo {
  std::int32_t node_id = 0;
  std::string host;
  std::int32_t port = 0;
};

struct PartitionInfo {
  std::int16_t error = 0;
  std::int32_t leader = -1;
};

struct Metadata {
  std::vector<BrokerInfo> brokers;
  std::vector<PartitionInfo> partitions;  // indexado pelo id da particao
};

Metadata metadata(const std::string& topico, const BrokerAddr& addr) {
  Conn conn(addr.host, addr.port);
  std::string payload;
  put_i32(payload, 1);
  put_str(payload, topico);
  const std::string resp = roundtrip(conn, 3, 0, kClientIdCorrBase + 1, payload);

  Metadata md;
  Reader r{resp};
  const std::int32_t nb = r.i32();
  if (nb < 0) die("resposta de metadata malformada");
  for (std::int32_t i = 0; i < nb; ++i) {
    BrokerInfo b;
    b.node_id = r.i32();
    b.host = r.str();
    b.port = r.i32();
    md.brokers.push_back(std::move(b));
  }
  const std::int32_t nt = r.i32();
  if (nt < 1) die("topico '" + topico + "' nao encontrado no metadata");
  for (std::int32_t t = 0; t < nt; ++t) {
    const std::string nome = r.str();
    const std::int32_t np = r.i32();
    if (np < 0) die("resposta de metadata malformada");
    for (std::int32_t p = 0; p < np; ++p) {
      PartitionInfo pi;
      pi.error = r.i16();
      const std::int32_t id = r.i32();
      pi.leader = r.i32();
      const std::int32_t nr = r.i32();  // [replicas]
      if (nr < 0) die("resposta de metadata malformada");
      r.skip(static_cast<std::size_t>(nr) * 4);
      const std::int32_t ni = r.i32();  // [isr]
      if (ni < 0) die("resposta de metadata malformada");
      r.skip(static_cast<std::size_t>(ni) * 4);
      if (nome == topico) {
        if (id >= 0 && static_cast<std::size_t>(id) >= md.partitions.size()) {
          md.partitions.resize(static_cast<std::size_t>(id) + 1);
        }
        if (id >= 0) md.partitions[static_cast<std::size_t>(id)] = pi;
      }
    }
  }
  return md;
}

// Acha o lider da particao e devolve seu endereco; conecta no bootstrap e,
// se o lider for outro broker, devolve o endereco dele.
BrokerAddr lider_addr(const std::string& topico, std::int32_t particao, Metadata& md) {
  const BrokerAddr bootstrap = bootstrap_addr();
  md = metadata(topico, bootstrap);
  if (static_cast<std::size_t>(particao) >= md.partitions.size()) {
    die("topico '" + topico + "' nao tem a particao " + std::to_string(particao));
  }
  const PartitionInfo& pi = md.partitions[static_cast<std::size_t>(particao)];
  if (pi.error != 0) die_code("metadata do topico '" + topico + "'", pi.error);
  for (const BrokerInfo& b : md.brokers) {
    if (b.node_id == pi.leader) return {b.host, std::to_string(b.port)};
  }
  die("lider da particao " + std::to_string(particao) + " do topico '" + topico +
      "' nao encontrado no metadata");
}

// ------------------------------------------------------------------ message

// MessageSet v0: offset(int64), message_size(int32), message
// (crc(int32) do corpo, magic(int8), attributes(int8), key BYTES, value BYTES).
std::string encode_message(const std::string& valor) {
  std::string body;
  put_i8(body, 0);    // magic v0
  put_i8(body, 0);    // attributes (sem compressao)
  put_i32(body, -1);  // key = NULL
  put_bytes(body, valor);
  std::string msg;
  put_i32(msg, static_cast<std::int32_t>(crc32_ieee(body)));
  msg += body;
  std::string out;
  put_i64(out, 0);  // offset ignorado pelo broker na producao
  put_i32(out, static_cast<std::int32_t>(msg.size()));
  out += msg;
  return out;
}

// ------------------------------------------------------------------ produce

}  // namespace

std::int64_t kafka_produzir(const std::string& topico, const std::string& valor,
                            std::int32_t particao) {
  if (particao < 0) die("particao deve ser >= 0");

  Metadata md;
  const BrokerAddr addr = lider_addr(topico, particao, md);
  Conn conn(addr.host, addr.port);

  const std::string message_set = encode_message(valor);

  std::string payload;
  put_i16(payload, 1);     // required_acks = 1 (espera o lider gravar)
  put_i32(payload, 5000);  // timeout ms
  put_i32(payload, 1);     // [topic_data]
  put_str(payload, topico);
  put_i32(payload, 1);  // [data]
  put_i32(payload, particao);
  put_i32(payload, static_cast<std::int32_t>(message_set.size()));
  payload += message_set;

  const std::string resp = roundtrip(conn, 0, 1, kClientIdCorrBase + 2, payload);
  Reader r{resp};
  const std::int32_t nresp = r.i32();
  for (std::int32_t i = 0; i < nresp; ++i) {
    const std::string nome = r.str();
    const std::int32_t np = r.i32();
    for (std::int32_t p = 0; p < np; ++p) {
      const std::int32_t part = r.i32();
      const std::int16_t erro = r.i16();
      const std::int64_t offset = r.i64();  // presente a partir da v1
      if (nome == topico && part == particao) {
        if (erro != 0) die_code("produce no topico '" + topico + "'", erro);
        return offset;
      }
    }
  }
  die("resposta de produce sem a particao " + std::to_string(particao));
}

Value kafka_ler(const std::string& topico, bool do_fim, std::int64_t max) {
  if (max < 0) die("max deve ser >= 0");

  Metadata md;
  const BrokerAddr addr = lider_addr(topico, 0, md);
  Conn conn(addr.host, addr.port);

  std::string payload;
  put_i32(payload, -1);   // replica_id: cliente normal
  put_i32(payload, 100);  // max_wait ms
  put_i32(payload, 1);    // min_bytes
  put_i32(payload, 1);    // [topics]
  put_str(payload, topico);
  put_i32(payload, 1);  // [partitions]
  put_i32(payload, 0);  // particao 0
  // 0.9-era: offset -1 = "latest" (high watermark), 0 = earliest.
  put_i64(payload, do_fim ? -1 : 0);
  put_i32(payload, 16 * 1024 * 1024);  // max_bytes

  const std::string resp = roundtrip(conn, 1, 1, kClientIdCorrBase + 3, payload);
  Reader r{resp};
  const std::int32_t nresp = r.i32();
  for (std::int32_t i = 0; i < nresp; ++i) {
    const std::string nome = r.str();
    const std::int32_t np = r.i32();
    for (std::int32_t p = 0; p < np; ++p) {
      const std::int32_t part = r.i32();
      const std::int16_t erro = r.i16();
      r.i64();  // high_watermark
      std::string message_set = r.bytes();
      if (nome == topico && part == 0) {
        if (erro != 0) die_code("fetch no topico '" + topico + "'", erro);

        Value out = Value::lista();
        Reader ms{message_set};
        while (max > 0 && static_cast<std::int64_t>(out.list->size()) < max &&
               ms.pos + 12 <= ms.d.size()) {
          ms.i64();  // offset
          const std::int32_t size = ms.i32();
          if (size < 14 || ms.pos + static_cast<std::size_t>(size) > ms.d.size()) {
            die("message set malformado na resposta de fetch");
          }
          Reader msg{ms.d};
          msg.pos = ms.pos;
          msg.i32();  // crc (ignorado na leitura)
          const std::int8_t magic = msg.i8();
          if (magic != 0 && magic != 1) {
            die("magic byte inesperado no message set: " + std::to_string(magic));
          }
          msg.i8();                   // attributes
          if (magic == 1) msg.i64();  // timestamp (message v1)
          msg.bytes();                // key
          const std::string valor = msg.bytes();
          out.list->push_back(Value::texto(valor));
          ms.pos += static_cast<std::size_t>(size);
        }
        return out;
      }
    }
  }
  die("resposta de fetch sem a particao 0 do topico '" + topico + "'");
}

}  // namespace tilt::rt
