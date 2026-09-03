#include "runtime/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace tilt::rt {

namespace {

constexpr std::size_t kMaxRequest = 1 << 20;  // 1 MiB

const char* status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

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
  if (::listen(fd_, 16) != 0) return "listen() falhou";
  return "";
}

std::optional<std::pair<int, HttpRequest>> HttpServer::accept_one() {
  int client = ::accept(fd_, nullptr, nullptr);
  if (client < 0) return std::nullopt;

  std::string buf;
  char chunk[4096];
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos && buf.size() < kMaxRequest) {
    ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buf.append(chunk, static_cast<std::size_t>(n));
    header_end = buf.find("\r\n\r\n");
  }
  if (header_end == std::string::npos) {
    ::close(client);
    return std::nullopt;
  }

  HttpRequest req;
  const std::string head = buf.substr(0, header_end);
  const std::size_t l1 = head.find("\r\n");
  const std::string request_line = head.substr(0, l1 == std::string::npos ? head.size() : l1);
  {
    const std::size_t sp1 = request_line.find(' ');
    const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      req.method = upper(request_line.substr(0, sp1));
      req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    }
  }

  std::size_t content_length = 0;
  {
    std::string lower_head = head;
    for (char& c : lower_head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::size_t cl = lower_head.find("content-length:");
    if (cl != std::string::npos) {
      content_length = static_cast<std::size_t>(std::strtoul(head.c_str() + cl + 15, nullptr, 10));
    }
  }

  std::string body = buf.substr(header_end + 4);
  while (body.size() < content_length && buf.size() < kMaxRequest) {
    ssize_t n = ::recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    body.append(chunk, static_cast<std::size_t>(n));
  }
  req.body = body.substr(0, content_length);

  return std::make_pair(client, std::move(req));
}

void HttpServer::respond(int client_fd, int status, const std::string& content_type,
                         const std::string& body) {
  std::string out = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
  out += "Content-Type: " + content_type + "\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Connection: close\r\n\r\n";
  out += body;

  std::size_t sent = 0;
  while (sent < out.size()) {
    ssize_t n = ::send(client_fd, out.data() + sent, out.size() - sent, 0);
    if (n <= 0) break;
    sent += static_cast<std::size_t>(n);
  }
  ::close(client_fd);
}

}  // namespace tilt::rt
