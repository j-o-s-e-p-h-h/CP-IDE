// Codeforces: login with handle/password from config.json, submit through the
// website form, then poll api/user.status for the verdict.
#include <chrono>
#include <random>
#include <regex>
#include <thread>
#include "../http_client.hpp"
#include "../statement.hpp"
#include "judge.hpp"

namespace {

std::string randomToken(size_t n) {
  static const char* alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device rd;
  std::mt19937 rng(rd());
  std::string s;
  for (size_t i = 0; i < n; ++i) s += alphabet[rng() % 36];
  return s;
}

std::string csrfFrom(const std::string& body) {
  static const std::regex re1(R"rx(name="csrf_token"\s+value="([0-9a-f]+)")rx");
  static const std::regex re2(R"(data-csrf='([0-9a-f]+)')");
  static const std::regex re3(R"rx(<meta name="X-Csrf-Token" content="([0-9a-f]+)")rx");
  std::smatch m;
  if (std::regex_search(body, m, re1) || std::regex_search(body, m, re2) || std::regex_search(body, m, re3)) return m[1];
  return {};
}

std::string verdictText(const json& sub) {
  std::string v = sub.value("verdict", "TESTING");
  int passed = sub.value("passedTestCount", 0);
  std::string testset = sub.value("testset", "TESTS");
  std::string tn = std::string(testset == "PRETESTS" ? "pretest " : "test ") + std::to_string(passed + 1);
  if (v == "OK") return "Accepted";
  if (v == "WRONG_ANSWER") return "Wrong answer on " + tn;
  if (v == "TIME_LIMIT_EXCEEDED") return "Time limit exceeded on " + tn;
  if (v == "MEMORY_LIMIT_EXCEEDED") return "Memory limit exceeded on " + tn;
  if (v == "RUNTIME_ERROR") return "Runtime error on " + tn;
  if (v == "COMPILATION_ERROR") return "Compilation error";
  if (v == "IDLENESS_LIMIT_EXCEEDED") return "Idleness limit exceeded on " + tn;
  if (v == "PARTIAL") return "Partial";
  if (v == "SKIPPED") return "Skipped";
  if (v == "REJECTED") return "Rejected";
  if (v == "CHALLENGED") return "Hacked";
  if (v == "TESTING") return "Running on " + tn;
  return v;
}

class CodeforcesJudge : public Judge {
 public:
  std::string name() const override { return "Codeforces"; }

  bool canAutoSubmit(const json& config) const override {
    auto cf = config.value("codeforces", json::object());
    return !cf.value("handle", "").empty() && !cf.value("password", "").empty();
  }

  std::string submitUrl(const Problem& p) const override {
    if (!p.cfContestId.empty()) {
      std::string u = "https://codeforces.com/contest/" + p.cfContestId + "/submit/" + p.cfIndex;
      if (util::contains(p.url, "/gym/")) u = "https://codeforces.com/gym/" + p.cfContestId + "/submit/" + p.cfIndex;
      return u;
    }
    return p.url;
  }

  bool login(HttpClient& http, const std::string& handle, const std::string& password, std::string& err) {
    auto page = http.get("https://codeforces.com/enter");
    if (!page.ok()) {
      err = "Could not open codeforces.com/enter (" + (page.error.empty() ? "HTTP " + std::to_string(page.status) : page.error) + ")";
      return false;
    }
    std::string csrf = csrfFrom(page.body);
    if (csrf.empty()) {
      err = "Login page has no CSRF token (site layout changed or bot check)";
      return false;
    }
    std::string ftaa = randomToken(18);
    std::string body = "csrf_token=" + csrf + "&action=enter&ftaa=" + ftaa + "&bfaa=" + bfaa_ +
                       "&handleOrEmail=" + util::urlEncode(handle) + "&password=" + util::urlEncode(password) +
                       "&_tta=176&remember=on";
    auto res = http.post("https://codeforces.com/enter", body, "application/x-www-form-urlencoded",
                         {{"Referer", "https://codeforces.com/enter"}, {"Origin", "https://codeforces.com"}});
    if (!res.ok()) {
      err = "Login request failed (" + (res.error.empty() ? "HTTP " + std::to_string(res.status) : res.error) + ")";
      return false;
    }
    if (!util::contains(res.body, "/logout")) {
      err = util::contains(res.body, "Invalid handle or password") ? "Invalid handle or password" : "Login did not succeed";
      return false;
    }
    ftaa_ = ftaa;
    return true;
  }

