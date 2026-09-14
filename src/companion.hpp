// Competitive Companion payload -> Problem, plus URL/judge helpers.
#pragma once
#include <string>
#include "model.hpp"

namespace companion {

// "codeforces" | "atcoder" | "cses" | "usaco" | "hackerrank" | "other"
std::string judgeForUrl(const std::string& url);

// Codeforces contest id + index from a URL; false when not a CF problem URL.
bool parseCodeforcesUrl(const std::string& url, std::string& contestId, std::string& index);

// A link to a whole contest rather than one problem, e.g.
// codeforces.com/contest/2009 or atcoder.jp/contests/abc319 (including the
// /standings, /my and virtual-participation variants people actually copy).
// Sets contestId to the CF id or the AtCoder slug.
bool parseContestUrl(const std::string& url, std::string& judge, std::string& contestId);

// "D. Range Repaint" -> ("D", "Range Repaint"); "A - Title" -> ("A", "Title");
// "Weird Algorithm" -> ("", "Weird Algorithm").
void splitName(const std::string& name, std::string& index, std::string& title);

// "Codeforces - Codeforces Round 970 (Div. 2)" -> "Codeforces Round 970 (Div. 2)"
std::string contestNameFromGroup(const std::string& group);

// Parses a Competitive Companion JSON payload. Returns false when it's not one.
bool parsePayload(const json& j, Problem& out, std::string& batchId, int& batchSize);

}  // namespace companion
