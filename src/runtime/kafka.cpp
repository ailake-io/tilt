#include "runtime/kafka.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/tls.hpp"

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

BrokerAddr parse_addr(const std::string& addr) {
  const std::size_t colon = addr.rfind(':');
  if (colon == std::string::npos) return {addr, "9092"};
  return {addr.substr(0, colon), addr.substr(colon + 1)};
}

BrokerAddr bootstrap_addr() {
  const char* env = std::getenv("KAFKA_BOOTSTRAP");
  return parse_addr(env && *env ? env : "127.0.0.1:9092");
}

// Conexao simples: abre, usa, fecha. Bloqueante com timeout de recv/send.
class Conn {
 public:
  Conn(const std::string& host, const std::string& port, bool tls = false) {
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

    if (tls) tls_.emplace(fd_, host);
  }

  ~Conn() {
    if (fd_ >= 0) ::close(fd_);
  }

  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  void write_full(const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = send_raw(data.data() + off, data.size() - off);
      if (n <= 0) die("falha ao enviar requisicao");
      off += static_cast<std::size_t>(n);
    }
  }

  void read_full(std::size_t n, std::string& out) {
    out.resize(n);
    std::size_t off = 0;
    while (off < n) {
      const ssize_t r = recv_raw(out.data() + off, n - off);
      if (r <= 0) die("resposta incompleta do broker");
      off += static_cast<std::size_t>(r);
    }
  }

 private:
  // Primitivas de transporte: TLS quando ativo, socket cru caso contrario.
  ssize_t send_raw(const char* p, std::size_t n) {
    if (tls_) {
      tls_->write_all(p, n);
      return static_cast<ssize_t>(n);
    }
    return ::send(fd_, p, n, MSG_NOSIGNAL);
  }
  ssize_t recv_raw(char* p, std::size_t n) {
    if (tls_) return static_cast<ssize_t>(tls_->read_some(p, n));
    return ::recv(fd_, p, n, 0);
  }

  int fd_ = -1;
  std::optional<TlsStream> tls_;
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
    case 15:
      die(contexto + " falhou: CoordinatorNotAvailable (kafka codigo 15)");
    case 16:
      die(contexto + " falhou: NotCoordinatorForGroup (kafka codigo 16)");
    case 21:
      die(contexto + " falhou: IllegalGeneration (kafka codigo 21)");
    case 22:
      die(contexto + " falhou: InconsistentGroupProtocol (kafka codigo 22)");
    case 25:
      die(contexto + " falhou: UnknownMemberId (kafka codigo 25)");
    case 27:
      die(contexto + " falhou: RebalanceInProgress (kafka codigo 27)");
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

