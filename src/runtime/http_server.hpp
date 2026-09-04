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
// With `threads` > 1 route handling runs on a worker pool: the event loop
// parses and dispatches complete requests, workers invoke `handler`
// concurrently, and completions return via an eventfd; per-connection
// sequence numbers keep pipelined responses in order. The handler must be
// thread-safe when `threads` > 1.
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
  // have been served. `threads` <= 1 runs route handling serially in the
  // event-loop thread; `threads` > 1 runs it on a pool of that many
  // workers. Returns the number of requests served, or -1 on a fatal
  // error (see last_error()).
  int run(const std::function<HttpResponse(const HttpRequest&)>& handler, int max_requests,
          int threads = 1);

  const std::string& last_error() const { return last_error_; }

 private:
  int fd_ = -1;
  std::string last_error_;
};

}  // namespace tilt::rt
