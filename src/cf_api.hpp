// Codeforces public API helpers (no login needed).
#pragma once
#include <set>
#include <string>
#include <vector>
#include "http_client.hpp"
#include "model.hpp"

namespace cfapi {

struct ApiProblem {
  std::string contestId, index, name;
  int rating = 0;
};

// problemset.problems, filtered by rating range. Empty on failure (error set).
std::vector<ApiProblem> problemsInRange(HttpClient& http, int lo, int hi, std::string& error);

// Set of "contestId/index" the handle has an OK verdict on.
std::set<std::string> solvedBy(HttpClient& http, const std::string& handle);

// Picks `count` random problems from `pool` excluding `exclude` keys, favouring distinct contests.
std::vector<ApiProblem> pickRandom(std::vector<ApiProblem> pool, const std::set<std::string>& exclude, int count);

inline std::string problemUrl(const std::string& contestId, const std::string& index) {
  return "https://codeforces.com/problemset/problem/" + contestId + "/" + index;
}

}  // namespace cfapi
