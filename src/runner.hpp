// Process spawning (pipes, timeouts, kill) and running solutions against tests.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "model.hpp"

// A child process with piped stdin/stdout/stderr. Output is delivered on
// reader threads through the callbacks given to start().
class Process {
 public:
  Process() = default;
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  bool start(const std::wstring& cmdline, const fs::path& cwd,
             std::function<void(const std::string&)> onStdout,
             std::function<void(const std::string&)> onStderr);
  void write(const std::string& data);
  void closeStdin();
  // Waits up to timeoutMs (-1 = forever). Returns true when the process exited.
  bool wait(int timeoutMs);
  int exitCode();
  void kill();
  bool running();
  long pid() const { return pid_; }
  std::string error() const { return error_; }

 private:
#ifdef _WIN32
  void* hProc_ = nullptr;
  void* hJob_ = nullptr;
  void* hStdinW_ = nullptr;
#else
  int pid_fd_ = -1;      // stdin pipe (write end)
  bool exited_ = false;
  int status_ = 0;
#endif
  long pid_ = 0;
  std::thread tOut_, tErr_;
  std::mutex wmu_;
  std::string error_;
  void joinReaders();
};

struct ProcResult {
  bool started = false;
  bool timedOut = false;
  int exitCode = 0;
  int ms = 0;
  std::string out, err, error;
};

// Runs a command to completion, feeding stdinData, killing it after timeoutMs.
ProcResult runProcess(const std::wstring& cmdline, const fs::path& cwd, const std::string& stdinData, int timeoutMs,
                      std::atomic<bool>* cancel = nullptr);

struct Toolchain {
  std::string python = "python";
  std::string gpp = "g++";  // the chosen C++ compiler (g++ or clang++, any path); name kept for history
  std::string cppFlags = "-O2 -std=c++23";
  std::vector<std::string> cppCandidates;  // every C++ compiler found on the machine (for the setup page)
  std::string java = "java";
  std::string javac = "javac";
  std::string node = "node";
  static Toolchain fromConfig(const json& cfg);
  static bool needsCompile(const std::string& lang) { return lang == "cpp" || lang == "java"; }
  std::wstring quote(const std::string& s) const;
  // Command line to run a solution file in `dir`.
  std::wstring runCommand(const std::string& lang, const fs::path& dir, const std::string& exeName = "") const;
  std::wstring pythonCommand(const fs::path& script) const;
};

struct CompileResult {
  bool ok = true;
  std::string log;
  int ms = 0;
};

// Compiles main.cpp in dir into sol.exe (or debug build sol_debug.exe with -g -O0).
CompileResult compileCpp(const Toolchain& tc, const fs::path& dir, bool debug = false, std::atomic<bool>* cancel = nullptr);
// javac Main.java -> Main.class in dir.
CompileResult compileJava(const Toolchain& tc, const fs::path& dir, std::atomic<bool>* cancel = nullptr);
// Dispatches on language; languages without a compile step return ok.
CompileResult compileFor(const Toolchain& tc, const std::string& lang, const fs::path& dir, std::atomic<bool>* cancel = nullptr);

struct TestVerdict {
  std::string status;  // pass | fail | tle | re
  std::string got;
  int ms = 0;
};

// Runs one test. Compilation must have happened already for C++.
TestVerdict runTest(const Toolchain& tc, const std::string& lang, const fs::path& dir, const TestCase& t,
                    double timeLimitSec, std::atomic<bool>* cancel = nullptr);
