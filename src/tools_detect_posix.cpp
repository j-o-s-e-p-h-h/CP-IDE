// Linux / macOS tool detection: config.json value, then PATH, then the usual folders.
#include "tools_detect.hpp"
#ifndef _WIN32
#include <algorithm>

namespace {

bool isFile(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

std::string onPath(const std::string& name) {
  for (auto& dir : util::split(util::envVar("PATH"), ':')) {
    if (dir.empty()) continue;
    fs::path p = fs::path(dir) / name;
    if (isFile(p)) return p.string();
  }
  return {};
}

// Expands one "*" component (e.g. /usr/lib/jvm/*/bin), newest name first.
std::vector<fs::path> expandGlob(const std::string& pattern) {
  std::vector<fs::path> out;
  auto star = pattern.find('*');
  if (star == std::string::npos) {
    out.push_back(pattern);
    return out;
  }
  auto slash = pattern.rfind('/', star);
  std::string parent = pattern.substr(0, slash);
  std::string rest = pattern.substr(slash + 1);
  auto tail = rest.find('/');
  std::string namePat = tail == std::string::npos ? rest : rest.substr(0, tail);
  std::string suffix = tail == std::string::npos ? "" : rest.substr(tail + 1);
  std::string pre = namePat.substr(0, namePat.find('*')), post = namePat.substr(namePat.find('*') + 1);
  std::error_code ec;
  std::vector<std::string> names;
  for (auto& e : fs::directory_iterator(parent, ec)) {
    std::string nm = e.path().filename().string();
    if (util::startsWith(nm, pre) && util::endsWith(nm, post)) names.push_back(nm);
  }
  std::sort(names.begin(), names.end(), std::greater<>());
  for (auto& nm : names) {
    fs::path p = fs::path(parent) / nm;
    if (!suffix.empty()) p /= suffix;
    out.push_back(p);
  }
  return out;
}

std::string resolve(const std::string& configured, const std::vector<std::string>& names, const std::vector<std::string>& dirs) {
  if (!configured.empty() && util::contains(configured, "/") && isFile(configured)) return configured;
  if (!configured.empty() && !util::contains(configured, "/")) {
    auto p = onPath(configured);
    if (!p.empty()) return p;
  }
  for (auto& n : names) {
    auto p = onPath(n);
    if (!p.empty()) return p;
  }
  for (auto& pat : dirs)
    for (auto& dir : expandGlob(pat))
      for (auto& n : names)
        if (isFile(dir / n)) return (dir / n).string();
  return {};
}

}  // namespace

std::vector<ToolStatus> detectToolchain(const json& config, Toolchain& tc) {
  std::string home = util::envVar("HOME");
#ifdef __APPLE__
  const bool mac = true;
#else
  const bool mac = false;
#endif
  std::vector<std::string> bins = {"/usr/local/bin", "/usr/bin", "/bin", "/opt/homebrew/bin", "/opt/local/bin", home + "/.local/bin"};
  std::vector<ToolStatus> st;
  auto add = [&](const std::string& id, const std::string& label, const std::string& path, const std::string& hint) {
    ToolStatus t;
    t.id = id;
    t.label = label;
    t.path = path;
    t.found = !path.empty();
    t.hint = hint;
    st.push_back(t);
    if (!path.empty()) util::prependPath(fs::path(path).parent_path());
  };
  std::string aptOrBrew = mac ? "brew install " : "sudo apt install ";

  tc.python = resolve(config.value("python", ""), {"python3", "python"}, bins);
  add("python", "Python", tc.python, aptOrBrew + (mac ? "python" : "python3"));

  std::vector<std::string> ccDirs = bins;
  ccDirs.push_back("/usr/lib/gcc/*/bin");
  ccDirs.push_back("/opt/homebrew/opt/llvm/bin");
  ccDirs.push_back("/usr/local/opt/llvm/bin");
  tc.cppCandidates.clear();
  auto addCandidate = [&](const std::string& p) {
    if (p.empty()) return;
    std::error_code ec;
    std::string real = fs::weakly_canonical(p, ec).string();
    for (auto& c : tc.cppCandidates) {
      std::string cr = fs::weakly_canonical(c, ec).string();
      if (c == p || (!real.empty() && cr == real)) return;
    }
    tc.cppCandidates.push_back(p);
  };
  std::string configured = config.value("cppCompiler", config.value("gpp", ""));
  if (!configured.empty() && isFile(configured)) addCandidate(configured);
  for (auto* n : {"g++", "clang++", "g++-15", "g++-14", "g++-13", "g++-12"}) {
    addCandidate(onPath(n));
    for (auto& d : ccDirs)
      for (auto& dir : expandGlob(d))
        if (isFile(dir / n)) addCandidate((dir / n).string());
  }
  tc.gpp = resolve(configured, {"g++", "clang++"}, ccDirs);
  if (tc.gpp.empty() && !tc.cppCandidates.empty()) tc.gpp = tc.cppCandidates.front();
  add("gpp", "C++ compiler (g++ / clang++)", tc.gpp, mac ? "xcode-select --install" : "sudo apt install g++");

  std::vector<std::string> jdkDirs = bins;
  std::string javaHome = util::envVar("JAVA_HOME");
  if (!javaHome.empty()) jdkDirs.insert(jdkDirs.begin(), javaHome + "/bin");
  jdkDirs.push_back("/usr/lib/jvm/*/bin");
  jdkDirs.push_back("/Library/Java/JavaVirtualMachines/*/Contents/Home/bin");
  jdkDirs.push_back("/opt/homebrew/opt/openjdk/bin");
  tc.javac = resolve(config.value("javac", ""), {"javac"}, jdkDirs);
  std::string javaNext = tc.javac.empty() ? "" : (fs::path(tc.javac).parent_path() / "java").string();
  tc.java = isFile(javaNext) ? javaNext : resolve(config.value("java", ""), {"java"}, jdkDirs);
  add("javac", "Java compiler (javac)", tc.javac, mac ? "brew install openjdk" : "sudo apt install default-jdk");
  add("java", "Java runtime (java)", tc.java, mac ? "brew install openjdk" : "sudo apt install default-jdk");

  std::vector<std::string> nodeDirs = bins;
  nodeDirs.push_back(home + "/.nvm/versions/node/*/bin");
  tc.node = resolve(config.value("node", ""), {"node", "nodejs"}, nodeDirs);
  add("node", "JavaScript (Node.js)", tc.node, mac ? "brew install node" : "sudo apt install nodejs");

  std::string gdb = resolve("", {"gdb"}, bins);
  add("gdb", "C++ debugger (gdb)", gdb, mac ? "not available on macOS (use the Python debugger)" : "sudo apt install gdb");

  if (tc.python.empty()) tc.python = "python3";
  if (tc.gpp.empty()) tc.gpp = "g++";
  if (tc.javac.empty()) tc.javac = "javac";
  if (tc.java.empty()) tc.java = "java";
  if (tc.node.empty()) tc.node = "node";
  return st;
}
#endif  // !_WIN32