Metadata metadata(const std::string& topico, const BrokerAddr& addr, bool tls) {
  Conn conn(addr.host, addr.port, tls);
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

// Acha o endereco do lider de uma particao num metadata ja carregado.
BrokerAddr lider_addr_md(const std::string& topico, std::int32_t particao, const Metadata& md) {
  if (particao < 0 || static_cast<std::size_t>(particao) >= md.partitions.size()) {
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

// Acha o lider da particao e devolve seu endereco; conecta no bootstrap e,
// se o lider for outro broker, devolve o endereco dele.
BrokerAddr lider_addr(const std::string& topico, std::int32_t particao, Metadata& md, bool tls) {
  const BrokerAddr bootstrap = bootstrap_addr();
  md = metadata(topico, bootstrap, tls);
  return lider_addr_md(topico, particao, md);
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

// -------------------------------------------------------------- fetch interno

// Executa FetchRequest v1 para uma particao numa conexao ja aberta e devolve
// os pares (offset, valor) na ordem do log. `offset` < 0 = "latest".
std::vector<std::pair<std::int64_t, std::string>> fetch_msgs(Conn& conn, const std::string& topico,
                                                             std::int32_t particao,
                                                             std::int64_t offset,
                                                             std::int64_t max) {
  std::string payload;
  put_i32(payload, -1);   // replica_id: cliente normal
  put_i32(payload, 100);  // max_wait ms
  put_i32(payload, 1);    // min_bytes
  put_i32(payload, 1);    // [topics]
  put_str(payload, topico);
  put_i32(payload, 1);  // [partitions]
  put_i32(payload, particao);
  put_i64(payload, offset);
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
      if (nome == topico && part == particao) {
        if (erro != 0) die_code("fetch no topico '" + topico + "'", erro);
        std::vector<std::pair<std::int64_t, std::string>> out;
        Reader ms{message_set};
        while (max > 0 && static_cast<std::int64_t>(out.size()) < max &&
               ms.pos + 12 <= ms.d.size()) {
          const std::int64_t msg_offset = ms.i64();
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
          out.emplace_back(msg_offset, msg.bytes());
          ms.pos += static_cast<std::size_t>(size);
        }
        return out;
      }
    }
  }
  die("resposta de fetch sem a particao " + std::to_string(particao) + " do topico '" + topico +
      "'");
}

}  // namespace

std::int64_t kafka_produzir(const std::string& topico, const std::string& valor,
                            std::int32_t particao, bool tls) {
  if (particao < 0) die("particao deve ser >= 0");

  Metadata md;
  const BrokerAddr addr = lider_addr(topico, particao, md, tls);
  Conn conn(addr.host, addr.port, tls);

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

Value kafka_ler(const std::string& topico, bool do_fim, std::int64_t max,
                const std::string& broker, bool tls) {
  if (max < 0) die("max deve ser >= 0");

  Metadata md;
  BrokerAddr addr;
  if (broker.empty()) {
    addr = lider_addr(topico, 0, md, tls);
  } else {
    md = metadata(topico, parse_addr(broker), tls);
    addr = lider_addr_md(topico, 0, md);
  }
  Conn conn(addr.host, addr.port, tls);

  // 0.9-era: offset -1 = "latest" (high watermark), 0 = earliest.
  Value out = Value::lista();
  for (const auto& [off, valor] :
       fetch_msgs(conn, topico, 0, do_fim ? -1 : 0, max)) {
    (void)off;
    out.list->push_back(Value::texto(valor));
  }
  return out;
}

// ------------------------------------------------------- consumer group 0.9

namespace {

// MemberMetadata v0: version int16=0, [topics], user_data BYTES.
std::string member_metadata(const std::string& topico) {
  std::string m;
  put_i16(m, 0);
  put_i32(m, 1);
  put_str(m, topico);
  put_i32(m, -1);  // user_data NULL
  return m;
}

// MemberAssignment v0 -> lista de (topico, [particoes]).
std::vector<std::pair<std::string, std::vector<std::int32_t>>> parse_assignment(
    const std::string& bytes) {
  Reader a{bytes};
  a.i16();  // version
  const std::int32_t nt = a.i32();
  if (nt < 0) die("assignment do sync group malformado");
  std::vector<std::pair<std::string, std::vector<std::int32_t>>> out;
  for (std::int32_t t = 0; t < nt; ++t) {
    const std::string topico = a.str();
    const std::int32_t np = a.i32();
    if (np < 0) die("assignment do sync group malformado");
    std::vector<std::int32_t> parts;
    for (std::int32_t p = 0; p < np; ++p) parts.push_back(a.i32());
    out.emplace_back(topico, std::move(parts));
  }
  a.bytes();  // user_data
  return out;
}

// FindCoordinator (api 10, v0): group_id -> host:port do coordenador.
BrokerAddr find_coordinator(Conn& conn, const std::string& grupo) {
  std::string payload;
  put_str(payload, grupo);
  const std::string resp = roundtrip(conn, 10, 0, 10, payload);
  Reader r{resp};
  const std::int16_t erro = r.i16();
  if (erro != 0) die_code("find_coordinator do grupo '" + grupo + "'", erro);
  r.i32();  // coordinator id
  const std::string host = r.str();
  const std::int32_t port = r.i32();
  return {host, std::to_string(port)};
}

struct JoinInfo {
  std::int32_t generation = 0;
  std::string member_id;
};

// JoinGroup (api 11, v0): entra no grupo com member_id vazio e o protocolo
// "range"; devolve a geracao e o member_id atribuidos pelo coordenador.
JoinInfo join_group(Conn& conn, const std::string& grupo, const std::string& topico) {
  std::string payload;
  put_str(payload, grupo);
  put_i32(payload, 30000);  // session_timeout ms
  put_str(payload, "");     // member_id vazio: primeiro join
  put_str(payload, "consumer");
  put_i32(payload, 1);  // [protocols]
  put_str(payload, "range");
  put_bytes(payload, member_metadata(topico));

  const std::string resp = roundtrip(conn, 11, 0, 11, payload);
  Reader r{resp};
  const std::int16_t erro = r.i16();
  if (erro != 0) die_code("join_group do grupo '" + grupo + "'", erro);
  JoinInfo j;
  j.generation = r.i32();
  r.str();  // group_protocol escolhido
  r.str();  // leader_id
  j.member_id = r.str();
  const std::int32_t nm = r.i32();  // [members]
  if (nm < 0) die("resposta de join group malformada");
  for (std::int32_t i = 0; i < nm; ++i) {
    r.str();    // member_id
    r.bytes();  // metadata
  }
  if (j.member_id.empty()) die("coordenador nao atribuiu member_id no join");
  return j;
}

// Heartbeat (api 12, v0): 1 batida apos o join (sessao nao expira na janela
// de um fetch curto).
void heartbeat(Conn& conn, const std::string& grupo, std::int32_t generation,
               const std::string& member_id) {
  std::string payload;
  put_str(payload, grupo);
  put_i32(payload, generation);
  put_str(payload, member_id);
  const std::string resp = roundtrip(conn, 12, 0, 12, payload);
  Reader r{resp};
  const std::int16_t erro = r.i16();
  if (erro != 0) die_code("heartbeat do grupo '" + grupo + "'", erro);
}

// LeaveGroup (api 13, v0): melhor esforco no fim da sessao.
void leave_group(Conn& conn, const std::string& grupo, const std::string& member_id) {
  std::string payload;
  put_str(payload, grupo);
  put_str(payload, member_id);
  const std::string resp = roundtrip(conn, 13, 0, 13, payload);
  Reader r{resp};
  const std::int16_t erro = r.i16();
  if (erro != 0) die_code("leave_group do grupo '" + grupo + "'", erro);
}

// Garante LeaveGroup mesmo quando fetch/commit lanca excecao.
struct GroupSession {
  Conn& conn;
  std::string grupo;
  std::string member_id;
  ~GroupSession() {
    try {
      leave_group(conn, grupo, member_id);
    } catch (...) {
      // sessao expira sozinha pelo session_timeout
    }
  }
};

// SyncGroup (api 14, v0) com group_assignment vazio: a resposta traz o
// MemberAssignment deste membro.
std::vector<std::pair<std::string, std::vector<std::int32_t>>> sync_group(
    Conn& conn, const std::string& grupo, std::int32_t generation, const std::string& member_id) {
  std::string payload;
  put_str(payload, grupo);
  put_i32(payload, generation);
  put_str(payload, member_id);
  put_i32(payload, 0);  // group_assignment vazio
  const std::string resp = roundtrip(conn, 14, 0, 14, payload);
  Reader r{resp};
  const std::int16_t erro = r.i16();
  if (erro != 0) die_code("sync_group do grupo '" + grupo + "'", erro);
  return parse_assignment(r.bytes());
}

// OffsetFetch (api 9, v0): offsets commitados por particao (-1 se nenhum).
std::vector<std::pair<std::int32_t, std::int64_t>> offset_fetch(
    Conn& conn, const std::string& grupo,
    const std::vector<std::pair<std::string, std::vector<std::int32_t>>>& assignment) {
  std::string payload;
  put_str(payload, grupo);
  put_i32(payload, static_cast<std::int32_t>(assignment.size()));
  for (const auto& [topico, parts] : assignment) {
    put_str(payload, topico);
    put_i32(payload, static_cast<std::int32_t>(parts.size()));
    for (const std::int32_t p : parts) put_i32(payload, p);
  }
  const std::string resp = roundtrip(conn, 9, 0, 9, payload);
  Reader r{resp};
  const std::int32_t nt = r.i32();
  if (nt < 0) die("resposta de offset fetch malformada");
  std::vector<std::pair<std::int32_t, std::int64_t>> out;
  for (std::int32_t t = 0; t < nt; ++t) {
    const std::string topico = r.str();
    const std::int32_t np = r.i32();
    if (np < 0) die("resposta de offset fetch malformada");
    for (std::int32_t p = 0; p < np; ++p) {
      const std::int32_t part = r.i32();
      const std::int64_t offset = r.i64();
      r.str();                       // metadata
      const std::int16_t erro = r.i16();
      if (erro != 0) die_code("offset_fetch do grupo '" + grupo + "'", erro);
      out.emplace_back(part, offset);
    }
    (void)topico;
  }
  return out;
}

// OffsetCommit (api 8, v1): commita o offset seguinte ao ultimo lido,
// metadata "tilt".
void offset_commit(Conn& conn, const std::string& grupo, std::int32_t generation,
                   const std::string& member_id, const std::string& topico,
                   const std::vector<std::pair<std::int32_t, std::int64_t>>& commits) {
  std::string payload;
  put_str(payload, grupo);
  put_i32(payload, generation);
  put_str(payload, member_id);
  put_i32(payload, 1);  // [topics]
  put_str(payload, topico);
  put_i32(payload, static_cast<std::int32_t>(commits.size()));
  for (const auto& [part, offset] : commits) {
    put_i32(payload, part);
    put_i64(payload, offset);
    put_i64(payload, -1);  // timestamp: server time
    put_str(payload, "tilt");
  }
  const std::string resp = roundtrip(conn, 8, 1, 8, payload);
  Reader r{resp};
  const std::int32_t nt = r.i32();
  if (nt < 0) die("resposta de offset commit malformada");
  for (std::int32_t t = 0; t < nt; ++t) {
    r.str();
    const std::int32_t np = r.i32();
    if (np < 0) die("resposta de offset commit malformada");
    for (std::int32_t p = 0; p < np; ++p) {
      r.i32();
      const std::int16_t erro = r.i16();
      if (erro != 0) die_code("offset_commit do grupo '" + grupo + "'", erro);
    }
  }
}

}  // namespace

std::vector<std::pair<int, std::string>> kafka_consume_group(const std::string& broker,
                                                             const std::string& grupo,
                                                             const std::string& topico,
                                                             int max_msgs, bool tls) {
  if (max_msgs < 0) die("max deve ser >= 0");
  if (grupo.empty()) die("grupo nao pode ser vazio");

  const BrokerAddr bootstrap = broker.empty() ? bootstrap_addr() : parse_addr(broker);

  // 1. FindCoordinator no bootstrap.
  BrokerAddr coord;
  {
    Conn conn(bootstrap.host, bootstrap.port, tls);
    coord = find_coordinator(conn, grupo);
  }

  // Coordenador: join -> heartbeat -> sync -> offsets -> (fetch/commit) ->
  // leave ao sair do escopo.
  Conn conn(coord.host, coord.port, tls);
  const JoinInfo join = join_group(conn, grupo, topico);
  GroupSession sess{conn, grupo, join.member_id};
  heartbeat(conn, grupo, join.generation, join.member_id);

  const auto assignment = sync_group(conn, grupo, join.generation, join.member_id);
  if (assignment.empty()) {
    die("grupo '" + grupo + "' nao atribuiu particoes deste membro no sync");
  }

  // 4. Offsets commitados por particao.
  const auto offsets = offset_fetch(conn, grupo, assignment);

  // 5. Fetch: cada particao atribuida a partir do offset commitado (ou do
  // earliest, quando -1), sequencialmente, ate `max_msgs`.
  const Metadata md = metadata(topico, bootstrap, tls);
  std::vector<std::pair<int, std::string>> out;
  std::vector<std::pair<std::int32_t, std::int64_t>> commits;
  for (const auto& [t, parts] : assignment) {
    if (t != topico) continue;  // assignment de outro topico: ignora
    for (const std::int32_t part : parts) {
      std::int64_t inicio = 0;
      for (const auto& [p, off] : offsets) {
        if (p == part && off >= 0) inicio = off;
      }
      const std::int64_t restante = static_cast<std::int64_t>(max_msgs) -
                                    static_cast<std::int64_t>(out.size());
      if (restante <= 0) {
        commits.emplace_back(part, inicio);
        continue;
      }
      const BrokerAddr lider = lider_addr_md(topico, part, md);
      Conn fc(lider.host, lider.port, tls);
      const auto msgs = fetch_msgs(fc, topico, part, inicio, restante);
      std::int64_t proximo = inicio;
      for (const auto& [off, valor] : msgs) {
        out.emplace_back(static_cast<int>(part), valor);
        proximo = off + 1;
      }
      commits.emplace_back(part, proximo);
    }
  }

  // 6. Commit do offset seguinte ao ultimo lido em todas as particoes.
  offset_commit(conn, grupo, join.generation, join.member_id, topico, commits);
  return out;
}

}  // namespace tilt::rt
