// Windows tool detection. The POSIX version lives in tools_detect_posix.cpp.
#include "tools_detect.hpp"
#ifdef _WIN32
#include <algorithm>
#include <regex>

namespace {

bool isFile(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

// The Microsoft Store "python.exe" alias is a 0-byte reparse point that opens the Store.
bool isStoreAlias(const fs::path& p) {
  std::error_code ec;
  auto s = util::lower(util::pstr(p));
  return util::contains(s, "\\windowsapps\\") || (fs::exists(p, ec) && fs::file_size(p, ec) == 0);
}

// Searches PATH for name.exe (SearchPathW). Returns empty when absent.
std::string onPath(const std::string& name) {
  wchar_t buf[MAX_PATH * 2];
  std::wstring n = util::widen(name);
  if (n.find(L'.') == std::wstring::npos) n += L".exe";
  DWORD r = SearchPathW(nullptr, n.c_str(), nullptr, (DWORD)std::size(buf), buf, nullptr);
  if (r == 0 || r >= std::size(buf)) return {};
  return util::narrow(std::wstring(buf, r));
}

// Expands one directory pattern with a single "*" component (e.g. C:\Python3* or
// C:\Program Files\Java\jdk-*) and returns matching dirs, newest version first.
std::vector<fs::path> expandGlob(const std::string& pattern) {
  std::vector<fs::path> out;
  auto star = pattern.find('*');
  if (star == std::string::npos) {
    out.push_back(util::upath(pattern));
    return out;
  }
  auto slash = pattern.rfind('\\', star);
  std::string parent = pattern.substr(0, slash);
  std::string rest = pattern.substr(slash + 1);
  auto tail = rest.find('\\');
  std::string namePat = tail == std::string::npos ? rest : rest.substr(0, tail);
  std::string suffix = tail == std::string::npos ? "" : rest.substr(tail + 1);
  std::string pre = namePat.substr(0, namePat.find('*')), post = namePat.substr(namePat.find('*') + 1);
  std::error_code ec;
  std::vector<std::string> names;
  for (auto& e : fs::directory_iterator(util::upath(parent), ec)) {
    if (!e.is_directory()) continue;
    std::string nm = util::pstr(e.path().filename());
    if (util::startsWith(nm, pre) && util::endsWith(nm, post)) names.push_back(nm);
  }
  // natural sort descending so "Python313" beats "Python39" and "jdk-21" beats "jdk-17"
  static const std::regex digits("\\d+");
  auto key = [](const std::string& s) {
    std::vector<long long> nums;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), digits); it != std::sregex_iterator(); ++it)
      nums.push_back(std::stoll(it->str()));
    return nums;
  };
  std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) { return key(a) > key(b); });
  for (auto& nm : names) {
    fs::path p = util::upath(parent) / util::upath(nm);
    if (!suffix.empty()) p /= util::upath(suffix);
    out.push_back(p);
  }
  return out;
}

std::string env(const char* n) { return util::envVar(n); }

std::string firstExisting(const std::vector<std::string>& patterns, const std::string& exe) {
  for (auto& pat : patterns) {
    if (pat.empty() || util::startsWith(pat, "\\")) continue;
    for (auto& dir : expandGlob(pat)) {
      fs::path p = dir / exe;
      if (isFile(p)) return util::pstr(p);
    }
  }
  return {};
}

std::string resolve(const std::string& configured, const std::string& exe, const std::vector<std::string>& dirs) {
  // 1. explicit path in config.json
  if (!configured.empty() && (util::contains(configured, "\\") || util::contains(configured, "/")) && isFile(util::upath(configured)))
    return configured;
  // 2. PATH (by the configured name, then the default name)
  for (auto& name : {configured, exe}) {
    if (name.empty()) continue;
    auto p = onPath(name);
    if (!p.empty() && !isStoreAlias(util::upath(p))) return p;
  }
  // 3. usual install folders
  return firstExisting(dirs, exe);
}

void prependPathDir(const std::string& exePath) {
  if (exePath.empty()) return;
  fs::path dir = util::upath(exePath).parent_path();
  std::string cur = util::envVar("PATH");
  std::string d = util::pstr(dir);
  if (util::contains(util::lower(cur), util::lower(d) + ";")) return;
  SetEnvironmentVariableW(L"PATH", (dir.wstring() + L";" + util::widen(cur)).c_str());
}

}  // namespace

