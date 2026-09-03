#include "runtime/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/epoll.h>
#endif

#include "runtime/arena.hpp"

namespace tilt::rt {

namespace {

constexpr std::size_t kMaxRequest = 1 << 20;   // 1 MiB por requisicao
constexpr std::size_t kArenaCap = 1 << 16;     // 64 KiB de scratch por conexao
constexpr int kBacklog = 128;
constexpr int kMaxConns = 256;
constexpr int kIdleTimeoutSec = 30;
constexpr int kMaxEvents = 64;

const char* status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
  }
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Copies `text` into the arena. Returns nullptr (caller falls back to the
// heap) when the arena is out of space.
const char* arena_copy(TiltArena& arena, const char* data, std::size_t len) {
  char* dst = static_cast<char*>(arena.alocar(len + 1));
  if (!dst) return nullptr;
  std::memcpy(dst, data, len);
  dst[len] = '\0';
  return dst;
}

enum class ParseResult { Complete, NeedMore, Bad };

// Parses one HTTP request out of `in` (consumed bytes are erased). Header
// keys/values are copied into the per-connection arena as parse scratch;
// the arena is reset by the caller once the request is dispatched.
ParseResult parse_request(std::string& in, TiltArena& arena, HttpRequest& req) {
  const std::size_t header_end = in.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return in.size() > kMaxRequest ? ParseResult::Bad : ParseResult::NeedMore;
  }

  const std::string head = in.substr(0, header_end);
  const std::size_t l1 = head.find("\r\n");
  const std::string request_line = head.substr(0, l1 == std::string::npos ? head.size() : l1);

  const std::size_t sp1 = request_line.find(' ');
  const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return ParseResult::Bad;

  std::string version = request_line.substr(sp2 + 1);
  const std::string method = upper(request_line.substr(0, sp1));
  const std::string path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

  // Headers: track Connection (case-insensitive) and Content-Length.
  std::size_t content_length = 0;
  bool connection_close = false;
  bool connection_keep_alive = false;

  std::size_t pos = l1 == std::string::npos ? head.size() : l1 + 2;
  while (pos < head.size()) {
    const std::size_t eol = head.find("\r\n", pos);
    const std::size_t line_end = eol == std::string::npos ? head.size() : eol;
    const std::size_t colon = head.find(':', pos);
    if (colon != std::string::npos && colon < line_end) {
      // Parse scratch vive na arena; copia em heap so se a arena estiver cheia.
      const char* key = arena_copy(arena, head.data() + pos, colon - pos);
      std::size_t vstart = colon + 1;
      while (vstart < line_end && head[vstart] == ' ') ++vstart;
      const char* value = arena_copy(arena, head.data() + vstart, line_end - vstart);
      if (key && value) {
        std::string k(key), v(value);
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (k == "content-length") {
          content_length = static_cast<std::size_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "connection") {
          for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (v == "close") connection_close = true;
          if (v == "keep-alive") connection_keep_alive = true;
        }
      }
    }
    if (eol == std::string::npos) break;
    pos = eol + 2;
  }

  const std::size_t total = header_end + 4 + content_length;
  if (total > kMaxRequest) return ParseResult::Bad;
  if (in.size() < total) return ParseResult::NeedMore;

  req.method = method;
  req.path = path;
  req.body = in.substr(header_end + 4, content_length);
  in.erase(0, total);

  // HTTP/1.1 mantem a conexao por padrao; HTTP/1.0 so com keep-alive explicito.
  const bool http11 = version != "HTTP/1.0";
  req.keep_alive = http11 ? !connection_close : connection_keep_alive;
  return ParseResult::Complete;
}

std::string build_response(const HttpResponse& resp, bool keep_alive) {
  std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) + "\r\n";
  out += "Content-Type: " + resp.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
  out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
  out += resp.body;
  return out;
}

#if defined(__linux__)

struct Conn {
  explicit Conn(int f) : fd(f), arena(kArenaCap) {}
  int fd;
  std::string in;    // bytes ainda nao consumidos (inclui request pipeline)
  std::string out;   // resposta aguardando envio
  bool close_after = false;   // fecha quando `out` esvaziar
  bool peer_eof = false;      // recv() retornou 0; so falta drenar `out`
  bool writing = false;       // EPOLLOUT registrado
  std::uint64_t last_active;  // epoch seconds
  TiltArena arena;
};

