#include "runner.hpp"
#include <chrono>

#ifdef _WIN32
// ============================================================ Windows process
namespace {

struct Pipe {
  HANDLE r = nullptr, w = nullptr;
  bool create(bool inheritRead, bool inheritWrite) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&r, &w, &sa, 1 << 16)) return false;
    if (!inheritRead) SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    if (!inheritWrite) SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0);
    return true;
  }
};

void readerLoop(HANDLE h, std::function<void(const std::string&)> cb) {
  char buf[1 << 14];
  DWORD n = 0;
  while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
    if (cb) cb(std::string(buf, n));
  }
  CloseHandle(h);
}

}  // namespace

Process::~Process() {
  kill();
  joinReaders();
  if (hStdinW_) CloseHandle(hStdinW_);
  if (hProc_) CloseHandle(hProc_);
  if (hJob_) CloseHandle(hJob_);
}

void Process::joinReaders() {
  if (tOut_.joinable()) tOut_.join();
  if (tErr_.joinable()) tErr_.join();
}

// Inheritable pipe ends exist between CreatePipe and the CloseHandle after CreateProcessW;
// a child started concurrently on another thread would inherit them and keep our reader
// threads waiting for EOF until it exits. One process start at a time avoids that.
static std::mutex g_spawnMutex;

bool Process::start(const std::wstring& cmdline, const fs::path& cwd,
                    std::function<void(const std::string&)> onStdout,
                    std::function<void(const std::string&)> onStderr) {
  std::lock_guard spawnLock(g_spawnMutex);
  // Children inherit this: no "program has stopped working" box, no hard-error
  // dialog waiting on a user who is looking at the IDE, not at a hidden window.
  static const bool errorModeSet = [] {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    return true;
  }();
  (void)errorModeSet;
  Pipe in, out, err;
  if (!in.create(true, false) || !out.create(false, true) || !err.create(false, true)) {
    error_ = "CreatePipe failed";
    return false;
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in.r;
  si.hStdOutput = out.w;
  si.hStdError = err.w;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = cmdline;  // CreateProcessW may modify the buffer
  std::wstring wcwd = cwd.wstring();
  // Job object so that killing the process also kills any children.
  // DIE_ON_UNHANDLED_EXCEPTION matters as much: without it a solution that segfaults
  // or aborts is held by Windows Error Reporting while WerFault collects a dump, so
  // the crash looks like a hang and gets reported as TLE instead of the real error.
  hJob_ = CreateJobObjectW(nullptr, nullptr);
  if (hJob_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(hJob_, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
  }
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr,
                           wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  CloseHandle(in.r);
  CloseHandle(out.w);
  CloseHandle(err.w);
  if (!ok) {
    DWORD e = GetLastError();
    error_ = "CreateProcess failed (error " + std::to_string(e) + ")";
    CloseHandle(in.w);
    CloseHandle(out.r);
    CloseHandle(err.r);
    return false;
  }
  if (hJob_) AssignProcessToJobObject(hJob_, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  hProc_ = pi.hProcess;
  pid_ = pi.dwProcessId;
  hStdinW_ = in.w;
  tOut_ = std::thread(readerLoop, out.r, std::move(onStdout));
  tErr_ = std::thread(readerLoop, err.r, std::move(onStderr));
  return true;
}

void Process::write(const std::string& data) {
  std::lock_guard lk(wmu_);
  if (!hStdinW_) return;
  size_t off = 0;
  while (off < data.size()) {
    DWORD n = 0;
    if (!WriteFile(hStdinW_, data.data() + off, (DWORD)std::min<size_t>(data.size() - off, 1 << 16), &n, nullptr)) break;
    off += n;
  }
}

void Process::closeStdin() {
  std::lock_guard lk(wmu_);
  if (hStdinW_) {
    CloseHandle(hStdinW_);
    hStdinW_ = nullptr;
  }
}

bool Process::wait(int timeoutMs) {
  if (!hProc_) return true;
  DWORD r = WaitForSingleObject(hProc_, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs);
  return r == WAIT_OBJECT_0;
}

int Process::exitCode() {
  if (!hProc_) return -1;
  DWORD code = 0;
  GetExitCodeProcess(hProc_, &code);
  return (int)code;
}

bool Process::running() {
  if (!hProc_) return false;
  return WaitForSingleObject(hProc_, 0) == WAIT_TIMEOUT;
}

void Process::kill() {
  if (hProc_ && running()) {
    if (hJob_) TerminateJobObject(hJob_, 1);
    TerminateProcess(hProc_, 1);
    WaitForSingleObject(hProc_, 2000);
  }
}

#else
// ============================================================== POSIX process
// The command line is run through /bin/sh -c in its own process group so that
// kill() takes the whole tree (python -> child, java, ...) down with it.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void readerLoop(int fd, std::function<void(const std::string&)> cb) {
  char buf[1 << 14];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) break;
    if (cb) cb(std::string(buf, (size_t)n));
  }
  ::close(fd);
}
}  // namespace

