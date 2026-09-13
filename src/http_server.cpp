#include "http_server.hpp"
#include <map>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket ::close
#define SD_SEND SHUT_WR
static int WSAGetLastError() { return errno; }
#endif

namespace {

std::string mimeFor(const fs::path& p) {
  static const std::map<std::string, std::string> m = {
      {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
      {".json", "application/json"},        {".png", "image/png"},                       {".jpg", "image/jpeg"},
      {".svg", "image/svg+xml"},            {".ttf", "font/ttf"},                        {".woff", "font/woff"},
      {".woff2", "font/woff2"},             {".ico", "image/x-icon"},                    {".py", "text/plain; charset=utf-8"},
      {".txt", "text/plain; charset=utf-8"}, {".map", "application/json"}};
  auto ext = util::lower(util::pstr(p.extension()));
  auto it = m.find(ext);
  return it == m.end() ? "application/octet-stream" : it->second;
}

void sendAll(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, (int)std::min<size_t>(data.size() - off, 1 << 16), 0);
    if (n <= 0) break;
    off += (size_t)n;
  }
}

std::string response(int code, const std::string& text, const std::string& body, const std::string& mime) {
  std::string r = "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\n";
  r += "Content-Type: " + mime + "\r\n";
  r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  r += "Access-Control-Allow-Origin: *\r\n";
  r += "Access-Control-Allow-Headers: *\r\n";
  r += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  r += "Cache-Control: no-cache\r\n";
  r += "Connection: close\r\n\r\n";
  r += body;
  return r;
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
  std::string key = util::lower(name) + ":";
  for (auto& line : util::split(rawHeaders, '\n')) {
    std::string l = line;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (util::startsWith(util::lower(l), key)) return util::trim(l.substr(key.size()));
  }
  return {};
}

HttpServer::HttpServer(int port, fs::path staticRoot) : port_(port), root_(std::move(staticRoot)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(PostHandler onPost) {
  onPost_ = std::move(onPost);
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    error_ = "socket() failed";
    return false;
  }
  int reuse = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof reuse);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port_);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (sockaddr*)&addr, sizeof addr) != 0) {
    error_ = "Port " + std::to_string(port_) + " is already in use (error " + std::to_string(WSAGetLastError()) + ")";
    closesocket(s);
    return false;
  }
  if (listen(s, 64) != 0) {
    error_ = "listen() failed";
    closesocket(s);
    return false;
  }
  listenSock_ = (uintptr_t)s;
  running_ = true;
  thread_ = std::thread(&HttpServer::loop, this);
  return true;
}

void HttpServer::stop() {
  if (!running_) return;
  running_ = false;
#ifndef _WIN32
  shutdown((SOCKET)listenSock_, SHUT_RDWR);  // wakes accept() on Linux/macOS; close alone does not
#endif
  closesocket((SOCKET)listenSock_);
  if (thread_.joinable()) thread_.join();
}

void HttpServer::loop() {
  while (running_) {
    sockaddr_in ca{};
    socklen_t cl = sizeof ca;
    SOCKET c = accept((SOCKET)listenSock_, (sockaddr*)&ca, &cl);
    if (c == INVALID_SOCKET) {
      if (!running_) break;
      continue;
    }
    std::thread(&HttpServer::handle, this, (uintptr_t)c).detach();
  }
}

void HttpServer::handle(uintptr_t sk) {
  // Detached thread: an escaping exception would terminate the whole app.
  try {
    handleImpl(sk);
  } catch (...) {
    closesocket((SOCKET)sk);
  }
}

void HttpServer::handleImpl(uintptr_t sk) {
  SOCKET s = (SOCKET)sk;
#ifdef _WIN32
  DWORD tmo = 15000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
#else
  timeval tmo{15, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
#endif
  std::string buf;
  char tmp[1 << 14];
  size_t hdrEnd = std::string::npos;
  while (hdrEnd == std::string::npos) {
    int n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) { closesocket(s); return; }
    buf.append(tmp, n);
    hdrEnd = buf.find("\r\n\r\n");
    if (buf.size() > (1 << 20)) { closesocket(s); return; }
  }
  HttpRequest req;
  std::string head = buf.substr(0, hdrEnd);
  auto firstNl = head.find("\r\n");
  std::string reqLine = head.substr(0, firstNl);
  req.rawHeaders = firstNl == std::string::npos ? "" : head.substr(firstNl + 2);
  {
    auto parts = util::split(reqLine, ' ');
    if (parts.size() >= 2) {
      req.method = parts[0];
      req.path = parts[1];
    }
  }
  size_t clen = 0;
  try { clen = (size_t)std::stoull(req.header("Content-Length").empty() ? "0" : req.header("Content-Length")); } catch (...) {}
  std::string body = buf.substr(hdrEnd + 4);
  while (body.size() < clen) {
    int n = recv(s, tmp, sizeof tmp, 0);
    if (n <= 0) break;
    body.append(tmp, n);
    if (body.size() > (64u << 20)) break;
  }
  req.body = body;

  std::string out;
  if (onDev_ && util::startsWith(req.path, "/__")) {
    out = response(200, "OK", onDev_(req), "application/json");
  } else if (req.method == "OPTIONS") {
    out = response(204, "No Content", "", "text/plain");
  } else if (req.method == "POST") {
    std::string res = onPost_ ? onPost_(req) : "{}";
    out = response(200, "OK", res, "application/json");
  } else if (req.method == "GET") {
    std::string path = req.path;
    auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    if (path == "/" || path.empty()) path = "/index.html";
    // percent-decoding (bad escapes are kept literally rather than throwing)
    std::string dec;
    for (size_t i = 0; i < path.size(); ++i) {
      if (path[i] == '%' && i + 2 < path.size() && isxdigit((unsigned char)path[i + 1]) && isxdigit((unsigned char)path[i + 2])) {
        dec += (char)std::stoi(path.substr(i + 1, 2), nullptr, 16);
        i += 2;
      } else dec += path[i];
    }
    // Only plain relative paths under the ui folder: no "..", no drive/colon, no leading
    // double slash or backslash (an absolute path would replace root_ in operator/).
    std::string rel = dec.substr(1);
    bool bad = util::contains(rel, "..") || util::contains(rel, ":") || util::contains(rel, "\\") || util::startsWith(rel, "/") ||
               rel.find('\0') != std::string::npos;
    fs::path file;
    if (!bad) {
      std::error_code ec;
      file = fs::weakly_canonical(root_ / util::upath(rel), ec);
      fs::path rootc = fs::weakly_canonical(root_, ec);
      std::string f = util::pstr(file), r = util::pstr(rootc);
      if (ec || f.size() <= r.size() || f.compare(0, r.size(), r) != 0 || (f[r.size()] != '/' && f[r.size()] != '\\')) bad = true;
    }
    if (bad) {
      out = response(403, "Forbidden", "forbidden", "text/plain");
    } else {
      auto data = util::readFile(file);
      if (!data) out = response(404, "Not Found", "not found", "text/plain");
      else out = response(200, "OK", *data, mimeFor(file));
    }
  } else {
    out = response(405, "Method Not Allowed", "", "text/plain");
  }
  sendAll(s, out);
  shutdown(s, SD_SEND);
  closesocket(s);
}