std::uint64_t now_sec() {
  return static_cast<std::uint64_t>(std::time(nullptr));
}

int set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int run_epoll(int listen_fd, const std::function<HttpResponse(const HttpRequest&)>& handler,
              int max_requests, std::string& fatal) {
  const int ep = epoll_create1(0);
  if (ep < 0) {
    fatal = "epoll_create1() falhou";
    return -1;
  }

  set_nonblocking(listen_fd);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd;
  if (epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
    fatal = "epoll_ctl(listen) falhou";
    ::close(ep);
    return -1;
  }

  std::unordered_map<int, std::unique_ptr<Conn>> conns;
  int served = 0;
  bool stopping = false;

  auto close_conn = [&](int fd) {
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    conns.erase(fd);
  };

  // Cota atingida: para de aceitar, fecha conexoes ociosas e marca as demais
  // para fechar assim que a resposta pendente terminar de ser enviada.
  auto begin_shutdown = [&]() {
    if (stopping) return;
    stopping = true;
    std::vector<int> droppable;
    for (const auto& [fd, c] : conns) {
      c->close_after = true;
      if (c->out.empty()) droppable.push_back(fd);
    }
    for (int fd : droppable) close_conn(fd);
  };

  // Enfileira a resposta; registra EPOLLOUT se a escrita nao completou.
  auto queue_out = [&](Conn& c, int ep) {
    if (c.out.empty()) {
      if (c.close_after || c.peer_eof) close_conn(c.fd);
      return;
    }
    if (!c.writing) {
      epoll_event wev{};
      wev.events = EPOLLIN | EPOLLOUT;
      wev.data.fd = c.fd;
      if (epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &wev) == 0) c.writing = true;
    }
  };

  auto drain_out = [&](Conn& c, int ep) {
    while (!c.out.empty()) {
      const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
      if (n > 0) {
        c.out.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EAGAIN) return;  // EPOLLOUT avisa quando der
      close_conn(c.fd);                      // erro: desiste da conexao
      return;
    }
    c.writing = false;
    if (c.close_after || c.peer_eof) {
      close_conn(c.fd);
    } else {
      epoll_event wev{};
      wev.events = EPOLLIN;
      wev.data.fd = c.fd;
      epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &wev);
    }
  };

  // Le tudo que esta pronto e consome requests completos do buffer.
  auto service_conn = [&](Conn& c, int ep) {
    char chunk[8192];
    while (true) {
      const ssize_t n = ::recv(c.fd, chunk, sizeof(chunk), 0);
      if (n > 0) {
        c.in.append(chunk, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) c.peer_eof = true;
      else if (errno != EAGAIN) {
        close_conn(c.fd);
        return;
      }
      break;
    }

    while (true) {
      HttpRequest req;
      c.arena.resetar();
      const ParseResult r = parse_request(c.in, c.arena, req);
      if (r == ParseResult::NeedMore) break;
      if (r == ParseResult::Bad) {
        HttpResponse bad;
        bad.status = 400;
        bad.body = R"({"erro":"requisicao malformada"})";
        c.out += build_response(bad, false);
        c.close_after = true;
        c.arena.resetar();
        queue_out(c, ep);
        return;
      }

      HttpResponse resp;
      try {
        resp = handler(req);
      } catch (...) {
        resp.status = 500;
        resp.body = R"({"erro":"falha interna"})";
      }
      const bool keep = req.keep_alive && (max_requests <= 0 || served + 1 < max_requests);
      c.out += build_response(resp, keep);
      c.close_after = !keep;
      ++served;
      c.arena.resetar();  // liberacao instantanea do scratch da requisicao
      if (c.close_after) break;
    }
    queue_out(c, ep);
  };

  epoll_event events[kMaxEvents];
  while (true) {
    if (stopping && conns.empty()) break;
    const int n = epoll_wait(ep, events, kMaxEvents, 1000);
    if (n < 0) {
      if (errno == EINTR) continue;
      fatal = "epoll_wait() falhou";
      break;
    }
    if (n == 0) {  // timeout: varre conexoes ociosas
      const std::uint64_t now = now_sec();
      std::vector<int> stale;
      for (const auto& [fd, c] : conns) {
        if (c->out.empty() && now - c->last_active > static_cast<std::uint64_t>(kIdleTimeoutSec)) {
          stale.push_back(fd);
        }
      }
      for (int fd : stale) close_conn(fd);
      if (max_requests > 0 && served >= max_requests) begin_shutdown();
      continue;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      if (fd == listen_fd) {
        if (stopping) continue;
        while (true) {
          const int client = ::accept(listen_fd, nullptr, nullptr);
          if (client < 0) break;  // EAGAIN: nada mais pronto
          if (conns.size() >= static_cast<std::size_t>(kMaxConns) || set_nonblocking(client) != 0) {
            const char* busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            ::send(client, busy, std::strlen(busy), MSG_NOSIGNAL);
            ::close(client);
            continue;
          }
          epoll_event cev{};
          cev.events = EPOLLIN;
          cev.data.fd = client;
          if (epoll_ctl(ep, EPOLL_CTL_ADD, client, &cev) != 0) {
            ::close(client);
            continue;
          }
          auto c = std::make_unique<Conn>(client);
          c->last_active = now_sec();
          conns.emplace(client, std::move(c));
        }
        if (max_requests > 0 && served >= max_requests) begin_shutdown();
        continue;
      }

      auto it = conns.find(fd);
      if (it == conns.end()) continue;
      Conn& c = *it->second;
      c.last_active = now_sec();

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        close_conn(fd);
        continue;
      }
      if (events[i].events & EPOLLOUT) drain_out(c, ep);
      it = conns.find(fd);
      if (it == conns.end()) continue;
      if (events[i].events & EPOLLIN && !it->second->peer_eof) service_conn(*it->second, ep);
      if (max_requests > 0 && served >= max_requests) begin_shutdown();
    }
  }

  for (const auto& [fd, c] : conns) {
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
  }
  ::close(ep);
  if (!fatal.empty()) return -1;
  return served;
}

