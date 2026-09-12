#include "debugger.hpp"
#include <regex>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket ::close
#endif

Debugger::~Debugger() { stop(); }

void Debugger::stop() {
  if (!running_ && !proc_) return;
  running_ = false;
  if (lang_ == "cpp" && proc_ && proc_->running()) {
    gdbSend("-gdb-exit");
    proc_->wait(500);
  }
  if (sock_ != ~(uintptr_t)0) {
    closesocket((SOCKET)sock_);
    sock_ = ~(uintptr_t)0;
  }
  if (listenSock_ != ~(uintptr_t)0) {
    closesocket((SOCKET)listenSock_);
    listenSock_ = ~(uintptr_t)0;
  }
  if (proc_) proc_->kill();
  if (reader_.joinable()) reader_.join();
  proc_.reset();
}

bool Debugger::start(const Toolchain& tc, const std::string& lang, const fs::path& dir, const fs::path& toolsDir,
                     const std::vector<int>& bps, const std::string& stdinData, DebugEventCb cb, std::string& err) {
  stop();
  lang_ = lang;
  cb_ = std::move(cb);
  proc_ = std::make_unique<Process>();
  running_ = true;
  if (lang == "python") {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, (sockaddr*)&addr, sizeof addr) != 0 || listen(ls, 1) != 0) {
      err = "Could not open a local socket for the debugger";
      closesocket(ls);
      running_ = false;
      return false;
    }
    socklen_t alen = sizeof addr;
    getsockname(ls, (sockaddr*)&addr, &alen);
    int port = ntohs(addr.sin_port);
    listenSock_ = (uintptr_t)ls;
    std::string bpList;
    for (int b : bps) bpList += (bpList.empty() ? "" : ",") + std::to_string(b);
    if (bpList.empty()) bpList = "-";
    std::wstring cmd = tc.quote(tc.python) + L" -u " + tc.quote(util::pstr(toolsDir / "cp_debug.py")) + L" " +
                       std::to_wstring(port) + L" " + util::widen(bpList) + L" " + tc.quote(util::pstr(dir / "main.py"));
    auto self = this;
    if (!proc_->start(
            cmd, dir, [self](const std::string& s) { if (self->cb_) self->cb_({{"type", "output"}, {"text", s}}); },
            [self](const std::string& s) { if (self->cb_) self->cb_({{"type", "output"}, {"text", s}}); })) {
      err = "Could not start python: " + proc_->error();
      running_ = false;
      return false;
    }
    proc_->write(stdinData);
    proc_->closeStdin();
    // wait for the tool to connect (up to 8 s)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(ls, &fds);
    timeval tv{8, 0};
    if (select((int)ls + 1, &fds, nullptr, nullptr, &tv) <= 0) {
      err = "The Python debugger helper did not connect (is python on PATH?)";
      stop();
      return false;
    }
    SOCKET c = accept(ls, nullptr, nullptr);
    if (c == INVALID_SOCKET) {
      err = "accept() failed";
      stop();
      return false;
    }
    sock_ = (uintptr_t)c;
    reader_ = std::thread(&Debugger::pyReader, this);
    cb_({{"type", "started"}});
    return true;
  }
  // ---- C++ via gdb/MI ----
  auto cr = compileCpp(tc, dir, true);
  if (!cr.ok) {
    err = "Compile error:\n" + cr.log;
    running_ = false;
    return false;
  }
  util::writeFile(dir / "tests" / "_debug.in", stdinData);
  std::string gdb = "gdb";
  {
    fs::path guess = util::upath(tc.gpp).parent_path() / util::exeName("gdb");
    std::error_code ec;
    if (fs::exists(guess, ec)) gdb = util::pstr(guess);
  }
#ifdef __APPLE__
  err = "C++ debugging needs gdb, which macOS does not ship; the Python debugger works. (lldb support is not implemented yet.)";
  running_ = false;
  return false;
