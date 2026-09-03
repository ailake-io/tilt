#pragma once

#include <functional>
#include <string>

namespace tilt::rt {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
  bool keep_alive = false;  // negotiated from the request line + Connection header
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

// HTTP/1.1 server.
//
// Linux: epoll event loop with non-blocking sockets — many concurrent
// connections (a slow client cannot stall the others), keep-alive with
// per-connection loops, non-blocking writes, idle timeout, and a TiltArena
// of scratch per request (reset right after the response is queued).
// Route handling itself runs serially in the event-loop thread; the
// interpreter is not reentrant.
//
// Other platforms: blocking fallback, one connection at a time,
// Connection: close.
class HttpServer {
 public:
  ~HttpServer();

  // Returns "" on success, otherwise an error message.
  std::string listen_on(const std::string& host, int port);

  // Runs the accept/serve loop; `handler` is invoked once per complete
  // request and must not throw. Stops once `max_requests` (> 0) requests
  // have been served. Returns the number of requests served, or -1 on a
  // fatal error (see last_error()).
  int run(const std::function<HttpResponse(const HttpRequest&)>& handler, int max_requests);

  const std::string& last_error() const { return last_error_; }

 private:
  int fd_ = -1;
  std::string last_error_;
};

}  // namespace tilt::rt
