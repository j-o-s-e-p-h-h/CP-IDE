#include "statement.hpp"
#include <regex>
#include "companion.hpp"
#include "runner.hpp"

namespace html {

std::string extractDiv(const std::string& doc, const std::string& marker, size_t* startPos, size_t* endPos) {
  size_t m = doc.find(marker);
  if (m == std::string::npos) return {};
  size_t open = doc.rfind("<div", m);
  if (open == std::string::npos) return {};
  size_t tagEnd = doc.find('>', m);
  if (tagEnd == std::string::npos) return {};
  int depth = 1;
  size_t pos = tagEnd + 1;
  while (depth > 0) {
    size_t a = doc.find("<div", pos);
    size_t b = doc.find("</div", pos);
    if (b == std::string::npos) return {};
    if (a != std::string::npos && a < b) {
      depth++;
      pos = a + 4;
    } else {
      depth--;
      if (depth == 0) {
        size_t close = doc.find('>', b);
        if (startPos) *startPos = open;
        if (endPos) *endPos = close == std::string::npos ? b + 6 : close + 1;
        return doc.substr(tagEnd + 1, b - tagEnd - 1);
      }
      pos = b + 5;
    }
  }
  return {};
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

double parseSeconds(const std::string& text) {
  static const std::regex re(R"(([\d.]+)\s*second)");
  std::smatch m;
  if (std::regex_search(text, m, re)) return atof(m[1].str().c_str());
  return 0;
}

int parseMegabytes(const std::string& text) {
  static const std::regex re(R"((\d+)\s*megabyte)");
  std::smatch m;
  if (std::regex_search(text, m, re)) return atoi(m[1].str().c_str());
  return 0;
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
    auto preAfter = [&](size_t from, size_t& end) -> std::string {
      size_t p = samples.find("<pre", from);
      if (p == std::string::npos) return std::string();
      size_t e = samples.find('>', p);
      size_t q = e == std::string::npos ? std::string::npos : samples.find("</pre>", e);
      if (q == std::string::npos) return std::string();
      end = q + 6;
      return html::preToText(samples.substr(e + 1, q - e - 1));
    };
    size_t pos = 0;
    while (true) {
      size_t a = samples.find("class=\"input\"", pos);
      if (a == std::string::npos) break;
      size_t inEnd = a;
      std::string in = preAfter(a, inEnd);
      size_t outA = samples.find("class=\"output\"", inEnd);
      if (outA == std::string::npos) break;
      size_t outEnd = outA;
      std::string out = preAfter(outA, outEnd);
      if (util::trim(in).empty() && util::trim(out).empty()) break;
      TestCase tc;
      tc.in = in;
      tc.out = out;
      si.samples.push_back(tc);
      pos = outEnd;
    }
  }
  // The whole problem block (header, legend, specs, samples, note) is kept so the
  // Description pane can show the statement exactly as Codeforces renders it.
  si.exact = true;
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

StatementInfo parseAtCoder(const std::string& doc, const std::string& url) {
  StatementInfo si;
  std::string stmt = html::extractDiv(doc, "id=\"task-statement\"");
  if (stmt.empty()) {
    si.error = "No task statement found on the page";
    return si;
  }
  // Prefer the English version when both languages are present.
  size_t en = stmt.find("class=\"lang-en\"");
  if (en != std::string::npos) {
    size_t s = 0, e = 0;
    std::string enHtml = html::extractDiv(stmt, "class=\"lang-en\"", &s, &e);
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
  si.timeLimitSec = parseSeconds(html::stripTags(doc.substr(0, std::min<size_t>(doc.size(), 200000))));
  si.memoryMB = 0;
  {
    static const std::regex re(R"(Memory Limit:\s*(\d+)\s*MB)");
    std::smatch m;
    if (std::regex_search(doc, m, re)) si.memoryMB = atoi(m[1].str().c_str());
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
  si.html = html::absolutizeUrls(html::removeScripts(stmt), url);
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

StatementInfo fetchStatement(HttpClient& http, const std::string& url, const std::string& judge) {
  StatementInfo si;
  if (url.empty() || !util::isSafeHttpUrl(url)) {
    si.error = url.empty() ? "No URL" : "Unsupported URL";
    return si;
  }
  auto res = http.get(url);
  int status = res.status;
  std::string body = res.body;
  if (!res.error.empty() || status == 403 || status == 503) {
    int st2 = 0;
    std::string b2;
    if (fetchWithCurl(url, st2, b2)) {
      status = st2;
      body = b2;
    } else if (!res.error.empty()) {
      si.error = res.error;
      return si;
    }
  }
  if (status != 200) {
    si.error = "HTTP " + std::to_string(status) + (status == 403 ? " (blocked by the site's anti-bot check)" : "");
    return si;
  }
  if (judge == "codeforces") return parseCodeforces(body, url);
  if (judge == "atcoder") return parseAtCoder(body, url);
  return parseGeneric(body, url);
}
