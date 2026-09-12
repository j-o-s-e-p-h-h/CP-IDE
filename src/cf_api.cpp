#include "cf_api.hpp"
#include <algorithm>
#include <random>

namespace cfapi {

std::vector<ApiProblem> problemsInRange(HttpClient& http, int lo, int hi, std::string& error) {
  std::vector<ApiProblem> out;
  auto res = http.get("https://codeforces.com/api/problemset.problems");
  if (!res.error.empty()) {
    error = res.error;
    return out;
  }
  if (res.status != 200) {
    error = "Codeforces API returned HTTP " + std::to_string(res.status);
    return out;
  }
  auto j = json::parse(res.body, nullptr, false);
  if (j.is_discarded() || j.value("status", "") != "OK") {
    error = "Codeforces API error";
    return out;
  }
  for (auto& p : j["result"]["problems"]) {
    if (!p.contains("rating") || !p.contains("contestId")) continue;
    int r = p["rating"].get<int>();
    if (r < lo || r > hi) continue;
    ApiProblem ap;
    ap.contestId = std::to_string(p["contestId"].get<int64_t>());
    ap.index = p.value("index", "");
    ap.name = p.value("name", "");
    ap.rating = r;
    out.push_back(ap);
  }
  return out;
}

std::set<std::string> solvedBy(HttpClient& http, const std::string& handle) {
  std::set<std::string> s;
  if (handle.empty()) return s;
  auto res = http.get("https://codeforces.com/api/user.status?handle=" + util::urlEncode(handle) + "&from=1&count=100000");
  if (!res.ok()) return s;
  auto j = json::parse(res.body, nullptr, false);
  if (j.is_discarded() || j.value("status", "") != "OK") return s;
  for (auto& sub : j["result"]) {
    if (sub.value("verdict", "") != "OK") continue;
    auto& p = sub["problem"];
    if (!p.contains("contestId")) continue;
    s.insert(std::to_string(p["contestId"].get<int64_t>()) + "/" + p.value("index", ""));
  }
  return s;
}

std::vector<ApiProblem> pickRandom(std::vector<ApiProblem> pool, const std::set<std::string>& exclude, int count) {
  std::vector<ApiProblem> out;
  pool.erase(std::remove_if(pool.begin(), pool.end(),
                            [&](const ApiProblem& p) { return exclude.count(p.contestId + "/" + p.index) > 0; }),
             pool.end());
  std::random_device rd;
  std::mt19937 rng(rd());
  std::shuffle(pool.begin(), pool.end(), rng);
  std::set<std::string> usedContests;
  for (auto& p : pool) {
    if ((int)out.size() >= count) break;
    if (usedContests.count(p.contestId)) continue;
    usedContests.insert(p.contestId);
    out.push_back(p);
  }
  for (auto& p : pool) {
    if ((int)out.size() >= count) break;
    if (std::find_if(out.begin(), out.end(), [&](auto& q) { return q.contestId == p.contestId && q.index == p.index; }) == out.end())
      out.push_back(p);
  }
  return out;
}

}  // namespace cfapi