#endif
  std::wstring cmd = tc.quote(gdb) + L" -q --interpreter=mi2 " + util::widen(util::exeName("sol_debug"));
  auto self = this;
  if (!proc_->start(
          cmd, dir,
          [self](const std::string& s) {
            std::lock_guard lk(self->gmu_);
            self->gbuf_ += s;
            size_t nl;
            while ((nl = self->gbuf_.find('\n')) != std::string::npos) {
              std::string line = self->gbuf_.substr(0, nl);
              self->gbuf_.erase(0, nl + 1);
              if (!line.empty() && line.back() == '\r') line.pop_back();
              self->gdbLine(line);
            }
          },
          [self](const std::string& s) { if (self->cb_) self->cb_({{"type", "output"}, {"text", s}}); })) {
    err = "Could not start gdb: " + proc_->error();
    running_ = false;
    return false;
  }
  gdbSend("-gdb-set confirm off");
  gdbSend("-gdb-set pagination off");
  gdbSend("-gdb-set new-console off");
  bool any = false;
  for (int b : bps) {
    gdbSend("-break-insert main.cpp:" + std::to_string(b));
    any = true;
  }
  if (!any) gdbSend("-break-insert main");
  gdbSend("-exec-arguments < tests/_debug.in");
  gdbSend("-exec-run");
  cb_({{"type", "started"}});
  return true;
}

void Debugger::command(const std::string& cmd) {
  if (!running_) return;
  if (cmd == "stop") {
    stop();
    if (cb_) cb_({{"type", "exited"}, {"code", -1}});
    return;
  }
  if (lang_ == "python") {
    pySend(json({{"cmd", cmd}}).dump());
    return;
  }
  if (cmd == "continue") gdbSend("-exec-continue");
  else if (cmd == "next") gdbSend("-exec-next");
  else if (cmd == "step") gdbSend("-exec-step");
  else if (cmd == "out") gdbSend("-exec-finish");
}

// ---------------- python ----------------
void Debugger::pySend(const std::string& line) {
  if (sock_ == ~(uintptr_t)0) return;
  std::string l = line + "\n";
  send((SOCKET)sock_, l.data(), (int)l.size(), 0);
}

void Debugger::pyReader() {
  std::string buf;
  char tmp[4096];
  while (running_) {
    int n = recv((SOCKET)sock_, tmp, sizeof tmp, 0);
    if (n <= 0) break;
    buf.append(tmp, n);
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      auto j = json::parse(line, nullptr, false);
      if (j.is_discarded()) continue;
      std::string ev = j.value("event", "");
      if (ev == "paused") {
        cb_({{"type", "paused"}, {"line", j.value("line", 0)}, {"vars", j.value("vars", json::array())}, {"stack", j.value("stack", json::array())}});
      } else if (ev == "exited") {
        running_ = false;
        cb_({{"type", "exited"}, {"code", j.value("code", 0)}});
      } else if (ev == "exception") {
        cb_({{"type", "output"}, {"text", j.value("message", "") + "\n"}});
      }
    }
  }
  if (running_) {
    running_ = false;
    if (cb_) cb_({{"type", "exited"}, {"code", 0}});
  }
}

// ---------------- gdb/MI ----------------
void Debugger::gdbSend(const std::string& cmd) {
  if (!proc_) return;
  proc_->write(cmd + "\n");
}

namespace {
// Extracts `key="value"` occurrences (handles \" escapes) from an MI record.
std::vector<std::pair<std::string, std::string>> miPairs(const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t eq = s.find("=\"", i);
    if (eq == std::string::npos) break;
    size_t ks = eq;
    while (ks > 0 && (isalnum((unsigned char)s[ks - 1]) || s[ks - 1] == '-' || s[ks - 1] == '_')) --ks;
    std::string key = s.substr(ks, eq - ks);
    std::string val;
    size_t j = eq + 2;
    while (j < s.size() && s[j] != '"') {
      if (s[j] == '\\' && j + 1 < s.size()) {
        char c = s[j + 1];
        val += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        j += 2;
      } else val += s[j++];
    }
    out.push_back({key, val});
    i = j + 1;
  }
  return out;
}
}  // namespace