#else  // !__linux__ : fallback bloqueante, uma conexao por vez

int run_blocking(int listen_fd, const std::function<HttpResponse(const HttpRequest&)>& handler,
                 int max_requests, std::string& fatal) {
  (void)fatal;
  int served = 0;
  while (max_requests <= 0 || served < max_requests) {
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }

    std::string in;
    std::string out;
    TiltArena arena(kArenaCap);
    bool alive = true;
    while (alive && (max_requests <= 0 || served < max_requests)) {
      HttpRequest req;
      arena.resetar();
      ParseResult r;
      while ((r = parse_request(in, arena, req)) == ParseResult::NeedMore) {
        char chunk[4096];
        const ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) {
          alive = false;
          break;
        }
        in.append(chunk, static_cast<std::size_t>(n));
      }
      if (!alive) break;
      if (r == ParseResult::Bad) {
        HttpResponse bad;
        bad.status = 400;
        bad.body = R"({"erro":"requisicao malformada"})";
        out += build_response(bad, false);
        alive = false;
      } else {
        HttpResponse resp;
        try {
          resp = handler(req);
        } catch (...) {
          resp.status = 500;
          resp.body = R"({"erro":"falha interna"})";
        }
        out += build_response(resp, req.keep_alive);
        alive = req.keep_alive && (max_requests <= 0 || served + 1 < max_requests);
      }
      arena.resetar();
      ++served;

      while (!out.empty()) {
        const ssize_t n = ::send(client, out.data(), out.size(), 0);
        if (n <= 0) {
          alive = false;
          break;
        }
        out.erase(0, static_cast<std::size_t>(n));
      }
    }
    ::close(client);
  }
  return served;
}

#endif

}  // namespace

HttpServer::~HttpServer() {
  if (fd_ >= 0) ::close(fd_);
}

std::string HttpServer::listen_on(const std::string& host, int port) {
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) return "socket() falhou";

  int on = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    return "endereco invalido: " + host;
  }
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    return "bind() falhou na porta " + std::to_string(port) + " (" + std::strerror(errno) + ")";
  }
  if (::listen(fd_, kBacklog) != 0) return "listen() falhou";
  return "";
}

int HttpServer::run(const std::function<HttpResponse(const HttpRequest&)>& handler, int max_requests) {
#if defined(__linux__)
  return run_epoll(fd_, handler, max_requests, last_error_);
#else
  return run_blocking(fd_, handler, max_requests, last_error_);
#endif
}

}  // namespace tilt::rt
