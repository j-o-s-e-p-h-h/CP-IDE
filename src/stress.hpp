// Stress testing: loop { gen.py -> input; brute.py -> expected; solution -> got; diff }.
#pragma once
#include <atomic>
#include <functional>
#include <thread>
#include "runner.hpp"

struct StressEvent {
  std::string state;  // running | found | error | stopped
  int iteration = 0;
  std::string input, expected, got, message;
};

class StressRunner {
 public:
  ~StressRunner() { stop(); }
  void start(const Toolchain& tc, const fs::path& dir, const std::string& lang, double timeLimitSec,
             std::function<void(const StressEvent&)> cb);
  void stop();
  bool running() const { return running_; }

 private:
  std::atomic<bool> running_{false}, cancel_{false};
  std::thread thread_;
};
