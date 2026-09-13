#include "stress.hpp"

void StressRunner::stop() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void StressRunner::start(const Toolchain& tc, const fs::path& dir, const std::string& lang, double timeLimitSec,
                         std::function<void(const StressEvent&)> cb) {
  stop();
  cancel_ = false;
  running_ = true;
  thread_ = std::thread([=, this] {
    StressEvent ev;
    struct Done {  // an exception must surface as an error event, not kill the app
      StressRunner* self;
      std::function<void(const StressEvent&)> cb;
      ~Done() {
        if (std::uncaught_exceptions()) {
          StressEvent e;
          e.state = "error";
          e.message = "Stress test failed unexpectedly";
          cb(e);
        }
        self->running_ = false;
      }
    } done{this, cb};
    if (Toolchain::needsCompile(lang)) {
      ev.state = "running";
      ev.message = "compiling";
      cb(ev);
      auto cr = compileFor(tc, lang, dir, &cancel_);
      if (!cr.ok) {
        ev.state = "error";
        ev.message = "Compile error:\n" + cr.log;
        cb(ev);
        running_ = false;
        return;
      }
    }
    std::error_code ec;
    if (!fs::exists(dir / "gen.py", ec) || !fs::exists(dir / "brute.py", ec)) {
      ev.state = "error";
      ev.message = "gen.py / brute.py not found in the problem folder";
      cb(ev);
      running_ = false;
      return;
    }
    int tl = (int)(timeLimitSec * 1000);
    if (tl <= 0) tl = 1000;
    for (int it = 1; !cancel_; ++it) {
      ev = StressEvent{};
      ev.iteration = it;
      auto g = runProcess(tc.pythonCommand(dir / "gen.py"), dir, "", 10000, &cancel_);
      if (cancel_) break;
      if (!g.started || g.exitCode != 0) {
        ev.state = "error";
        ev.message = "gen.py failed:\n" + (g.started ? g.err : g.error);
        cb(ev);
        running_ = false;
        return;
      }
      std::string input = util::replaceAll(g.out, "\r\n", "\n");
      auto b = runProcess(tc.pythonCommand(dir / "brute.py"), dir, input, 20000, &cancel_);
      if (cancel_) break;
      if (!b.started || b.exitCode != 0) {
        ev.state = "error";
        ev.message = "brute.py failed:\n" + (b.started ? b.err : b.error) + "\ninput:\n" + input;
        cb(ev);
        running_ = false;
        return;
      }
      TestCase t;
      t.in = input;
      t.out = b.out;
      auto v = runTest(tc, lang, dir, t, timeLimitSec, &cancel_);
      if (cancel_) break;
      if (v.status != "pass") {
        ev.state = "found";
        ev.input = input;
        ev.expected = util::normalizeOutput(b.out);
        ev.got = v.status == "tle" ? "(time limit exceeded)" : v.got;
        ev.message = v.status == "tle" ? "Time limit exceeded" : v.status == "re" ? "Runtime error" : "Wrong answer";
        cb(ev);
        running_ = false;
        return;
      }
      ev.state = "running";
      cb(ev);
    }
    ev.state = "stopped";
    cb(ev);
    running_ = false;
  });
}
