// Real debugging: Python through tools/cp_debug.py (bdb over a local socket),
// C++ through gdb/MI. Both expose the same event stream to the UI.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "runner.hpp"

// Events: {type:"paused", line, vars:[{k,v}], stack:[{fn,line}]}
//         {type:"output", text} {type:"exited", code} {type:"error", message} {type:"started"}
using DebugEventCb = std::function<void(const json&)>;

class Debugger {
 public:
  ~Debugger();
  bool start(const Toolchain& tc, const std::string& lang, const fs::path& dir, const fs::path& toolsDir,
             const std::vector<int>& bps, const std::string& stdinData, DebugEventCb cb, std::string& err);
  void command(const std::string& cmd);  // continue | next | step | out | stop
  void stop();
  bool running() const { return running_; }

 private:
  std::atomic<bool> running_{false};
  std::string lang_;
  std::unique_ptr<Process> proc_;
  DebugEventCb cb_;
  // python
  uintptr_t listenSock_ = ~(uintptr_t)0, sock_ = ~(uintptr_t)0;
  std::thread reader_;
  void pyReader();
  void pySend(const std::string& line);
  // gdb/MI
  std::mutex gmu_;
  std::string gbuf_;
  int token_ = 1;
  json pendingVars_, pendingStack_;
  int curLine_ = 0;
  void gdbSend(const std::string& cmd);
  void gdbLine(const std::string& line);
};