void Debugger::gdbLine(const std::string& line) {
  if (line.empty() || line == "(gdb)" || line == "(gdb) ") return;
  char c = line[0];
  size_t p = 0;
  while (p < line.size() && isdigit((unsigned char)line[p])) ++p;
  bool hasToken = p > 0 && p < line.size() && (line[p] == '^' || line[p] == '*');
  int token = hasToken ? atoi(line.substr(0, p).c_str()) : 0;
  std::string rec = hasToken ? line.substr(p) : line;
  c = rec[0];
  if (c == '~' || c == '&' || c == '@') {
    // console stream output: "~\"text\"" -> show gdb's own messages only for errors
    if (c == '@') cb_({{"type", "output"}, {"text", miPairs("x=" + rec.substr(1)).empty() ? "" : miPairs("x=" + rec.substr(1))[0].second}});
    return;
  }
  if (c == '=') return;  // notify async
  if (c == '*') {
    if (util::startsWith(rec, "*stopped")) {
      auto pairs = miPairs(rec);
      std::string reason, lineNo;
      for (auto& [k, v] : pairs) {
        if (k == "reason" && reason.empty()) reason = v;
        if (k == "line" && lineNo.empty()) lineNo = v;
      }
      if (util::startsWith(reason, "exited")) {
        int code = 0;
        for (auto& [k, v] : pairs) if (k == "exit-code") code = (int)strtol(v.c_str(), nullptr, 8);
        running_ = false;
        gdbSend("-gdb-exit");
        cb_({{"type", "exited"}, {"code", code}});
        return;
      }
      curLine_ = atoi(lineNo.c_str());
      pendingVars_ = json::array();
      pendingStack_ = json::array();
      gdbSend("1001-stack-list-variables --simple-values");
      gdbSend("1002-stack-list-frames");
    }
    return;
  }
  if (c == '^') {
    if (util::startsWith(rec, "^error")) {
      auto pairs = miPairs(rec);
      std::string msg = pairs.empty() ? rec : pairs[0].second;
      cb_({{"type", "output"}, {"text", "gdb: " + msg + "\n"}});
      if (util::contains(msg, "not being run") || util::contains(msg, "No executable")) {
        running_ = false;
        cb_({{"type", "exited"}, {"code", -1}});
      }
      return;
    }
    if (token == 1001) {
      // variables=[{name="n",type="int",value="6"},...]
      json vars = json::array();
      size_t pos = 0;
      while ((pos = rec.find('{', pos)) != std::string::npos) {
        size_t end = rec.find('}', pos);
        if (end == std::string::npos) break;
        std::string k, v;
        for (auto& [kk, vv] : miPairs(rec.substr(pos, end - pos))) {
          if (kk == "name") k = vv;
          if (kk == "value") v = vv;
        }
        if (!k.empty()) vars.push_back({{"k", k}, {"v", v.empty() ? "…" : v}});
        pos = end + 1;
      }
      pendingVars_ = vars;
      return;
    }
    if (token == 1002) {
      json stack = json::array();
      size_t pos = 0;
      while ((pos = rec.find("frame={", pos)) != std::string::npos) {
        size_t end = rec.find('}', pos);
        if (end == std::string::npos) break;
        std::string fn, ln;
        for (auto& [kk, vv] : miPairs(rec.substr(pos, end - pos))) {
          if (kk == "func") fn = vv;
          if (kk == "line") ln = vv;
        }
        if (!fn.empty()) stack.push_back({{"fn", fn + "()"}, {"line", ln}});
        pos = end + 1;
      }
      pendingStack_ = stack;
      cb_({{"type", "paused"}, {"line", curLine_}, {"vars", pendingVars_}, {"stack", pendingStack_}});
      return;
    }
    return;
  }
  // Anything else is the debugged program's own stdout.
  cb_({{"type", "output"}, {"text", line + "\n"}});
}
