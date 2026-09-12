// Finds python / g++ / javac / java / node: config.json value first, then PATH,
// then the usual install folders. Reports what is missing with an install hint.
#pragma once
#include <string>
#include <vector>
#include "runner.hpp"

struct ToolStatus {
  std::string id;     // python | gpp | javac | java | node | gdb
  std::string label;  // "Python", "C++ compiler (g++)", ...
  std::string path;   // resolved executable, empty when not found
  std::string hint;   // install command shown when missing
  bool found = false;
};

// Resolves every tool, fills `tc`, prepends each tool's folder to PATH (so child
// processes find their DLLs) and returns the per-tool status list.
std::vector<ToolStatus> detectToolchain(const json& config, Toolchain& tc);
