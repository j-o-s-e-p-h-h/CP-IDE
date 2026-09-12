// Fetches a problem page and extracts the statement HTML (+ limits, samples)
// so the Description pane can show it, images included.
#pragma once
#include <string>
#include <vector>
#include "http_client.hpp"
#include "model.hpp"

struct StatementInfo {
  bool ok = false;
  std::string error;
  std::string html;   // statement HTML or empty
  bool exact = false; // true when html is the judge's complete problem block (title, samples, note included)
  std::string title;  // title without index, when found
  std::string index;  // e.g. "D"
  int rating = 0;
  double timeLimitSec = 0;
  int memoryMB = 0;
  std::vector<TestCase> samples;
};

StatementInfo fetchStatement(HttpClient& http, const std::string& url, const std::string& judge);

// HTML helpers (also used by the Codeforces judge).
namespace html {
// Inner HTML of the first element whose opening tag contains `marker`
// (e.g. `class="problem-statement"`). Only works for <div>-balanced content.
std::string extractDiv(const std::string& doc, const std::string& marker, size_t* startPos = nullptr, size_t* endPos = nullptr);
std::string stripTags(const std::string& s);
std::string preToText(const std::string& pre);
std::string absolutizeUrls(std::string s, const std::string& base);
std::string removeScripts(std::string s);
std::string attr(const std::string& tag, const std::string& name);
}  // namespace html