Process::~Process() {
  kill();
  joinReaders();
  closeStdin();
}

void Process::joinReaders() {
  if (tOut_.joinable()) tOut_.join();
  if (tErr_.joinable()) tErr_.join();
}

bool Process::start(const std::wstring& cmdline, const fs::path& cwd,
                    std::function<void(const std::string&)> onStdout,
                    std::function<void(const std::string&)> onStderr) {
  int in[2], out[2], err[2];
  if (pipe(in) || pipe(out) || pipe(err)) {
    error_ = "pipe() failed";
    return false;
  }
  std::string cmd = util::narrow(cmdline);
  std::string wd = util::pstr(cwd);
  pid_t pid = fork();
  if (pid < 0) {
    error_ = "fork() failed";
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(err[1], 2);
    for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) ::close(fd);
    if (!wd.empty()) (void)chdir(wd.c_str());
    execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
    _exit(127);
  }
  ::close(in[0]);
  ::close(out[1]);
  ::close(err[1]);
  pid_ = pid;
  pid_fd_ = in[1];
  exited_ = false;
  tOut_ = std::thread(readerLoop, out[0], std::move(onStdout));
  tErr_ = std::thread(readerLoop, err[0], std::move(onStderr));
  return true;
}

void Process::write(const std::string& data) {
  std::lock_guard lk(wmu_);
  if (pid_fd_ < 0) return;
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(pid_fd_, data.data() + off, data.size() - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
}

void Process::closeStdin() {
  std::lock_guard lk(wmu_);
  if (pid_fd_ >= 0) {
    ::close(pid_fd_);
    pid_fd_ = -1;
  }
}

bool Process::wait(int timeoutMs) {
  if (pid_ <= 0 || exited_) return true;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 1000000000 : timeoutMs);
  for (;;) {
    int st = 0;
    pid_t r = waitpid((pid_t)pid_, &st, WNOHANG);
    if (r == (pid_t)pid_) {
      exited_ = true;
      status_ = st;
      return true;
    }
    if (r < 0) {
      exited_ = true;
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    usleep(5000);
  }
}

int Process::exitCode() {
  if (!exited_) return -1;
  if (WIFEXITED(status_)) return WEXITSTATUS(status_);
  if (WIFSIGNALED(status_)) return 128 + WTERMSIG(status_);
  return -1;
}

bool Process::running() {
  if (pid_ <= 0 || exited_) return false;
  return !wait(0);
}

void Process::kill() {
  if (pid_ > 0 && running()) {
    ::kill(-(pid_t)pid_, SIGKILL);  // whole process group
    ::kill((pid_t)pid_, SIGKILL);
    wait(2000);
  }
}
#endif

ProcResult runProcess(const std::wstring& cmdline, const fs::path& cwd, const std::string& stdinData, int timeoutMs,
                      std::atomic<bool>* cancel) {
  ProcResult r;
  std::mutex mu;
  Process p;
  auto t0 = std::chrono::steady_clock::now();
  bool ok = p.start(
      cmdline, cwd,
      [&](const std::string& s) { std::lock_guard lk(mu); if (r.out.size() < (16u << 20)) r.out += s; },
      [&](const std::string& s) { std::lock_guard lk(mu); if (r.err.size() < (1u << 20)) r.err += s; });
  if (!ok) {
    r.error = p.error();
    return r;
  }
  r.started = true;
  // Feed stdin from a helper thread so a child that writes a lot before
  // reading cannot dead-lock us.
  std::thread feeder([&] {
    p.write(stdinData);
    p.closeStdin();
  });
  int waited = 0;
  bool exited = false;
  while (true) {
    int slice = 50;
    if (timeoutMs >= 0) slice = std::min(slice, std::max(0, timeoutMs - waited));
    if (p.wait(slice)) { exited = true; break; }
    waited += slice;
    if (timeoutMs >= 0 && waited >= timeoutMs) break;
    if (cancel && cancel->load()) break;
  }
  if (!exited) {
    r.timedOut = !(cancel && cancel->load());
    p.kill();
  }
  feeder.join();
  r.exitCode = p.exitCode();
  r.ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

Toolchain Toolchain::fromConfig(const json& cfg) {
  Toolchain t;
  t.python = cfg.value("python", "python");
  t.gpp = cfg.value("cppCompiler", cfg.value("gpp", "g++"));
  if (t.gpp.empty()) t.gpp = "g++";
  t.cppFlags = cfg.value("cppFlags", "-O2 -std=c++23");
  t.java = cfg.value("java", "java");
  t.javac = cfg.value("javac", "javac");
  t.node = cfg.value("node", "node");
  std::error_code ec;
  if (!fs::exists(util::upath(t.gpp), ec) && util::contains(t.gpp, "\\")) t.gpp = "g++";  // fall back to PATH
  return t;
}

std::wstring Toolchain::quote(const std::string& s) const {
  std::wstring w = util::widen(s);
  if (w.find(L' ') == std::wstring::npos && w.find(L'"') == std::wstring::npos) return w;
  std::wstring o = L"\"";
  for (wchar_t c : w) {
    if (c == L'"') o += L"\\\"";
    else o += c;
  }
  o += L"\"";
  return o;
}

std::wstring Toolchain::runCommand(const std::string& lang, const fs::path& dir, const std::string& exeName) const {
  if (lang == "cpp") return quote(util::pstr(dir / (exeName.empty() ? util::exeName("sol") : exeName)));
  if (lang == "java") return quote(java) + L" -Xss256m -cp " + quote(util::pstr(dir)) + L" Main";
  if (lang == "js") return quote(node) + L" --stack-size=65500 " + quote(util::pstr(dir / "main.js"));
  return quote(python) + L" " + quote(util::pstr(dir / "main.py"));
}

CompileResult compileJava(const Toolchain& tc, const fs::path& dir, std::atomic<bool>* cancel) {
  CompileResult cr;
  std::wstring cmd = tc.quote(tc.javac) + L" -encoding UTF-8 -d . Main.java";
  auto r = runProcess(cmd, dir, "", 120000, cancel);
  cr.ms = r.ms;
  if (!r.started) {
    cr.ok = false;
    cr.log = "Could not start javac: " + r.error + "\nInstall a JDK (e.g. winget install EclipseAdoptium.Temurin.21.JDK) or set \"javac\" in cp/config.json.";
    return cr;
  }
  cr.ok = r.exitCode == 0 && !r.timedOut;
  cr.log = r.err + r.out;
  if (r.timedOut) cr.log += "\n(compiler timed out)";
  return cr;
}

CompileResult compileFor(const Toolchain& tc, const std::string& lang, const fs::path& dir, std::atomic<bool>* cancel) {
  if (lang == "cpp") return compileCpp(tc, dir, false, cancel);
  if (lang == "java") return compileJava(tc, dir, cancel);
  return CompileResult{};
}

std::wstring Toolchain::pythonCommand(const fs::path& script) const {
  return quote(python) + L" " + quote(util::pstr(script));
}

CompileResult compileCpp(const Toolchain& tc, const fs::path& dir, bool debug, std::atomic<bool>* cancel) {
  CompileResult cr;
  std::wstring cmd = tc.quote(tc.gpp) + L" " + (debug ? L"-g -O0 -std=c++23" : util::widen(tc.cppFlags)) +
                     L" main.cpp -o " + util::widen(util::exeName(debug ? "sol_debug" : "sol"));
  auto r = runProcess(cmd, dir, "", 120000, cancel);
  cr.ms = r.ms;
  if (!r.started) {
    cr.ok = false;
    cr.log = "Could not start the compiler: " + r.error + "\nCommand: " + util::narrow(cmd);
    return cr;
  }
  cr.ok = r.exitCode == 0 && !r.timedOut;
  cr.log = r.err + r.out;
  if (r.timedOut) cr.log += "\n(compiler timed out)";
  return cr;
}

std::string explainVerdict(const TestVerdict& v, double timeLimitSec) {
  if (v.status == "tle") {
    char buf[64];
    snprintf(buf, sizeof buf, "did not finish within the %.10g s limit", timeLimitSec > 0 ? timeLimitSec : 1.0);
    return buf;
  }
  if (v.status != "re") return {};

  // A bare exit code says nothing; these are the ones a solution actually hits.
  struct Known { long long code; const char* meaning; };
  static const Known kKnown[] = {
      {3221225477LL, "access violation - read or wrote memory it does not own (out-of-bounds index, bad pointer)"},
      {3221225725LL, "stack overflow - recursion too deep, or a huge array declared inside a function"},
      {3221225620LL, "integer division by zero"},
      {3221226356LL, "heap corruption - wrote past the end of an allocation"},
      {139, "segmentation fault - out-of-bounds index or bad pointer"},
      {136, "floating point exception - division by zero"},
      {134, "aborted - a failed assert, or an uncaught C++ exception"},
      {137, "killed - out of memory"},
  };

  std::vector<std::string> lines;
  long long exitCode = 0;
  bool haveExit = false;
  for (auto& raw : util::split(util::replaceAll(v.got, "\r", ""), '\n')) {
    std::string l = util::rtrim(raw);
    if (util::trim(l).empty()) continue;
    std::string t = util::trim(l);
    if (util::startsWith(t, "(exit code ") && util::endsWith(t, ")")) {
      exitCode = atoll(t.c_str() + 11);
      haveExit = true;
      continue;
    }
    lines.push_back(l);
  }

  std::string msg;
  bool python = false, thrown = false;
  for (auto& l : lines) {
    if (util::startsWith(l, "Traceback (most recent call last)")) python = true;
    if (util::startsWith(l, "terminate called after throwing an instance of")) thrown = true;
  }
  if (python && !lines.empty()) {
    msg = util::trim(lines.back());  // the exception line ends a traceback
  } else if (thrown) {
    // Two lines of noise carry one fact each: the type, then the message.
    std::string type, what;
    for (auto& l : lines) {
      if (util::startsWith(l, "terminate called after throwing an instance of")) {
        auto a = l.find('\''), b = l.rfind('\'');
        if (a != std::string::npos && b > a) type = l.substr(a + 1, b - a - 1);
      }
      auto w = l.find("what():");
      if (w != std::string::npos) what = util::trim(l.substr(w + 7));
    }
    msg = !type.empty() && !what.empty() ? type + ": " + what : (!type.empty() ? type : what);
  } else {
    for (auto& l : lines) {
      std::string t = util::trim(l);
      if (t == "(no output)") continue;
      msg = t;  // last meaningful line, usually the stderr message
    }
  }

  if (haveExit) {
    for (auto& k : kKnown)
      if (k.code == exitCode || (unsigned long long)k.code == (unsigned int)exitCode) {
        msg = msg.empty() ? k.meaning : msg + "  -  " + k.meaning;
        return msg;
      }
    if (msg.empty()) msg = "exited with code " + std::to_string(exitCode);
  }
  return msg;
}

TestVerdict runTest(const Toolchain& tc, const std::string& lang, const fs::path& dir, const TestCase& t,
                    double timeLimitSec, std::atomic<bool>* cancel, bool scratchIfNoExpected) {
  TestVerdict v;
  int tl = (int)(timeLimitSec * 1000);
  if (tl <= 0) tl = 1000;
  // Give Python and interpreter startup some slack, as judges do.
  int hard = lang == "python" ? tl * 3 + 2000 : tl * 2 + 500;
  // t.pre is the part of stdin the test box does not show (a split sample's count line).
  auto r = runProcess(tc.runCommand(lang, dir), dir, t.pre + t.in, hard, cancel);
  v.ms = r.ms;
  if (!r.started) {
    v.status = "re";
    v.got = "Could not start: " + r.error;
    return v;
  }
  if (r.timedOut) {
    v.status = "tle";
    v.got = util::normalizeOutput(r.out);
    return v;
  }
  if (r.exitCode != 0) {
    v.status = "re";
    v.got = util::normalizeOutput(r.out);
    std::string e = util::trim(r.err);
    if (!e.empty()) v.got += (v.got.empty() ? "" : "\n") + e;
    v.got += "\n(exit code " + std::to_string(r.exitCode) + ")";
    return v;
  }
  std::string got = util::normalizeOutput(r.out);
  std::string exp = util::normalizeOutput(t.out);
  v.got = got;
  if (scratchIfNoExpected && util::trim(t.out).empty()) {
    // No expected output recorded: a scratch input the user wants to see the answer
    // for. Showing it as FAIL would be a lie, so report it separately.
    v.status = "out";
    if (v.got.empty()) v.got = "(no output)";
    if (!util::trim(r.err).empty()) v.got += "\n[stderr] " + util::trim(r.err);
    return v;
  }
  v.status = got == exp ? "pass" : "fail";
  if (v.status == "fail" && !util::trim(r.err).empty()) v.got += "\n[stderr] " + util::trim(r.err);
  if (v.status == "fail" && got.empty()) v.got = "(no output)";
  return v;
}