std::vector<ToolStatus> detectToolchain(const json& config, Toolchain& tc) {
  std::string pf = env("ProgramFiles"), pf86 = env("ProgramFiles(x86)"), lad = env("LOCALAPPDATA"), javaHome = env("JAVA_HOME");
  if (pf.empty()) pf = "C:\\Program Files";
  if (pf86.empty()) pf86 = "C:\\Program Files (x86)";

  std::vector<ToolStatus> st;
  auto add = [&](const std::string& id, const std::string& label, const std::string& path, const std::string& hint) {
    ToolStatus t;
    t.id = id;
    t.label = label;
    t.path = path;
    t.found = !path.empty();
    t.hint = hint;
    st.push_back(t);
    prependPathDir(path);
  };

  tc.python = resolve(config.value("python", "python"), "python.exe",
                      {"C:\\Python3*", lad + "\\Programs\\Python\\Python3*", pf + "\\Python3*", "C:\\msys64\\ucrt64\\bin"});
  add("python", "Python", tc.python, "winget install Python.Python.3.13");

  // C++: any g++ or clang++ works (same command line). Collect every candidate so the setup
  // page can offer a choice; the configured one (cppCompiler, or legacy gpp) wins when it exists.
  std::vector<std::string> gccDirs = {"C:\\msys64\\ucrt64\\bin", "C:\\msys64\\mingw64\\bin", "C:\\msys64\\clang64\\bin", "C:\\mingw64\\bin",
                                      "C:\\TDM-GCC-64\\bin", "C:\\w64devkit\\bin", pf + "\\mingw-w64\\*\\mingw64\\bin", "C:\\MinGW\\bin",
                                      pf + "\\LLVM\\bin", lad + "\\Programs\\LLVM\\bin"};
  auto addCandidate = [&](const std::string& p) {
    if (p.empty()) return;
    for (auto& c : tc.cppCandidates)
      if (util::lower(c) == util::lower(p)) return;
    tc.cppCandidates.push_back(p);
  };
  std::string configured = config.value("cppCompiler", config.value("gpp", ""));
  if (!configured.empty() && (util::contains(configured, "\\") || util::contains(configured, "/")) && isFile(util::upath(configured)))
    addCandidate(configured);
  for (auto* exe : {"g++.exe", "clang++.exe"}) {
    addCandidate(onPath(exe));
    for (auto& pat : gccDirs)
      for (auto& dir : expandGlob(pat))
        if (isFile(dir / exe)) addCandidate(util::pstr(dir / exe));
  }
  tc.gpp = resolve(configured.empty() ? "g++" : configured, "g++.exe", gccDirs);
  if (tc.gpp.empty() && !tc.cppCandidates.empty()) tc.gpp = tc.cppCandidates.front();
  add("gpp", "C++ compiler (g++ / clang++)", tc.gpp, "winget install MSYS2.MSYS2, then in MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc");

  std::vector<std::string> jdkDirs = {javaHome.empty() ? "" : javaHome + "\\bin", pf + "\\Eclipse Adoptium\\jdk-*\\bin", pf + "\\Java\\jdk-*\\bin",
                                      pf + "\\Java\\jdk*\\bin", pf + "\\Microsoft\\jdk-*\\bin", pf + "\\Amazon Corretto\\jdk*\\bin",
                                      pf + "\\Zulu\\zulu-*\\bin", pf + "\\OpenJDK\\jdk-*\\bin", pf + "\\BellSoft\\LibericaJDK-*\\bin",
                                      pf86 + "\\Eclipse Adoptium\\jdk-*\\bin", pf + "\\Common Files\\Oracle\\Java\\javapath"};
  tc.javac = resolve(config.value("javac", "javac"), "javac.exe", jdkDirs);
  // prefer the java.exe next to the compiler so both come from the same JDK
  std::string javaNext = tc.javac.empty() ? "" : util::pstr(util::upath(tc.javac).parent_path() / "java.exe");
  tc.java = isFile(util::upath(javaNext)) ? javaNext : resolve(config.value("java", "java"), "java.exe", jdkDirs);
  add("javac", "Java compiler (javac)", tc.javac, "winget install EclipseAdoptium.Temurin.21.JDK");
  add("java", "Java runtime (java)", tc.java, "winget install EclipseAdoptium.Temurin.21.JDK");

  tc.node = resolve(config.value("node", "node"), "node.exe", {pf + "\\nodejs", lad + "\\Programs\\nodejs", pf86 + "\\nodejs"});
  add("node", "JavaScript (Node.js)", tc.node, "winget install OpenJS.NodeJS.LTS");

  std::string gdb = tc.gpp.empty() ? onPath("gdb") : util::pstr(util::upath(tc.gpp).parent_path() / "gdb.exe");
  if (!isFile(util::upath(gdb))) gdb = onPath("gdb");
  add("gdb", "C++ debugger (gdb)", gdb, "in MSYS2: pacman -S mingw-w64-ucrt-x86_64-gdb");

  // keep plain names as a last resort so error messages stay meaningful
  if (tc.python.empty()) tc.python = "python";
  if (tc.gpp.empty()) tc.gpp = "g++";
  if (tc.javac.empty()) tc.javac = "javac";
  if (tc.java.empty()) tc.java = "java";
  if (tc.node.empty()) tc.node = "node";
  return st;
}

#endif  // _WIN32
