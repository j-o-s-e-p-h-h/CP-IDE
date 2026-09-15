#include "companion.hpp"
#include <algorithm>
#include <regex>

namespace companion {

std::string judgeForUrl(const std::string& url) {
  // Exact host match (with subdomains), never a substring: "evil.example/?codeforces.com"
  // must not be treated as Codeforces.
  std::string h = util::urlHost(url);
  auto isHost = [&](const char* domain) {
    std::string d = domain;
    return h == d || (h.size() > d.size() && util::endsWith(h, "." + d));
  };
  if (isHost("codeforces.com") || isHost("codeforc.es")) return "codeforces";
  if (isHost("atcoder.jp")) return "atcoder";
  if (isHost("cses.fi")) return "cses";
  if (isHost("usaco.org")) return "usaco";
  if (isHost("hackerrank.com")) return "hackerrank";
  if (isHost("codechef.com")) return "codechef";
  return "other";
}

bool parseCodeforcesUrl(const std::string& url, std::string& contestId, std::string& index) {
  static const std::regex re1(R"((?:contest|gym)/(\d+)/problem/([A-Za-z]\d?))");
  static const std::regex re2(R"(problemset/problem/(\d+)/([A-Za-z]\d?))");
  std::smatch m;
  if (std::regex_search(url, m, re1) || std::regex_search(url, m, re2)) {
    contestId = m[1];
    index = m[2];
    for (auto& c : index) c = (char)toupper((unsigned char)c);
    return true;
  }
  return false;
}

bool parseContestUrl(const std::string& url, std::string& judge, std::string& contestId) {
  std::string j = judgeForUrl(url);
  if (j == "codeforces") {
    // A problem link is not a contest link, even though it contains the contest id.
    if (util::contains(url, "/problem/") || util::contains(url, "/problemset/problem/")) return false;
    static const std::regex re(R"((?:contest|gym)/(\d+))");
    std::smatch m;
    if (!std::regex_search(url, m, re)) return false;
    judge = "codeforces";
    contestId = m[1];
    return true;
  }
  if (j == "atcoder") {
    if (util::contains(url, "/tasks/")) return false;
    static const std::regex re(R"(atcoder\.jp/contests/([A-Za-z0-9_\-]+))");
    std::smatch m;
    if (!std::regex_search(url, m, re)) return false;
    judge = "atcoder";
    contestId = m[1];
    return true;
  }
  return false;
}

void splitName(const std::string& name, std::string& index, std::string& title) {
  static const std::regex re(R"(^\s*([A-Za-z]\d?)\s*[.\-:–—]\s*(.+)$)");
  std::smatch m;
  std::string n = util::trim(name);
  if (std::regex_match(n, m, re) && m[2].str().size() > 0) {
    index = m[1];
    for (auto& c : index) c = (char)toupper((unsigned char)c);
    title = util::trim(m[2]);
    return;
  }
  index.clear();
  title = n;
}

std::string contestNameFromGroup(const std::string& group) {
  std::string g = util::trim(group);
  auto p = g.find(" - ");
  if (p != std::string::npos) g = util::trim(g.substr(p + 3));
  if (g.empty()) g = "Imported";
  return g;
}

// The payload comes from an unauthenticated local port, so every field is checked for
// type before use (nlohmann's value() throws on a type mismatch).
static std::string str(const json& j, const char* key) {
  return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

bool parsePayload(const json& j, Problem& out, std::string& batchId, int& batchSize) {
  if (!j.is_object() || !j.contains("name") || !j["name"].is_string() || !j.contains("tests") || !j["tests"].is_array()) return false;
  out = Problem{};
  std::string name = str(j, "name");
  out.url = str(j, "url");
  if (!out.url.empty() && !util::isSafeHttpUrl(out.url)) out.url.clear();  // never fetch or open odd URLs
  out.group = str(j, "group");
  out.judge = judgeForUrl(out.url);
  out.interactive = j.contains("interactive") && j["interactive"].is_boolean() && j["interactive"].get<bool>();
  if (j.contains("timeLimit") && j["timeLimit"].is_number()) out.timeLimitSec = std::max(0.1, std::min(60.0, j["timeLimit"].get<double>() / 1000.0));
  if (j.contains("memoryLimit") && j["memoryLimit"].is_number()) out.memoryMB = std::max(16, std::min(4096, (int)j["memoryLimit"].get<double>()));
  splitName(name, out.id, out.title);
  if (out.judge == "codeforces") {
    std::string cid, idx;
    if (parseCodeforcesUrl(out.url, cid, idx)) {
      out.cfContestId = cid;
      out.cfIndex = idx;
      if (out.id.empty()) out.id = idx;
    }
  }
  for (auto& t : j["tests"]) {
    if (!t.is_object()) continue;
    TestCase tc;
    tc.in = str(t, "input");
    tc.out = str(t, "output");
    out.tests.push_back(tc);
    if (out.tests.size() >= 50) break;
  }
  batchId.clear();
  batchSize = 1;
  if (j.contains("batch") && j["batch"].is_object()) {
    batchId = str(j["batch"], "id");
    if (j["batch"].contains("size") && j["batch"]["size"].is_number()) batchSize = (int)j["batch"]["size"].get<double>();
  }
  if (name.size() > 200) name.resize(200);
  out.created = util::nowSec();
  return true;
}

}  // namespace companion
