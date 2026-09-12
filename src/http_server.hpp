// Tiny blocking HTTP/1.1 server on 127.0.0.1:<port>.
//  * POST  -> handed to the Competitive Companion handler
//  * GET   -> static files from the ui/ directory (the webview loads the UI from here)
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include "util.hpp"

struct HttpRequest {
  std::string method, path, body;
  std::string header(const std::string& name) const;
  std::string rawHeaders;
};

class HttpServer {
 public:
  using PostHandler = std::function<std::string(const HttpRequest&)>;  // returns response body (JSON)
  HttpServer(int port, fs::path staticRoot);
  ~HttpServer();
  bool start(PostHandler onPost);
  // Optional handler for /__* paths (developer/test hook); any method.
  void setDevHandler(PostHandler onDev) { onDev_ = std::move(onDev); }
  void stop();
  int port() const { return port_; }
  std::string lastError() const { return error_; }

 private:
  int port_;
  fs::path root_;
  PostHandler onPost_;
  PostHandler onDev_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  uintptr_t listenSock_ = ~(uintptr_t)0;
  std::string error_;
  void loop();
  void handle(uintptr_t sock);
};
