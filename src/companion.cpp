#include "companion.hpp"
#include <regex>

namespace companion {

std::string judgeForUrl(const std::string& url) {
  std::string u = util::lower(url);
  if (util::contains(u, "codeforces.com") || util::contains(u, "codeforc.es")) return "codeforces";
  if (util::contains(u, "atcoder.jp")) return "atcoder";
  if (util::contains(u, "cses.fi")) return "cses";
  if (util::contains(u, "usaco.org")) return "usaco";
  if (util::contains(u, "hackerrank.com")) return "hackerrank";
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

bool parsePayload(const json& j, Problem& out, std::string& batchId, int& batchSize) {
  if (!j.is_object() || !j.contains("name") || !j.contains("tests")) return false;
  out = Problem{};
  std::string name = j.value("name", "");
  out.url = j.value("url", "");
  out.group = j.value("group", "");
  out.judge = judgeForUrl(out.url);
  out.interactive = j.value("interactive", false);
  if (j.contains("timeLimit") && j["timeLimit"].is_number()) out.timeLimitSec = j["timeLimit"].get<double>() / 1000.0;
  if (j.contains("memoryLimit") && j["memoryLimit"].is_number()) out.memoryMB = j["memoryLimit"].get<int>();
  splitName(name, out.id, out.title);
  if (out.judge == "codeforces") {
    std::string cid, idx;
    if (parseCodeforcesUrl(out.url, cid, idx)) {
      out.cfContestId = cid;
      out.cfIndex = idx;
      if (out.id.empty()) out.id = idx;
    }
  }
  if (j.contains("tests") && j["tests"].is_array())
    for (auto& t : j["tests"]) {
      TestCase tc;
      tc.in = t.value("input", "");
      tc.out = t.value("output", "");
      out.tests.push_back(tc);
    }
  batchId.clear();
  batchSize = 1;
  if (j.contains("batch") && j["batch"].is_object()) {
    batchId = j["batch"].value("id", "");
    batchSize = j["batch"].value("size", 1);
  }
  out.created = util::nowSec();
  return true;
}

}  // namespace companion
