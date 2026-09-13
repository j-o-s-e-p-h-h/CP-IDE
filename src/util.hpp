// Small shared helpers: UTF-8/wide conversion, file IO, strings, time. The few
// OS-specific bits (paths, env, exe location) are ifdef'd for Windows / POSIX.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <cstdlib>
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace util {

#ifdef _WIN32
inline constexpr bool kWindows = true;
inline std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

inline std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

inline fs::path upath(const std::string& utf8) { return fs::path(widen(utf8)); }
inline std::string pstr(const fs::path& p) { return narrow(p.wstring()); }
#else
inline constexpr bool kWindows = false;
// wchar_t is UTF-32 on Linux/macOS; command lines are kept as std::wstring for
// symmetry with the Windows code and converted back to UTF-8 before use.
inline std::wstring widen(const std::string& s) {
  std::wstring w;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
    else { cp = c & 0x07; len = 4; }
    for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    w += (wchar_t)cp;
    i += len;
  }
  return w;
}

inline std::string narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t wc : w) {
    uint32_t cp = (uint32_t)wc;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
  }
  return s;
}

inline fs::path upath(const std::string& utf8) { return fs::path(utf8); }
inline std::string pstr(const fs::path& p) { return p.string(); }
#endif

// "sol.exe" on Windows, "sol" elsewhere.
inline std::string exeName(const std::string& stem) { return kWindows ? stem + ".exe" : stem; }

inline std::optional<std::string> readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline bool writeFile(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), (std::streamsize)data.size());
  return (bool)f;
}

inline std::optional<json> readJson(const fs::path& p) {
  auto s = readFile(p);
  if (!s) return std::nullopt;
  auto j = json::parse(*s, nullptr, false);
  if (j.is_discarded()) return std::nullopt;
  return j;
}

inline bool writeJson(const fs::path& p, const json& j) { return writeFile(p, j.dump(2)); }

inline std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

inline std::string rtrim(const std::string& s) {
  size_t b = s.size();
  while (b > 0 && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(0, b);
}

inline std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

inline bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
inline bool endsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
inline bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

inline std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else cur += c;
  }
  out.push_back(cur);
  return out;
}

inline std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

// Normalises program output for comparison: trailing whitespace on every line
// is dropped, blank trailing lines are dropped, CRLF -> LF.
inline std::string normalizeOutput(const std::string& s) {
  std::string out;
  std::string line;
  auto flush = [&] {
    out += rtrim(line);
    out += '\n';
    line.clear();
  };
  for (char c : s) {
    if (c == '\r') continue;
    if (c == '\n') flush();
    else line += c;
  }
  if (!line.empty()) flush();
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  size_t lead = 0;
  while (lead < out.size() && out[lead] == '\n') ++lead;  // leading blank lines (samples often start with one)
  return out.substr(lead);
}

// Makes a string safe as a folder name on Windows.
inline std::string safeName(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
      out += '_';
    else out += (char)c;
  }
  out = trim(out);
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  if (out.empty()) out = "untitled";
  if (out.size() > 80) out.resize(80);
  return out;
}

inline int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline int64_t nowSec() { return nowMs() / 1000; }

inline tm localTime(time_t t) {
  tm lt{};
#ifdef _WIN32
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  return lt;
}

// "HH:MM" local time.
inline std::string clockHHMM(int64_t epochSec = 0) {
  time_t t = epochSec ? (time_t)epochSec : time(nullptr);
  tm lt = localTime(t);
  char buf[16];
  strftime(buf, sizeof buf, "%H:%M", &lt);
  return buf;
}

// "Sep 2" style date, or "Today".
inline std::string prettyDate(int64_t epochSec) {
  time_t t = (time_t)epochSec, now = time(nullptr);
  tm a = localTime(t), b = localTime(now);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return "Today";
  char buf[32];
  strftime(buf, sizeof buf, "%b %e", &a);
  std::string s = buf;
  return replaceAll(s, "  ", " ");
}

inline std::string fmtClock(int64_t secs) {
  if (secs < 0) secs = 0;
  char buf[32];
  snprintf(buf, sizeof buf, "%02lld:%02lld", (long long)(secs / 60), (long long)(secs % 60));
  return buf;
}

inline std::string envVar(const char* name) {
#ifdef _WIN32
  std::wstring wname = widen(name);
  DWORD need = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);  // size incl. the terminator
  if (need == 0) return {};
  std::wstring buf(need, L'\0');
  DWORD n = GetEnvironmentVariableW(wname.c_str(), buf.data(), need);
  if (n == 0 || n >= need) return {};
  buf.resize(n);
  return narrow(buf);
#else
  const char* v = getenv(name);
  return v ? v : "";
#endif
}

inline void setEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
  SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

inline fs::path exeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
  return fs::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
  char buf[PATH_MAX];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) == 0) {
    std::error_code ec;
    auto p = fs::weakly_canonical(fs::path(buf), ec);
    return (ec ? fs::path(buf) : p).parent_path();
  }
  return fs::current_path();
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::current_path() : p.parent_path();
#endif
}

// Prepends a directory to PATH for this process and its children.
inline void prependPath(const fs::path& dir) {
  std::string cur = envVar("PATH");
  std::string d = pstr(dir);
  const char sep = kWindows ? ';' : ':';
  if (contains(lower(cur), lower(d) + sep)) return;
  setEnvVar("PATH", d + sep + cur);
}

inline std::string htmlEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

inline std::string htmlUnescape(std::string s) {
  s = replaceAll(s, "&lt;", "<");
  s = replaceAll(s, "&gt;", ">");
  s = replaceAll(s, "&quot;", "\"");
  s = replaceAll(s, "&#39;", "'");
  s = replaceAll(s, "&nbsp;", " ");
  s = replaceAll(s, "&amp;", "&");
  return s;
}

// A URL we are willing to fetch / open / splice into a command line: http(s) only and
// nothing but the characters RFC 3986 allows (no quotes, spaces, backslashes, $, backticks).
inline bool isSafeHttpUrl(const std::string& u) {
  if (!(startsWith(u, "http://") || startsWith(u, "https://"))) return false;
  if (u.size() > 2048) return false;
  for (unsigned char c : u) {
    if (isalnum(c)) continue;
    if (std::string("-._~:/?#[]@!&()*+,;=%").find((char)c) != std::string::npos) continue;
    return false;
  }
  return true;
}

// Host part of a URL, lower-cased, without port ("www.codeforces.com").
inline std::string urlHost(const std::string& u) {
  auto p = u.find("://");
  if (p == std::string::npos) return {};
  auto start = p + 3;
  auto end = u.find_first_of("/?#", start);
  std::string hp = u.substr(start, end == std::string::npos ? std::string::npos : end - start);
  auto at = hp.rfind('@');
  if (at != std::string::npos) hp = hp.substr(at + 1);
  auto colon = hp.find(':');
  if (colon != std::string::npos) hp = hp.substr(0, colon);
  return lower(hp);
}

inline std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

}  // namespace util
