#include "statement.hpp"
#include <map>
#include <regex>
#include "companion.hpp"
#include "runner.hpp"

namespace html {

std::string extractTag(const std::string& doc, const std::string& marker, const std::string& tag,
                       size_t* startPos, size_t* endPos) {
  const std::string openTag = "<" + tag, closeTag = "</" + tag;
  size_t m = doc.find(marker);
  if (m == std::string::npos) return {};
  size_t open = doc.rfind(openTag, m);
  if (open == std::string::npos) return {};
  size_t tagEnd = doc.find('>', m);
  if (tagEnd == std::string::npos) return {};
  int depth = 1;
  size_t pos = tagEnd + 1;
  while (depth > 0) {
    size_t a = doc.find(openTag, pos);
    size_t b = doc.find(closeTag, pos);
    if (b == std::string::npos) return {};
    if (a != std::string::npos && a < b) {
      depth++;
      pos = a + openTag.size();
    } else {
      depth--;
      if (depth == 0) {
        size_t close = doc.find('>', b);
        if (startPos) *startPos = open;
        if (endPos) *endPos = close == std::string::npos ? b + closeTag.size() + 1 : close + 1;
        return doc.substr(tagEnd + 1, b - tagEnd - 1);
      }
      pos = b + closeTag.size();
    }
  }
  return {};
}

std::string extractDiv(const std::string& doc, const std::string& marker, size_t* startPos, size_t* endPos) {
  return extractTag(doc, marker, "div", startPos, endPos);
}

std::string stripTags(const std::string& s) {
  std::string o;
  bool in = false;
  for (char c : s) {
    if (c == '<') in = true;
    else if (c == '>') in = false;
    else if (!in) o += c;
  }
  return util::htmlUnescape(o);
}

std::string preToText(const std::string& pre) {
  std::string s = pre;
  static const std::regex br(R"(<br\s*/?>)", std::regex::icase);
  s = std::regex_replace(s, br, "\n");
  s = util::replaceAll(s, "</div>", "\n");
  s = stripTags(s);
  // collapse CRLF, drop leading/trailing blank lines, trim each line's trailing spaces
  std::string n = util::normalizeOutput(s);
  return n.empty() ? n : n + "\n";
}

std::string absolutizeUrls(std::string s, const std::string& base) {
  // base like https://codeforces.com/contest/1/problem/A
  std::string origin = base;
  auto p = origin.find("://");
  if (p != std::string::npos) {
    auto q = origin.find('/', p + 3);
    if (q != std::string::npos) origin = origin.substr(0, q);
  }
  std::string scheme = util::startsWith(base, "http://") ? "http:" : "https:";
  s = util::replaceAll(s, "src=\"//", "src=\"" + scheme + "//");
  s = util::replaceAll(s, "src='//", "src='" + scheme + "//");
  s = util::replaceAll(s, "src=\"/", "src=\"" + origin + "/");
  s = util::replaceAll(s, "src='/", "src='" + origin + "/");
  s = util::replaceAll(s, "href=\"/", "href=\"" + origin + "/");
  return s;
}

// Statement HTML is rendered with innerHTML inside the app page, next to the RPC bridge,
// and it comes from a site chosen by whoever POSTs to the local port. Strip everything that
// can run code: script-like elements (with their content), event-handler attributes and
// javascript:/data: URLs. The UI applies a DOM-based pass as well.
std::string removeScripts(std::string s) {
  static const std::regex blocks(R"(<(script|style|iframe|object|embed|svg|math|template|noscript|form|link|meta|base|frame|frameset|applet)\b[\s\S]*?(</\1\s*>|$))", std::regex::icase);
  static const std::regex selfClosing(R"(<(link|meta|base|embed|input|button|textarea|select|frame)\b[^>]*>)", std::regex::icase);
  static const std::regex onAttr(R"(\s+on[a-zA-Z]+\s*=\s*("[^"]*"|'[^']*'|[^\s>]+))", std::regex::icase);
  static const std::regex badUrl(R"(\s+(href|src|xlink:href|formaction|action|srcset|poster|background)\s*=\s*("\s*(javascript|data|vbscript)\s*:[^"]*"|'\s*(javascript|data|vbscript)\s*:[^']*'|\s*(javascript|data|vbscript)\s*:[^\s>]+))", std::regex::icase);
  static const std::regex styleAttr(R"(\s+style\s*=\s*("[^"]*"|'[^']*'))", std::regex::icase);
  s = std::regex_replace(s, blocks, "");
  s = std::regex_replace(s, selfClosing, "");
  s = std::regex_replace(s, onAttr, "");
  s = std::regex_replace(s, badUrl, "");
  // inline style with url() / expression() has been used for exfiltration; images keep max-width via CSS anyway
  s = std::regex_replace(s, styleAttr, "");
  return s;
}

std::string attr(const std::string& tag, const std::string& name) {
  std::regex re(name + R"rx(\s*=\s*"([^"]*)")rx");
  std::smatch m;
  if (std::regex_search(tag, m, re)) return m[1];
  std::regex re2(name + R"rx(\s*=\s*'([^']*)')rx");
  if (std::regex_search(tag, m, re2)) return m[1];
  return {};
}

}  // namespace html

namespace {

// Codeforces writes "2 seconds", AtCoder "2 sec"; memory is "256 megabytes" on
// Codeforces and "1024 MiB" on AtCoder. Missing either one used to leave the
// problem on the 1 s / 256 MB defaults, which shows the wrong limits and marks
// perfectly fast solutions TLE locally.
double parseSeconds(const std::string& text) {
  static const std::regex re(R"(([\d.]+)\s*(?:seconds?|secs?|s)\b)", std::regex::icase);
  std::smatch m;
  if (std::regex_search(text, m, re)) return atof(m[1].str().c_str());
  return 0;
}

int parseMegabytes(const std::string& text) {
  static const std::regex re(R"((\d+)\s*(?:megabytes?|MiB|MB)\b)", std::regex::icase);
  std::smatch m;
  if (std::regex_search(text, m, re)) return atoi(m[1].str().c_str());
  return 0;
}

// Codeforces wraps every sample line in <div class="… test-example-line-N">, where N
// is the test case the line belongs to and 0 is the shared preamble (the "t" line).
// Both the input and the output block are tagged, which is what lets the site
// highlight a case and its answer together.
std::map<int, std::vector<std::string>> taggedLines(const std::string& preHtml, int& maxIdx) {
  static const std::regex re(R"(<div[^>]*test-example-line-(\d+)[^>]*>([\s\S]*?)</div>)");
  std::map<int, std::vector<std::string>> out;
  maxIdx = 0;
  for (auto it = std::sregex_iterator(preHtml.begin(), preHtml.end(), re); it != std::sregex_iterator(); ++it) {
    int idx = atoi((*it)[1].str().c_str());
    out[idx].push_back(util::rtrim(html::stripTags((*it)[2].str())));
    maxIdx = std::max(maxIdx, idx);
  }
  return out;
}

// One sample holding t test cases becomes t tests, each with its own expected
// output, so a verdict names the case that failed instead of the whole blob.
// Each case shows exactly the lines Codeforces shows for it. The "1 test case"
// count line a solution expects to read first is kept out of sight in `pre`, so the
// box is not cluttered with a line that is not part of the case.
bool splitMultiTest(const std::string& inRaw, const std::string& outRaw, std::vector<TestCase>& out) {
  int mi = 0, mo = 0;
  auto in = taggedLines(inRaw, mi);
  auto ou = taggedLines(outRaw, mo);
  if (mi < 2 || mi != mo) return false;
  auto pre = in.find(0);
  // Only safe when the preamble is exactly the count of cases: that is the line the
  // per-case input has to replace with "1".
  if (pre == in.end() || pre->second.size() != 1 || util::trim(pre->second[0]) != std::to_string(mi)) return false;
  for (int i = 1; i <= mi; ++i)
    if (!in.count(i) || !ou.count(i)) return false;
  for (int i = 1; i <= mi; ++i) {
    TestCase tc;
    tc.pre = "1\n";
    for (auto& l : in[i]) tc.in += l + "\n";
    for (auto& l : ou[i]) tc.out += l + "\n";
    out.push_back(tc);
  }
  return true;
}

StatementInfo parseCodeforces(const std::string& doc, const std::string& url) {
  StatementInfo si;
  std::string stmt = html::extractDiv(doc, "class=\"problem-statement\"");
  if (stmt.empty()) {
    si.error = "No problem statement found on the page";
    return si;
  }
  // header: title + limits
  std::string header = html::extractDiv(stmt, "class=\"header\"");
  {
    std::string title = html::stripTags(html::extractDiv(header, "class=\"title\""));
    std::string idx, t;
    companion::splitName(util::trim(title), idx, t);
    si.index = idx;
    si.title = t;
    si.timeLimitSec = parseSeconds(html::stripTags(html::extractDiv(header, "class=\"time-limit\"")));
    si.memoryMB = parseMegabytes(html::stripTags(html::extractDiv(header, "class=\"memory-limit\"")));
  }
  // samples
  size_t sa = 0, sb = 0;
  std::string samples = html::extractDiv(stmt, "class=\"sample-tests\"", &sa, &sb);
  if (!samples.empty()) {
    // Each sample is: class="input" ... <pre>IN</pre> ... class="output" ... <pre>OUT</pre>.
    // Take the first <pre> after each marker; this does not depend on div nesting.
    auto preRawAfter = [&](size_t from, size_t& end) -> std::string {
      size_t p = samples.find("<pre", from);
      if (p == std::string::npos) return std::string();
      size_t e = samples.find('>', p);
      size_t q = e == std::string::npos ? std::string::npos : samples.find("</pre>", e);
      if (q == std::string::npos) return std::string();
      end = q + 6;
      return samples.substr(e + 1, q - e - 1);
    };
    std::vector<std::pair<std::string, std::string>> raws;
    size_t pos = 0;
    while (true) {
      size_t a = samples.find("class=\"input\"", pos);
      if (a == std::string::npos) break;
      size_t inEnd = a;
      std::string inRaw = preRawAfter(a, inEnd);
      size_t outA = samples.find("class=\"output\"", inEnd);
      if (outA == std::string::npos) break;
      size_t outEnd = outA;
      std::string outRaw = preRawAfter(outA, outEnd);
      std::string in = html::preToText(inRaw), out = html::preToText(outRaw);
      if (util::trim(in).empty() && util::trim(out).empty()) break;
      TestCase tc;
      tc.in = in;
      tc.out = out;
      si.samples.push_back(tc);
      raws.emplace_back(inRaw, outRaw);
      pos = outEnd;
    }
    // A single sample carrying many test cases is worth one test per case.
    if (si.samples.size() == 1 && raws.size() == 1) {
      std::vector<TestCase> split;
      if (splitMultiTest(raws[0].first, raws[0].second, split)) si.samples = std::move(split);
    }
  }
  // Codeforces answers an index that does not exist (asking for G when the problem
  // is split into G1/G2/G3) with a page that still carries a problem-statement
  // block — for an unrelated problem. Importing that silently would give you the
  // wrong statement and no samples, so refuse it instead.
  {
    std::string wantContest, wantIndex;
    companion::parseCodeforcesUrl(url, wantContest, wantIndex);
    std::string gotIndex = util::lower(si.index), want = util::lower(wantIndex);
    if (!want.empty() && !gotIndex.empty() && gotIndex != want) {
      si.error = "Codeforces returned problem " + si.index + " for a link to " + wantIndex +
                 " — that index may only exist as " + wantIndex + "1 / " + wantIndex + "2";
      return si;
    }
    if (si.samples.empty() && si.timeLimitSec <= 0) {
      si.error = "That Codeforces page has no problem on it (check the contest and index)";
      return si;
    }
  }
  // The whole problem block (header, legend, specs, samples, note) is kept so the
  // Description pane can show the statement exactly as Codeforces renders it.
  si.exact = true;
  si.selfSamples = true;
  // rating from the sidebar tag box
  {
    static const std::regex re(R"(title="Difficulty"[^>]*>\s*\*(\d+))");
    std::smatch m;
    if (std::regex_search(doc, m, re)) si.rating = atoi(m[1].str().c_str());
  }
  si.html = html::absolutizeUrls(html::removeScripts(stmt), url);
  si.ok = true;
  return si;
}

// AtCoder writes every formula as bare LaTeX inside <var>…</var> with no delimiters,
// so KaTeX never sees it and the reader gets "1\leq M\leq N\leq2\times10 ^ 5" as text.
// Outside a <pre> the tags become \( … \); inside one KaTeX does not run at all
// (auto-render skips <pre>), so there the tags are dropped and the text kept as is.
std::string varsToTex(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 32);
  for (size_t i = 0; i < s.size();) {
    if (s.compare(i, 4, "<var") == 0) {
      size_t close = s.find('>', i);
      if (close != std::string::npos) {
        out += "\\(";
        i = close + 1;
        continue;
      }
    }
    if (s.compare(i, 6, "</var>") == 0) {
      out += "\\)";
      i += 6;
      continue;
    }
    out += s[i++];
  }
  return out;
}

std::string atcoderMath(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 128);
  size_t i = 0;
  while (i < s.size()) {
    size_t pre = s.find("<pre", i);
    if (pre == std::string::npos) {
      out += varsToTex(s.substr(i));
      break;
    }
    out += varsToTex(s.substr(i, pre - i));
    size_t open = s.find('>', pre);
    size_t end = open == std::string::npos ? std::string::npos : s.find("</pre>", open);
    if (end == std::string::npos) {
      out += s.substr(pre);
      break;
    }
    std::string body = s.substr(open + 1, end - open - 1);
    // The input-format block is a <pre> full of <var>s. KaTeX's auto-render skips
    // <pre> entirely, so that one becomes a styled div and keeps its maths; the
    // sample input/output blocks hold no <var> and stay verbatim <pre>.
    if (body.find("<var") != std::string::npos) out += "<div class=\"io-format\">" + varsToTex(body) + "</div>";
    else out += s.substr(pre, end + 6 - pre);
    i = end + 6;
  }
  return out;
}

StatementInfo parseAtCoder(const std::string& doc, const std::string& url) {
  StatementInfo si;
  std::string stmt = html::extractDiv(doc, "id=\"task-statement\"");
  if (stmt.empty()) {
    si.error = "No task statement found on the page";
    return si;
  }
  // Prefer the English version when both languages are present. AtCoder wraps each
  // language in a <span>, not a <div> — balancing on <div> used to fail silently and
  // leave the Japanese and English statements (and every figure) in twice.
  if (stmt.find("class=\"lang-en\"") != std::string::npos) {
    std::string enHtml = html::extractTag(stmt, "class=\"lang-en\"", "span");
    if (enHtml.empty()) enHtml = html::extractDiv(stmt, "class=\"lang-en\"");
    if (!enHtml.empty()) stmt = enHtml;
  }
  {
    static const std::regex re(R"(<span class="h2">\s*([^<]+?)\s*(?:<|$))");
    std::smatch m;
    if (std::regex_search(doc, m, re)) {
      std::string idx, t;
      companion::splitName(util::trim(m[1]), idx, t);
      si.index = idx;
      si.title = t;
    }
  }
  // AtCoder prints both on one line: "Time Limit: 2 sec / Memory Limit: 1024 MiB".
  si.timeLimitSec = 0;
  si.memoryMB = 0;
  {
    static const std::regex re(R"(Time Limit:\s*([\d.]+)\s*sec[^/]*/\s*Memory Limit:\s*(\d+)\s*(?:MiB|MB))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(doc, m, re)) {
      si.timeLimitSec = atof(m[1].str().c_str());
      si.memoryMB = atoi(m[2].str().c_str());
    }
  }
  if (si.timeLimitSec <= 0) {
    std::string head = html::stripTags(doc.substr(0, std::min<size_t>(doc.size(), 200000)));
    si.timeLimitSec = parseSeconds(head);
    if (si.memoryMB <= 0) si.memoryMB = parseMegabytes(head);
  }
  // samples: <h3>Sample Input N</h3><pre>...</pre>
  {
    static const std::regex re(R"(Sample (Input|Output) (\d+)</h3>\s*<pre[^>]*>([\s\S]*?)</pre>)");
    std::map<int, TestCase> map;
    for (auto it = std::sregex_iterator(stmt.begin(), stmt.end(), re); it != std::sregex_iterator(); ++it) {
      int n = atoi((*it)[2].str().c_str());
      if ((*it)[1] == "Input") map[n].in = html::preToText((*it)[3]);
      else map[n].out = html::preToText((*it)[3]);
    }
    for (auto& [n, tc] : map) si.samples.push_back(tc);
  }
  // The statement carries its own "Sample Input/Output" sections, and the explanations
  // there are where AtCoder puts its figures — so keep them and let the UI skip its
  // own copy rather than showing every sample twice.
  si.selfSamples = true;
  si.html = atcoderMath(html::absolutizeUrls(html::removeScripts(stmt), url));
  si.ok = true;
  return si;
}

// USACO writes its maths with MathJax's plain $…$ and $$…$$. Those delimiters are
// far too easy to hit by accident to enable globally — a Codeforces statement that
// mentions "$5" would start rendering as maths — so they are rewritten here into
// the \(…\) and \[…\] forms the page's KaTeX already accepts.
std::string dollarMath(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 128);
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '<') {
      // Step over a tag, or a whole <pre> block: KaTeX does not run inside one,
      // and a sample's text must survive exactly as the judge printed it.
      if (s.compare(i, 4, "<pre") == 0) {
        size_t end = s.find("</pre>", i);
        if (end == std::string::npos) { out += s.substr(i); break; }
        out += s.substr(i, end + 6 - i);
        i = end + 6;
        continue;
      }
      size_t close = s.find('>', i);
      if (close == std::string::npos) { out += s.substr(i); break; }
      out += s.substr(i, close + 1 - i);
      i = close + 1;
      continue;
    }
    if (s[i] == '$') {
      bool display = s.compare(i, 2, "$$") == 0;
      size_t open = i + (display ? 2 : 1);
      size_t end = s.find(display ? "$$" : "$", open);
      // A lone or empty $, or one whose partner sits past some markup, is a
      // literal dollar sign — leave it alone rather than swallowing the tags.
      if (end != std::string::npos && end > open && s.find('<', open) > end) {
        out += display ? "\\[" : "\\(";
        out += s.substr(open, end - open);
        out += display ? "\\]" : "\\)";
        i = end + (display ? 2 : 1);
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

// CodeChef renders its problem pages in the browser, so the HTML that arrives over
// HTTP carries no statement at all. Its public JSON API does, and in a cleaner form
// than the page would have been: <contest>/<code> out of the URL, and the API answers
// with the title, limits, the statement in parts, and the samples already separated.
//   /problems/FLOW001                 -> PRACTICE / FLOW001
//   /START100A/problems/XYZ           -> START100A / XYZ
//   /practice/course/.../problems/ABC -> PRACTICE / ABC
bool codechefApiUrl(const std::string& url, std::string& out) {
  std::smatch m;
  static const std::regex withContest(R"(codechef\.com/([A-Za-z0-9_\-]+)/problems/([A-Za-z0-9_\-]+))");
  static const std::regex practice(R"(codechef\.com/(?:.*/)?problems/([A-Za-z0-9_\-]+))");
  std::string contest, code;
  if (std::regex_search(url, m, withContest) && m[1] != "practice") {
    contest = m[1];
    code = m[2];
  } else if (std::regex_search(url, m, practice)) {
    contest = "PRACTICE";
    code = m[1];
  } else {
    return false;
  }
  out = "https://www.codechef.com/api/contests/" + contest + "/problems/" + code;
  return true;
}

// The statement parts come back as light Markdown. Only the handful of constructs
// CodeChef actually emits are handled; $…$ maths is left for dollarMath.
std::string miniMarkdown(const std::string& md) {
  std::string out;
  bool inList = false;
  size_t i = 0;
  auto inline_ = [](const std::string& s) {
    std::string r = s;
    r = std::regex_replace(r, std::regex(R"(\*\*([^*]+)\*\*)"), "<b>$1</b>");
    r = std::regex_replace(r, std::regex(R"(`([^`]+)`)"), "<code>$1</code>");
    // [text](url), http(s) only — anything else stays literal rather than becoming
    // a link to a scheme we did not intend to allow.
    r = std::regex_replace(r, std::regex(R"(\[([^\]]+)\]\((https?://[^)\s]+)\))"),
                           "<a href=\"$2\" target=\"_blank\">$1</a>");
    return r;
  };
  while (i <= md.size()) {
    size_t nl = md.find('\n', i);
    std::string line = util::trim(md.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
    if (!line.empty() && (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0)) {
      if (!inList) { out += "<ul>"; inList = true; }
      out += "<li>" + inline_(line.substr(2)) + "</li>";
    } else {
      if (inList) { out += "</ul>"; inList = false; }
      if (!line.empty()) out += "<p>" + inline_(line) + "</p>";
    }
    if (nl == std::string::npos) break;
    i = nl + 1;
  }
  if (inList) out += "</ul>";
  return out;
}

StatementInfo parseCodeChef(const std::string& body, const std::string& url) {
  StatementInfo si;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    si.error = "CodeChef sent something that is not JSON";
    return si;
  }
  auto str = [&](const json& o, const char* key) {
    return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
  };
  if (!str(j, "problem_name").empty()) si.title = str(j, "problem_name");
  si.timeLimitSec = atof(str(j, "max_timelimit").c_str());
  int rating = atoi(str(j, "difficulty_rating").c_str());
  if (rating > 0) si.rating = rating;   // unrated practice problems report -1

  const json& pc = j.contains("problemComponents") ? j["problemComponents"] : json::object();
  if (pc.is_object() && pc.contains("sampleTestCases") && pc["sampleTestCases"].is_array()) {
    for (const auto& c : pc["sampleTestCases"]) {
      TestCase t;
      t.in = str(c, "input");
      t.out = str(c, "output");
      if (!t.in.empty()) si.samples.push_back(t);
    }
  }

  std::string html;
  std::string statement = pc.is_object() ? str(pc, "statement") : std::string();
  if (!statement.empty()) {
    // The modern format: statement, then each section that is switched on.
    html = miniMarkdown(statement);
    auto section = [&](const char* title, const char* key, const char* stateKey) {
      std::string text = str(pc, key);
      if (text.empty()) return;
      if (stateKey && str(pc, stateKey) == "false") return;
      html += "<div class=\"section-title\">" + std::string(title) + "</div>" + miniMarkdown(text);
    };
    section("Input Format", "inputFormat", "inputFormatState");
    section("Output Format", "outputFormat", "outputFormatState");
    section("Constraints", "constraints", "constraintsState");
    section("Subtasks", "subtasks", "subtasksState");
  } else {
    // Older problems only have the rendered legacy body, samples included.
    html = str(j, "body");
    si.selfSamples = !html.empty();
  }
  if (html.empty()) {
    si.error = "No statement in CodeChef's answer for this problem";
    return si;
  }
  si.html = dollarMath(html::absolutizeUrls(html::removeScripts(html), url));
  si.ok = true;
  return si;
}

StatementInfo parseUsaco(const std::string& doc, const std::string& url) {
  StatementInfo si;
  std::string stmt = html::extractTag(doc, "id=\"probtext-text\"", "span");
  if (stmt.empty()) {
    si.error = "No problem text found on the page";
    return si;
  }
  // The page carries two <h2>s: the contest name, then this problem's title.
  {
    static const std::regex re(R"(<h2[^>]*>([\s\S]*?)</h2>)");
    std::vector<std::string> heads;
    for (auto it = std::sregex_iterator(doc.begin(), doc.end(), re); it != std::sregex_iterator(); ++it)
      heads.push_back(util::trim(html::stripTags((*it)[1])));
    if (heads.size() >= 2) si.title = heads[1];
    else if (!heads.empty()) si.title = heads[0];
  }
  // Samples come as alternating <pre class='in'> / <pre class='out'> blocks.
  {
    static const std::regex re(R"(<pre class=['"](in|out)['"]>([\s\S]*?)</pre>)");
    TestCase cur;
    bool haveIn = false;
    for (auto it = std::sregex_iterator(stmt.begin(), stmt.end(), re); it != std::sregex_iterator(); ++it) {
      std::string body = html::preToText((*it)[2]);
      if ((*it)[1] == "in") {
        if (haveIn) si.samples.push_back(cur);   // an input with no output of its own
        cur = TestCase{};
        cur.in = body;
        haveIn = true;
      } else if (haveIn) {
        cur.out = body;
        si.samples.push_back(cur);
        cur = TestCase{};
        haveIn = false;
      }
    }
    if (haveIn) si.samples.push_back(cur);
  }
  // USACO's section headings are <h4>; give them the look every other judge gets.
  {
    static const std::regex re(R"(<h4[^>]*>([\s\S]*?)</h4>)");
    stmt = std::regex_replace(stmt, re, "<div class=\"section-title\">$1</div>");
  }
  si.selfSamples = true;   // SAMPLE INPUT / SAMPLE OUTPUT are part of the text
  si.html = dollarMath(html::absolutizeUrls(html::removeScripts(stmt), url));
  si.ok = true;
  return si;
}

StatementInfo parseGeneric(const std::string& doc, const std::string& url) {
  StatementInfo si;
  static const std::regex re(R"(<title>([^<]*)</title>)", std::regex::icase);
  std::smatch m;
  if (std::regex_search(doc, m, re)) si.title = util::trim(util::htmlUnescape(m[1]));
  if (util::startsWith(si.title, "CSES - ")) si.title = si.title.substr(7);
  if (util::contains(url, "cses.fi")) {
    std::string body = html::extractDiv(doc, "class=\"md\"");
    if (body.empty()) body = html::extractDiv(doc, "class=\"content\"");
    if (!body.empty()) {
      // limits: <li><b>Time limit:</b> 1.00 s</li>
      static const std::regex tl(R"(Time limit:</b>\s*([\d.]+)\s*s)"), ml(R"(Memory limit:</b>\s*(\d+)\s*MB)");
      if (std::regex_search(doc, m, tl)) si.timeLimitSec = atof(m[1].str().c_str());
      if (std::regex_search(doc, m, ml)) si.memoryMB = atoi(m[1].str().c_str());
      // samples: <p>Input:</p><pre>..</pre><p>Output:</p><pre>..</pre>
      static const std::regex ex(R"(Input:</p>\s*<pre>([\s\S]*?)</pre>\s*<p>Output:</p>\s*<pre>([\s\S]*?)</pre>)");
      for (auto it = std::sregex_iterator(body.begin(), body.end(), ex); it != std::sregex_iterator(); ++it) {
        TestCase tc;
        tc.in = html::preToText((*it)[1]);
        tc.out = html::preToText((*it)[2]);
        si.samples.push_back(tc);
      }
      // drop the example block (rendered from tests) and turn CSES math spans into TeX delimiters
      auto exPos = body.find("<h1 id=\"example\">");
      if (exPos != std::string::npos) body = body.substr(0, exPos);
      static const std::regex mi(R"(<span class="math math-inline">([\s\S]*?)</span>)"), md(R"(<span class="math math-display">([\s\S]*?)</span>)");
      body = std::regex_replace(body, mi, "\\($1\\)");
      body = std::regex_replace(body, md, "\\[$1\\]");
      body = util::replaceAll(body, "<h1 id=\"input\">Input</h1>", "<div class=\"section-title\">Input</div>");
      body = util::replaceAll(body, "<h1 id=\"output\">Output</h1>", "<div class=\"section-title\">Output</div>");
      body = util::replaceAll(body, "<h1 id=\"constraints\">Constraints</h1>", "<div class=\"section-title\">Constraints</div>");
      si.html = html::absolutizeUrls(html::removeScripts(body), url);
      si.ok = true;
      return si;
    }
  }
  si.error = "Statement rendering is not supported for this site yet";
  return si;
}

}  // namespace

// Some sites (Codeforces behind Cloudflare) reject WinHTTP's TLS fingerprint with 403 but accept
// the curl.exe that ships with Windows 10/11, so that is the fallback fetcher.
static bool fetchWithCurl(const std::string& url, int& status, std::string& body) {
  if (!util::isSafeHttpUrl(url)) return false;  // the URL is spliced into a command line below
  std::wstring curlExe;
  if (util::kWindows) {
    std::string sysRoot = util::envVar("SystemRoot");
    if (sysRoot.empty()) sysRoot = "C:\\Windows";
    fs::path curl = util::upath(sysRoot) / "System32" / "curl.exe";
    std::error_code ec;
    if (!fs::exists(curl, ec)) return false;
    curlExe = curl.wstring();
  } else {
    curlExe = L"curl";
  }
  std::wstring cmd = L"\"" + curlExe +
                     L"\" -sS -L --compressed -m 25 -w \"\\n__CP_STATUS__%{http_code}\" "
                     L"-A \"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36\" "
                     L"-H \"Accept-Language: en-US,en;q=0.9\" \"" + util::widen(url) + L"\"";
  auto r = runProcess(cmd, fs::temp_directory_path(), "", 30000);
  if (!r.started) return false;
  auto pos = r.out.rfind("__CP_STATUS__");
  if (pos == std::string::npos) return false;
  status = atoi(r.out.c_str() + pos + 13);
  body = r.out.substr(0, pos);
  if (!body.empty() && body.back() == '\n') body.pop_back();
  return true;
}

bool fetchPage(HttpClient& http, const std::string& url, int& status, std::string& body) {
  if (url.empty() || !util::isSafeHttpUrl(url)) return false;
  auto res = http.get(url);
  status = res.status;
  body = res.body;
  if (!res.error.empty() || status == 403 || status == 503) {
    int st2 = 0;
    std::string b2;
    if (fetchWithCurl(url, st2, b2)) {
      status = st2;
      body = b2;
    } else if (!res.error.empty()) {
      status = 0;
      body = res.error;
      return false;
    }
  }
  return true;
}

StatementInfo fetchStatement(HttpClient& http, const std::string& url, const std::string& judge) {
  StatementInfo si;
  if (url.empty() || !util::isSafeHttpUrl(url)) {
    si.error = url.empty() ? "No URL" : "Unsupported URL";
    return si;
  }
  int status = 0;
  std::string body;
  // CodeChef's page is rendered by JavaScript; its API is what actually holds the
  // statement, so that is what gets fetched.
  std::string fetchUrl = url;
  if (judge == "codechef" && !codechefApiUrl(url, fetchUrl)) {
    si.error = "Not a CodeChef problem URL";
    return si;
  }
  if (!fetchPage(http, fetchUrl, status, body)) {
    si.error = body.empty() ? "Could not reach the site" : body;
    return si;
  }
  if (status != 200) {
    si.error = "HTTP " + std::to_string(status) + (status == 403 ? " (blocked by the site's anti-bot check)" : "");
    return si;
  }
  if (judge == "codeforces") return parseCodeforces(body, url);
  if (judge == "atcoder") return parseAtCoder(body, url);
  if (judge == "usaco") return parseUsaco(body, url);
  if (judge == "codechef") return parseCodeChef(body, url);
  return parseGeneric(body, url);
}