  SubmitProgress submit(const SubmitRequest& req, const json& config, const std::function<void(const SubmitProgress&)>& progress,
                        std::atomic<bool>& cancel) override {
    SubmitProgress sp;
    auto cf = config.value("codeforces", json::object());
    std::string handle = cf.value("handle", ""), password = cf.value("password", "");
    int programType = req.lang == "cpp"    ? cf.value("cppProgramTypeId", 91)
                      : req.lang == "java" ? cf.value("javaProgramTypeId", 87)
                      : req.lang == "js"   ? cf.value("jsProgramTypeId", 55)
                                           : cf.value("pythonProgramTypeId", 31);
    const Problem& p = req.problem;
    if (p.cfContestId.empty() || p.cfIndex.empty()) {
      sp.state = "error";
      sp.message = "Unknown Codeforces contest/problem id";
      return sp;
    }
    HttpClient http;
    sp.state = "judging";
    sp.message = "Logging in as " + handle + "...";
    progress(sp);
    std::string err;
    if (!login(http, handle, password, err)) {
      sp.state = "error";
      sp.message = err;
      return sp;
    }
    std::string base = util::contains(p.url, "/gym/") ? "https://codeforces.com/gym/" : "https://codeforces.com/contest/";
    std::string submitPage = base + p.cfContestId + "/submit";
    auto page = http.get(submitPage);
    std::string csrf = csrfFrom(page.body);
    if (!page.ok() || csrf.empty()) {
      sp.state = "error";
      sp.message = "Could not open the submit page (HTTP " + std::to_string(page.status) + ")";
      return sp;
    }
    sp.message = "Submitting...";
    progress(sp);
    int64_t t0 = util::nowSec();
    std::string boundary = "----CPIDEBoundary" + randomToken(12);
    auto field = [&](const std::string& n, const std::string& v) {
      return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + n + "\"\r\n\r\n" + v + "\r\n";
    };
    std::string body;
    body += field("csrf_token", csrf);
    body += field("ftaa", ftaa_);
    body += field("bfaa", bfaa_);
    body += field("action", "submitSolutionFormSubmitted");
    body += field("contestId", p.cfContestId);
    body += field("submittedProblemIndex", p.cfIndex);
    body += field("programTypeId", std::to_string(programType));
    body += field("source", req.code);
    body += field("tabSize", "4");
    body += field("_tta", "176");
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"sourceFile\"; filename=\"\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n";
    body += "--" + boundary + "--\r\n";
    auto res = http.post(submitPage + "?csrf_token=" + csrf, body, "multipart/form-data; boundary=" + boundary,
                         {{"Referer", submitPage}, {"Origin", "https://codeforces.com"}});
    if (!res.ok()) {
      sp.state = "error";
      sp.message = "Submit request failed (HTTP " + std::to_string(res.status) + ")";
      return sp;
    }
    if (util::contains(res.body, "You have submitted exactly the same code before")) {
      sp.state = "error";
      sp.message = "Codeforces rejected it: exactly the same code was submitted before";
      return sp;
    }
    if (util::contains(res.body, "error for__source") || util::contains(res.body, "Source code should be")) {
      sp.state = "error";
      sp.message = "Codeforces rejected the source (too short/empty?)";
      return sp;
    }
    bool okPage = util::contains(res.finalUrl, "/my") || util::contains(res.finalUrl, "/status");
    if (!okPage) {
      sp.state = "error";
      sp.message = "Submit did not go through (no redirect to the submissions page)";
      return sp;
    }
    sp.message = "Submitted - waiting for the verdict...";
    progress(sp);
    SubmitProgress final;
    if (pollAfterBrowserSubmit(req, config, t0 - 5, progress, cancel, final)) return final;
    sp.state = "error";
    sp.message = "Timed out waiting for the verdict";
    return sp;
  }

  bool pollAfterBrowserSubmit(const SubmitRequest& req, const json& config, int64_t sinceEpoch,
                              const std::function<void(const SubmitProgress&)>& progress, std::atomic<bool>& cancel,
                              SubmitProgress& final) override {
    auto cf = config.value("codeforces", json::object());
    std::string handle = cf.value("handle", "");
    if (handle.empty()) return false;
    const Problem& p = req.problem;
    HttpClient http;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(6);
    std::string lastMsg;
    while (std::chrono::steady_clock::now() < deadline && !cancel) {
      auto res = http.get("https://codeforces.com/api/user.status?handle=" + util::urlEncode(handle) + "&from=1&count=10");
      if (res.ok()) {
        auto j = json::parse(res.body, nullptr, false);
        if (!j.is_discarded() && j.value("status", "") == "OK") {
          for (auto& sub : j["result"]) {
            auto& pr = sub["problem"];
            if (!pr.contains("contestId")) continue;
            if (std::to_string(pr["contestId"].get<int64_t>()) != p.cfContestId || pr.value("index", "") != p.cfIndex) continue;
            if (sub.value("creationTimeSeconds", (int64_t)0) < sinceEpoch) continue;
            std::string v = sub.value("verdict", "TESTING");
            std::string text = verdictText(sub);
            if (v == "TESTING" || !sub.contains("verdict")) {
              if (text != lastMsg) {
                SubmitProgress sp;
                sp.state = "judging";
                sp.message = text;
                progress(sp);
                lastMsg = text;
              }
              break;
            }
            final.state = "done";
            final.verdict = text;
            final.ok = v == "OK";
            final.message = text;
            return true;
          }
        }
      }
      for (int i = 0; i < 20 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

 private:
  std::string ftaa_ = randomToken(18);
  std::string bfaa_ = "f1b3f18c715565b589b7823cda7448ce";
};

}  // namespace

std::unique_ptr<Judge> makeCodeforcesJudge() { return std::make_unique<CodeforcesJudge>(); }
