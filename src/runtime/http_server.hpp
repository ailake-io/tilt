#pragma once

#include <optional>
#include <string>
#include <utility>

namespace tilt::rt {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

// Minimal blocking HTTP/1.1 server: one request at a time, Connection: close.
// epoll + keep-alive + per-request arenas arrive in M10.2.
class HttpServer {
 public:
  ~HttpServer();

  // Returns "" on success, otherwise an error message.
  std::string listen_on(const std::string& host, int port);

  // Blocks for the next request. Returns {client_fd, request}, or nullopt on
  // a transient accept error.
  std::optional<std::pair<int, HttpRequest>> accept_one();

  static void respond(int client_fd, int status, const std::string& content_type,
                      const std::string& body);

 private:
  int fd_ = -1;
};

}  // namespace tilt::rt
