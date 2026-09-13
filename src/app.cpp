#include "app.hpp"
#include <algorithm>
#include <random>
#include "cf_api.hpp"
#include "companion.hpp"
#ifdef _WIN32
#include <shellapi.h>
#endif
#include "judges/judge_web.hpp"
#include "judges/judge.hpp"
#include "statement.hpp"
#include "webview/webview.h"

namespace {
std::string fmtTl(double s) {
  char buf[32];
  if (s == (int)s) snprintf(buf, sizeof buf, "%d s", (int)s);
  else snprintf(buf, sizeof buf, "%.1f s", s);
  return buf;
}
std::string langLabel(const std::string& lang) {
  if (lang == "cpp") return "C++";
  if (lang == "java") return "Java";
  if (lang == "js") return "JS";
  return "Py3";
}
}  // namespace

App::App(fs::path root, fs::path uiDir, fs::path toolsDir)
    : storage_(std::move(root)), uiDir_(std::move(uiDir)), toolsDir_(std::move(toolsDir)) {
  config_ = storage_.config();
  try {
    tools_ = detectToolchain(config_, tc_);
  } catch (const std::exception&) {  // e.g. a number where config.json expects a string
    tools_ = detectToolchain(json::object(), tc_);
  }
  auto st = storage_.loadState();
  std::string active = st.contains("activeContest") && st["activeContest"].is_string() ? st["activeContest"].get<std::string>() : "";
  if (!active.empty()) openContestDir(active);
  if (!hasContest_) {
    auto list = storage_.listContests();
    if (!list.empty()) openContestDir(list.front().dir);
  }
}

App::~App() { shutdown(); }

void App::shutdown() {
  runCancel_ = true;
  judgeCancel_ = true;
  stress_.stop();
  debugger_.stop();
  if (runThread_.joinable()) runThread_.join();
  if (judgeThread_.joinable()) judgeThread_.join();
  for (int i = 0; i < 100 && bgCount_ > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

void App::enableDevHook() {
  std::random_device rd;
  static const char* hex = "0123456789abcdef";
  devToken_.clear();
  for (int i = 0; i < 32; ++i) devToken_ += hex[rd() % 16];
  util::writeFile(storage_.root() / "dev.token", devToken_);
}

void App::closeAuxWindows() {
  for (auto& [id, w] : judgeWebs_) w->destroyWindow();
}

std::string App::defaultLang() {
  std::string l = storage_.loadState().value("defaultLang", "python");
  for (auto& k : Storage::languages())
    if (k == l) return l;
  return "python";
}

JudgeWeb& App::judgeWeb(const std::string& judgeId) {
  auto& w = judgeWebs_[judgeId];
  if (!w) w = std::make_unique<JudgeWeb>(judgeId, [this](std::function<void()> fn) { wv_->dispatch(std::move(fn)); });
  return *w;
}

void App::spawn(std::function<void()> fn) {
  ++bgCount_;
  std::thread([this, fn = std::move(fn)] {
    try { fn(); } catch (...) {}
    --bgCount_;
  }).detach();
}

void App::emit(const json& ev) {
  if (!wv_) return;
  // Program output can contain bytes that are not UTF-8 (Windows code pages, binary junk);
  // replace them instead of throwing on a worker thread.
  std::string js = "window.__cp && window.__cp.event(" + ev.dump(-1, ' ', false, json::error_handler_t::replace) + ");";
  auto* w = wv_;
  w->dispatch([w, js] { w->eval(js); });
}

std::string App::onDevRequest(const HttpRequest& req) {
  // The dev hook runs JavaScript inside the privileged page. Browsers always send an
  // Origin header on cross-site fetches, so a request carrying one is refused; local
  // tools must also present the per-run token from <root>/dev.token.
  std::string origin = req.header("Origin");
  bool ownOrigin = origin == "http://127.0.0.1:10045" || origin == "http://localhost:10045";
  if ((!origin.empty() && !ownOrigin) || (!devToken_.empty() && req.header("X-CP-Dev") != devToken_ && req.path != "/__result"))
    return "{\"ok\":false,\"error\":\"forbidden\"}";
  if (req.path == "/__result" && req.method == "POST") {
    std::lock_guard lk(devMu_);
    devResult_ = req.body;
    devHasResult_ = true;
    devCv_.notify_all();
    return "{}";
  }
  if (req.path == "/__eval" && req.method == "POST") {
    if (!wv_) return "{\"ok\":false,\"error\":\"no webview\"}";
    {
      std::lock_guard lk(devMu_);
      devHasResult_ = false;
      devResult_.clear();
    }
    std::string js = "(async()=>{let __r;try{__r={ok:true,value:await (async()=>{" + req.body +
                     "})()};}catch(e){__r={ok:false,error:String(e&&e.stack||e)};}"
                     "await fetch('/__result',{method:'POST',body:JSON.stringify(__r)});})();";
    auto* w = wv_;
    w->dispatch([w, js] { w->eval(js); });
    std::unique_lock lk(devMu_);
    if (!devCv_.wait_for(lk, std::chrono::seconds(30), [&] { return devHasResult_; })) return "{\"ok\":false,\"error\":\"timeout\"}";
    return devResult_;
  }
  return "{\"ok\":false,\"error\":\"unknown dev path\"}";
}

// ---------------------------------------------------------------- JSON views
json App::problemJson(const Problem& p) {
  json tests = json::array();
  for (auto& t : p.tests) tests.push_back(toJson(t));
  return {{"id", p.id},
          {"dir", p.dir},
          {"title", p.title},
          {"url", p.url},
          {"judge", p.judge},
          {"rating", p.rating},
          {"tl", fmtTl(p.timeLimitSec)},
          {"ml", std::to_string(p.memoryMB) + " MB"},
          {"tlSec", p.timeLimitSec},
          {"interactive", p.interactive},
          {"statementHtml", p.statementHtml},
          {"statementExact", p.statementExact},
          {"statementVersion", p.statementVersion},
          {"tests", tests},
          {"lang", p.lang},
          {"timeSeconds", p.timeSeconds},
          {"timerMode", p.timerMode},
          {"paused", p.paused},
          {"solved", p.solved},
          {"attempted", p.attempted},
          {"bps", p.bps},
          {"notes", p.notes},
          {"code", {{"python", storage_.loadCode(contest_, p, "python")}, {"cpp", storage_.loadCode(contest_, p, "cpp")},
                    {"java", storage_.loadCode(contest_, p, "java")}, {"js", storage_.loadCode(contest_, p, "js")}}},
          {"path", util::pstr(storage_.problemDir(contest_, p))}};
}

json App::sessionJson() {
  std::lock_guard lk(mu_);
  if (!hasContest_) return nullptr;
  json probs = json::array();
  for (auto& p : contest_.problems) probs.push_back(problemJson(p));
  return {{"name", contest_.name}, {"dir", contest_.dir},       {"kind", contest_.kind},
          {"active", contest_.activeProblem}, {"problems", probs}, {"path", util::pstr(storage_.contestDir(contest_))}};
}

json App::contestsJson() {
  json arr = json::array();
  for (auto& c : storage_.listContests()) {
    bool active = hasContest_ && c.dir == contest_.dir;
    std::string meta = active ? "active" : (c.total == 0 ? "empty" : std::to_string(c.solved) + "/" + std::to_string(c.total) + " solved");
    arr.push_back({{"name", c.name}, {"dir", c.dir}, {"kind", c.kind}, {"date", util::prettyDate(c.created)}, {"meta", meta}, {"active", active}});
  }
  return arr;
}

json App::historyJson() {
  json arr = json::array();
  auto h = storage_.loadHistory();
  for (auto it = h.rbegin(); it != h.rend(); ++it)
    arr.push_back({{"contest", it->contest}, {"prob", it->prob}, {"lang", it->lang}, {"verdict", it->verdict},
                   {"at", util::clockHHMM(it->at)}, {"ok", it->ok}, {"url", it->url}});
  return arr;
}

Problem* App::problemById(const std::string& id) { return hasContest_ ? contest_.find(id) : nullptr; }

std::string App::nextFreeId(const Contest& c, const std::string& wanted) {
  auto used = [&](const std::string& id) {
    for (auto& p : c.problems)
      if (p.id == id) return true;
    return false;
  };
  if (!wanted.empty() && !used(wanted)) return wanted;
  if (!wanted.empty()) {  // keep the judge's letter, e.g. a second "D" becomes "D2"
    for (int i = 2; i < 100; ++i)
      if (!used(wanted + std::to_string(i))) return wanted + std::to_string(i);
  }
  for (char ch = 'A'; ch <= 'Z'; ++ch)
    if (!used(std::string(1, ch))) return std::string(1, ch);
  for (int i = 1; i < 1000; ++i)
    if (!used("P" + std::to_string(i))) return "P" + std::to_string(i);
  return "X";
}

bool App::openContestDir(const std::string& dir) {
  Contest c;
  if (!storage_.loadContest(dir, c)) return false;
  switchToContest(std::move(c));
  return true;
}

void App::switchToContest(Contest c) {
  std::lock_guard lk(mu_);
  contest_ = std::move(c);
  hasContest_ = true;
  if (contest_.activeProblem.empty() && !contest_.problems.empty()) contest_.activeProblem = contest_.problems.front().id;
  auto st = storage_.loadState();
  st["activeContest"] = contest_.dir;
  storage_.saveState(st);
}

Contest* App::contestForIncoming(const Problem& p, int batchSize) {
  std::string groupName = companion::contestNameFromGroup(p.group);
  if (hasContest_) {
    if (batchSize <= 1 && contest_.kind == "session") return &contest_;
    if (contest_.name == groupName) return &contest_;
  }
  // existing contest with that name?
  for (auto& c : storage_.listContests())
    if (c.name == groupName && c.kind == "contest") {
      openContestDir(c.dir);
      return &contest_;
    }
  Contest c = storage_.createContest(groupName, "contest");
  switchToContest(std::move(c));
  return &contest_;
}

void App::addProblemAndFetch(Problem p, const std::string& toastMsg) {
  std::string cdir, pdir, pid;
  {
    std::lock_guard lk(mu_);
    p.id = nextFreeId(contest_, p.id);
    p.lang = defaultLang();
    p.dir = util::safeName(p.id);
    storage_.addProblem(contest_, p);
    contest_.activeProblem = p.id;
    storage_.saveContestMeta(contest_);
    cdir = contest_.dir;
    pdir = contest_.problems.back().dir;
    pid = p.id;
  }
  emit({{"type", "session"}, {"session", sessionJson()}, {"contests", contestsJson()}, {"toast", toast(toastMsg, "var(--ok)")}, {"activate", pid}});
  if (!p.url.empty()) fetchStatementAsync(cdir, pdir, pid);
}

void App::fetchStatementAsync(std::string contestDir, std::string problemDir, std::string problemId) {
  spawn([this, contestDir, problemDir, problemId] {
    std::string url, judge;
    {
      std::lock_guard lk(mu_);
      if (!hasContest_ || contest_.dir != contestDir) return;
      auto* p = contest_.find(problemId);
      if (!p) return;
      url = p->url;
      judge = p->judge;
    }
    HttpClient http;
    auto si = fetchStatement(http, url, judge);
    std::lock_guard lk(mu_);
    if (!hasContest_ || contest_.dir != contestDir) return;
    auto* p = contest_.find(problemId);
    if (!p || p->dir != problemDir) return;
    if (si.ok) {
      p->statementHtml = si.html;
      p->statementExact = si.exact;
      p->statementVersion = 2;
      if (p->rating == 0 && si.rating) p->rating = si.rating;
      if (si.timeLimitSec > 0 && p->timeLimitSec == 1.0) p->timeLimitSec = si.timeLimitSec;
      if (si.memoryMB > 0 && p->memoryMB == 256) p->memoryMB = si.memoryMB;
      // Companion/API imports already carry a good title; URL imports take the page's.
      if (!si.title.empty() && (p->title.empty() || p->group.empty() || util::startsWith(p->title, "Problem "))) p->title = si.title;
      bool onlyBlankTests = std::all_of(p->tests.begin(), p->tests.end(), [](const TestCase& t) { return util::trim(t.in).empty() && util::trim(t.out).empty(); });
      if (onlyBlankTests && !si.samples.empty()) {
        p->tests = si.samples;
        storage_.saveTests(contest_, *p);
      }
      storage_.saveProblemMeta(contest_, *p);
      emit({{"type", "problem"}, {"problem", problemJson(*p)}});
    } else {
      emit({{"type", "statementFailed"}, {"id", problemId}, {"message", si.error}});
    }
  });
}

// ------------------------------------------------------------- Companion
std::string App::onCompanionPost(const HttpRequest& req) {
  try {
    return onCompanionPostImpl(req);
  } catch (const std::exception& e) {
    return json({{"ok", false}, {"error", e.what()}}).dump();
  }
}

std::string App::onCompanionPostImpl(const HttpRequest& req) {
  if (req.body.size() > (4u << 20)) return "{\"ok\":false}";
  auto j = json::parse(req.body, nullptr, false);
  Problem p;
  std::string batchId;
  int batchSize = 1;
  if (j.is_discarded() || !companion::parsePayload(j, p, batchId, batchSize)) return "{\"ok\":false}";
  {
    std::lock_guard lk(mu_);
    contestForIncoming(p, batchSize);
  }
  std::string msg = (p.id.empty() ? "" : p.id + ". ") + p.title + " imported from Competitive Companion — " +
                    std::to_string(p.tests.size()) + " sample test" + (p.tests.size() == 1 ? "" : "s");
  addProblemAndFetch(std::move(p), msg);
  return "{\"ok\":true}";
}

// ------------------------------------------------------------------- RPC
std::string App::rpc(const std::string& name, const json& a) {
  try {
    if (name == "init") return rpcInit().dump();
    if (name == "saveUi") {
      auto st = storage_.loadState();
      for (auto& [k, v] : a.items()) st[k] = v;
      storage_.saveState(st);
      return "{}";
    }
    if (name == "setTool") {
      // Setup page: pick a tool path or the C++ flags; empty path = auto-detect again.
      std::string key = a.value("key", ""), value = util::trim(a.value("value", ""));
      static const std::vector<std::string> allowed = {"python", "cppCompiler", "cppFlags", "javac", "java", "node"};
      if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return "{\"ok\":false}";
      auto cfg = storage_.config();
      cfg[key] = value;
      if (key == "cppCompiler") cfg.erase("gpp");
      storage_.saveConfig(cfg);
      config_ = storage_.config();
      tools_ = detectToolchain(config_, tc_);
      json tools = json::array();
      for (auto& t : tools_) tools.push_back({{"id", t.id}, {"label", t.label}, {"found", t.found}, {"path", t.path}, {"hint", t.hint}});
      return json({{"ok", true}, {"tools", tools}, {"cppCandidates", tc_.cppCandidates}, {"cppFlags", tc_.cppFlags}, {"cppCompiler", tc_.gpp}}).dump();
    }
    if (name == "recheckTools") {
      config_ = storage_.config();
      tools_ = detectToolchain(config_, tc_);
      json tools = json::array();
      for (auto& t : tools_) tools.push_back({{"id", t.id}, {"label", t.label}, {"found", t.found}, {"path", t.path}, {"hint", t.hint}});
      return json({{"tools", tools}, {"cppCandidates", tc_.cppCandidates}, {"cppFlags", tc_.cppFlags}, {"cppCompiler", tc_.gpp}}).dump();
    }
    if (name == "installTool") {
      // Opens a console running the install command so the user can watch/confirm it.
      std::string id = a.value("id", "");
#ifdef _WIN32
      for (auto& t : tools_)
        if (t.id == id && !t.found && util::startsWith(t.hint, "winget ")) {
          std::wstring cmd = L"/k " + util::widen(t.hint) + L" && echo. && echo Done - go back to CP IDE and press Re-check.";
          ShellExecuteW(nullptr, L"open", L"cmd.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
          return "{\"ok\":true}";
        }
#endif
      return json({{"ok", false}, {"error", "Run this in a terminal: " + [&] { for (auto& t : tools_) if (t.id == id) return t.hint; return std::string(); }()}}).dump();
    }
    if (name == "judgeStatus") {
      std::string j = a.value("judge", "codeforces");
      if (!wv_ || !judgeSupportsInApp(j)) return "{}";
      judgeWeb(j).checkLogin(storage_.config(), [this, j](bool loggedIn, const std::string& handle) {
        emit({{"type", "judgeStatus"}, {"judge", j}, {"loggedIn", loggedIn}, {"handle", handle}});
      });
      return "{}";
    }
    if (name == "openContest") {
      std::string dir = a.value("dir", "");
      bool ok = openContestDir(dir);
      return json({{"ok", ok}, {"session", sessionJson()}, {"contests", contestsJson()}}).dump();
    }
    if (name == "deleteContest") {
      std::string dir = a.value("dir", "");
      bool ok;
      {
        std::lock_guard lk(mu_);
        bool wasOpen = hasContest_ && contest_.dir == dir;
        if (wasOpen) {
          runCancel_ = true;
          stress_.stop();
          debugger_.stop();
          hasContest_ = false;
          contest_ = Contest{};
        }
        ok = storage_.deleteContest(dir);
        if (wasOpen) {
          // fall back to the most recent remaining contest, or an empty state
          auto list = storage_.listContests();
          if (!list.empty()) openContestDir(list.front().dir);
          else {
            auto st = storage_.loadState();
            st["activeContest"] = "";
            storage_.saveState(st);
          }
        }
      }
      return json({{"ok", ok}, {"contests", contestsJson()}, {"session", sessionJson()}}).dump();
    }
    if (name == "newSession") return rpcNewSession(a).dump();
    if (name == "setActive") {
      std::lock_guard lk(mu_);
      if (hasContest_) {
        contest_.activeProblem = a.value("id", "");
        storage_.saveContestMeta(contest_);
      }
      return "{}";
    }
    if (name == "saveCode") {
      std::lock_guard lk(mu_);
      if (auto* p = problemById(a.value("id", ""))) storage_.saveCode(contest_, *p, a.value("lang", "python"), a.value("code", ""));
      return "{}";
    }
    if (name == "saveTests") {
      std::lock_guard lk(mu_);
      if (auto* p = problemById(a.value("id", ""))) {
        p->tests.clear();
        if (a.contains("tests"))
          for (auto& t : a["tests"]) p->tests.push_back(testFromJson(t));
        storage_.saveTests(contest_, *p);
      }
      return "{}";
    }
    if (name == "saveState") {
      std::lock_guard lk(mu_);
      if (auto* p = problemById(a.value("id", ""))) {
        applyProblemFields(*p, a);
        storage_.saveProblemState(contest_, *p);
      }
      return "{}";
    }
    if (name == "run") return rpcRun(a).dump();
    if (name == "stopRun") {
      runCancel_ = true;
      return "{}";
    }
    if (name == "submit") return rpcSubmit(a).dump();
    if (name == "stressStart") return rpcStress(a, true).dump();
    if (name == "stressStop") return rpcStress(a, false).dump();
    if (name == "debugStart") return rpcDebugStart(a).dump();
    if (name == "debugCmd") {
      debugger_.command(a.value("cmd", "stop"));
      return "{}";
    }
    if (name == "importUrl") return rpcImportUrl(a).dump();
    if (name == "cfEval") {  // developer hook for the Codeforces window
      if (wv_) judgeWeb(a.value("judge", "codeforces")).devEval(a.value("js", ""));
      return "{}";
    }
    if (name == "cfDev") {
      auto it = judgeWebs_.find(a.value("judge", "codeforces"));
      return json({{"value", it != judgeWebs_.end() ? it->second->lastDev() : ""}}).dump();
    }
    if (name == "judgeLogin") {
      std::string j = a.value("judge", "codeforces");
      if (wv_ && judgeSupportsInApp(j)) judgeWeb(j).showLogin(storage_.config());
      return "{}";
    }
    if (name == "openExternal") {
      openInBrowser(a.value("url", ""));
      return "{}";
    }
    if (name == "openFolder") {
      std::lock_guard lk(mu_);
      fs::path p = storage_.root();
      if (hasContest_ && !a.value("root", false)) {
        p = storage_.contestDir(contest_);
        if (auto* pr = problemById(a.value("id", ""))) p = storage_.problemDir(contest_, *pr);
      }
      openInBrowser(util::pstr(p));
      return "{}";
    }
    if (name == "format") return rpcFormat(a).dump();
    if (name == "refetchStatement") {
      std::lock_guard lk(mu_);
      if (auto* p = problemById(a.value("id", ""))) fetchStatementAsync(contest_.dir, p->dir, p->id);
      return "{}";
    }
    return json({{"error", "unknown rpc: " + name}}).dump();
  } catch (const std::exception& e) {
    return json({{"error", e.what()}}).dump();
  }
}

void App::applyProblemFields(Problem& p, const json& a) {
  if (a.contains("lang")) p.lang = a["lang"].get<std::string>();
  if (a.contains("timeSeconds")) p.timeSeconds = a["timeSeconds"].get<int64_t>();
  if (a.contains("timerMode")) p.timerMode = a["timerMode"].get<std::string>();
  if (a.contains("paused")) p.paused = a["paused"].get<bool>();
  if (a.contains("solved")) p.solved = a["solved"].get<bool>();
  if (a.contains("attempted")) p.attempted = a["attempted"].get<bool>();
  if (a.contains("notes")) p.notes = a["notes"].get<std::string>();
  if (a.contains("bps") && a["bps"].is_array()) {
    p.bps.clear();
    for (auto& b : a["bps"]) if (b.is_number()) p.bps.push_back(b.get<int>());
  }
}

json App::rpcInit() {
  config_ = storage_.config();
  tools_ = detectToolchain(config_, tc_);
  auto cf = config_.value("codeforces", json::object());
  json tools = json::array();
  for (auto& t : tools_) tools.push_back({{"id", t.id}, {"label", t.label}, {"found", t.found}, {"path", t.path}, {"hint", t.hint}});
  return {{"root", util::pstr(storage_.root())},
          {"config", {{"cfHandle", cf.value("handle", "")}, {"cfAuto", !cf.value("handle", "").empty() && !cf.value("password", "").empty()},
                      {"python", tc_.python}, {"gpp", tc_.gpp}, {"cppCompiler", tc_.gpp}, {"cppFlags", tc_.cppFlags},
                      {"cppCandidates", tc_.cppCandidates}, {"configPath", util::pstr(storage_.root() / "config.json")}}},
          {"tools", tools},
          {"ui", storage_.loadState()},
          {"contests", contestsJson()},
          {"session", sessionJson()},
          {"history", historyJson()},
          {"templates", {{"python", PY_TEMPLATE}, {"cpp", CPP_TEMPLATE}, {"java", ""}, {"js", ""}}},
          {"port", 10045}};
}

json App::rpcNewSession(const json& a) {
  std::string name = a.value("name", "");
  std::string mode = a.value("mode", "blank");
  if (util::trim(name).empty()) name = mode == "random" ? "Random practice" : "Practice Session";
  if (mode == "blank") {
    Contest c = storage_.createContest(name, "session");
    switchToContest(std::move(c));
    return {{"ok", true}, {"session", sessionJson()}, {"contests", contestsJson()},
            {"toast", toast("\"" + name + "\" created — add problems via Companion or a URL", "var(--ok)")}};
  }
  if (sessionBusy_) return {{"ok", false}, {"error", "Still creating the previous session"}};
  int lo = a.value("ratingMin", 1200), hi = a.value("ratingMax", 1500);
  if (hi < lo) std::swap(lo, hi);
  std::string url = a.value("url", "");
  if (mode == "url" && util::trim(url).empty()) return {{"ok", false}, {"error", "Paste a problem URL first"}};
  emit({{"type", "busy"}, {"message", mode == "random" ? "Asking the Codeforces API for problems…" : "Fetching the problem…"}});
  sessionBusy_ = true;
  spawn([this, name, mode, lo, hi, url] {
    // Whatever happens, the "busy" overlay must come down and the flag must clear.
    struct Done {
      App* app;
      std::atomic<bool>& b;
      ~Done() {
        b = false;
        if (std::uncaught_exceptions()) {
          app->emit({{"type", "busy"}, {"message", ""}});
          app->emit({{"type", "toast"}, {"toast", app->toast("Could not create the session (unexpected error)", "var(--bad)")}});
        }
      }
    } done{this, sessionBusy_};
    HttpClient http;
    std::vector<Problem> probs;
    std::string error;
    if (mode == "random") {
      auto pool = cfapi::problemsInRange(http, lo, hi, error);
      if (error.empty() && pool.empty()) error = "No Codeforces problems rated " + std::to_string(lo) + "–" + std::to_string(hi);
      if (error.empty()) {
        auto cf = config_.value("codeforces", json::object());
        auto solved = cfapi::solvedBy(http, cf.value("handle", ""));
        auto picked = cfapi::pickRandom(pool, solved, 3);
        char letter = 'A';
        for (auto& ap : picked) {
          Problem p;
          p.id = std::string(1, letter++);
          p.title = ap.name;
          p.url = cfapi::problemUrl(ap.contestId, ap.index);
          p.judge = "codeforces";
          p.rating = ap.rating;
          p.cfContestId = ap.contestId;
          p.cfIndex = ap.index;
          p.group = "Codeforces";
          auto si = fetchStatement(http, p.url, "codeforces");
          if (si.ok) {
            p.statementHtml = si.html;
            p.statementExact = si.exact;
            p.statementVersion = 2;
            if (si.timeLimitSec > 0) p.timeLimitSec = si.timeLimitSec;
            if (si.memoryMB > 0) p.memoryMB = si.memoryMB;
            p.tests = si.samples;
          }
          probs.push_back(p);
        }
      }
    } else {
      Problem p;
      p.url = util::trim(url);
      p.judge = companion::judgeForUrl(p.url);
      p.id = "A";
      if (p.judge == "codeforces") companion::parseCodeforcesUrl(p.url, p.cfContestId, p.cfIndex);
      auto si = fetchStatement(http, p.url, p.judge);
      if (si.ok) {
        p.statementHtml = si.html;
            p.statementExact = si.exact;
            p.statementVersion = 2;
        if (!si.title.empty()) p.title = si.title;
        if (!si.index.empty()) p.id = si.index;
        p.rating = si.rating;
        if (si.timeLimitSec > 0) p.timeLimitSec = si.timeLimitSec;
        if (si.memoryMB > 0) p.memoryMB = si.memoryMB;
        p.tests = si.samples;
      } else {
        error = si.error;
      }
      if (p.title.empty()) p.title = p.judge == "codeforces" && !p.cfContestId.empty() ? "Problem " + p.cfContestId + p.cfIndex : "Problem from URL";
      if (!p.cfIndex.empty()) p.id = p.cfIndex;
      probs.push_back(p);
      if (!error.empty() && p.judge != "other") error.clear();  // still create the problem; statement can be retried
    }
    if (!error.empty()) {
      emit({{"type", "busy"}, {"message", ""}});
      emit({{"type", "toast"}, {"toast", toast(error, "var(--bad)")}});
      return;
    }
    Contest c = storage_.createContest(name, "session");
    for (auto& p : probs) {
      std::lock_guard lk(mu_);
      p.dir = util::safeName(p.id);
      p.lang = defaultLang();
      storage_.addProblem(c, p);
    }
    if (!c.problems.empty()) c.activeProblem = c.problems.front().id;
    storage_.saveContestMeta(c);
    switchToContest(std::move(c));
    emit({{"type", "busy"}, {"message", ""}});
    emit({{"type", "session"}, {"session", sessionJson()}, {"contests", contestsJson()},
          {"toast", toast("\"" + name + "\" created with " + std::to_string(probs.size()) + " problem" + (probs.size() == 1 ? "" : "s"), "var(--ok)")}});
  });
  return {{"ok", true}, {"async", true}};
}

json App::rpcImportUrl(const json& a) {
  std::string url = util::trim(a.value("url", ""));
  if (url.empty()) return {{"ok", false}, {"error", "Paste a problem URL first"}};
  {
    std::lock_guard lk(mu_);
    if (!hasContest_) {
      Contest c = storage_.createContest("Practice Session", "session");
      switchToContest(std::move(c));
    }
  }
  spawn([this, url] {
    HttpClient http;
    Problem p;
    p.url = url;
    p.judge = companion::judgeForUrl(url);
    if (p.judge == "codeforces") companion::parseCodeforcesUrl(url, p.cfContestId, p.cfIndex);
    auto si = fetchStatement(http, url, p.judge);
    if (si.ok) {
      p.statementHtml = si.html;
            p.statementExact = si.exact;
            p.statementVersion = 2;
      p.title = si.title;
      p.id = si.index;
      p.rating = si.rating;
      if (si.timeLimitSec > 0) p.timeLimitSec = si.timeLimitSec;
      if (si.memoryMB > 0) p.memoryMB = si.memoryMB;
      p.tests = si.samples;
    }
    if (!p.cfIndex.empty() && p.id.empty()) p.id = p.cfIndex;
    if (p.title.empty()) p.title = !p.cfContestId.empty() ? "Problem " + p.cfContestId + p.cfIndex : "Problem from URL";
    std::string msg = (p.id.empty() ? "" : p.id + ". ") + p.title + " imported from URL" +
                      (si.ok ? " — " + std::to_string(p.tests.size()) + " sample tests" : " (statement unavailable: " + si.error + ")");
    addProblemAndFetch(std::move(p), msg);
  });
  return {{"ok", true}, {"async", true}};
}

json App::rpcRun(const json& a) {
  if (running_) return {{"ok", false}, {"error", "Already running"}};
  std::string id = a.value("id", ""), lang = a.value("lang", "python");
  fs::path dir;
  std::vector<TestCase> tests;
  double tl = 1.0;
  {
    std::lock_guard lk(mu_);
    auto* p = problemById(id);
    if (!p) return {{"ok", false}, {"error", "No problem selected"}};
    if (a.contains("code")) storage_.saveCode(contest_, *p, lang, a["code"].get<std::string>());
    if (a.contains("tests") && a["tests"].is_array()) {
      p->tests.clear();
      for (auto& t : a["tests"]) p->tests.push_back(testFromJson(t));
      storage_.saveTests(contest_, *p);
    }
    p->lang = lang;
    p->attempted = true;
    storage_.saveProblemState(contest_, *p);
    dir = storage_.problemDir(contest_, *p);
    tests = p->tests;
    tl = p->timeLimitSec;
  }
  if (tests.empty()) return {{"ok", false}, {"error", "No test cases — add one first"}};
  if (runThread_.joinable()) runThread_.join();
  running_ = true;
  runCancel_ = false;
  Toolchain tc = tc_;
  runThread_ = std::thread([this, tc, id, lang, dir, tests, tl] {
    // An exception here (folder deleted mid-run, odd filesystem state) must end as a
    // failed run, not as std::terminate of the whole IDE.
    struct Done {
      App* app;
      std::string id;
      int total;
      ~Done() {
        if (std::uncaught_exceptions())
          app->emit({{"type", "runDone"}, {"id", id}, {"ok", false}, {"compileError", "Run failed unexpectedly (see the problem folder)"}, {"passed", 0}, {"total", total}, {"ms", 0}});
        app->running_ = false;
      }
    } done{this, id, (int)tests.size()};
    int64_t t0 = util::nowMs();
    if (Toolchain::needsCompile(lang)) {
      emit({{"type", "compile"}, {"id", id}, {"state", "start"}});
      auto cr = compileFor(tc, lang, dir, &runCancel_);
      if (!cr.ok) {
        emit({{"type", "runDone"}, {"id", id}, {"ok", false}, {"compileError", cr.log}, {"passed", 0}, {"total", (int)tests.size()}, {"ms", cr.ms}});
        running_ = false;
        return;
      }
      emit({{"type", "compile"}, {"id", id}, {"state", "done"}, {"ms", cr.ms}});
    }
    int passed = 0, maxMs = 0;
    bool cancelled = false;
    for (size_t i = 0; i < tests.size(); ++i) {
      if (runCancel_) { cancelled = true; break; }
      emit({{"type", "testStatus"}, {"id", id}, {"index", (int)i}, {"status", "running"}});
      auto v = runTest(tc, lang, dir, tests[i], tl, &runCancel_);
      if (runCancel_) { cancelled = true; break; }
      if (v.status == "pass") passed++;
      maxMs = std::max(maxMs, v.ms);
      emit({{"type", "testStatus"}, {"id", id}, {"index", (int)i}, {"status", v.status}, {"got", v.got}, {"ms", v.ms}});
    }
    emit({{"type", "runDone"}, {"id", id}, {"ok", !cancelled && passed == (int)tests.size()}, {"passed", passed}, {"total", (int)tests.size()},
          {"ms", maxMs}, {"totalMs", (int)(util::nowMs() - t0)}, {"cancelled", cancelled}});
    running_ = false;
  });
  return {{"ok", true}};
}

void App::recordVerdict(const std::string& probId, const std::string& lang, const std::string& verdict, bool ok, const std::string& url) {
  std::lock_guard lk(mu_);
  HistoryEntry h;
  h.contest = hasContest_ ? contest_.name : "";
  h.prob = probId;
  h.lang = langLabel(lang);
  h.verdict = verdict;
  h.ok = ok;
  h.at = util::nowSec();
  h.url = url;
  storage_.appendHistory(h);
  if (auto* p = problemById(probId)) {
    p->attempted = true;
    if (ok) p->solved = true;
    storage_.saveProblemState(contest_, *p);
  }
}

json App::rpcSubmit(const json& a) {
  bool webBusy = false;
  for (auto& [jid, w] : judgeWebs_) webBusy = webBusy || w->busy();
  if (judging_ && !webBusy) return {{"ok", false}, {"error", "Already judging"}};
  for (auto& [jid, w] : judgeWebs_) if (w->busy()) w->cancel();  // a new Submit replaces one still waiting for the user
  std::string id = a.value("id", ""), lang = a.value("lang", "python");
  Problem snapshot;
  std::string code;
  {
    std::lock_guard lk(mu_);
    auto* p = problemById(id);
    if (!p) return {{"ok", false}, {"error", "No problem selected"}};
    code = a.value("code", storage_.loadCode(contest_, *p, lang));
    storage_.saveCode(contest_, *p, lang, code);
    p->lang = lang;
    storage_.saveProblemState(contest_, *p);
    snapshot = *p;
    config_ = storage_.config();
  }
  if (judgeThread_.joinable()) judgeThread_.join();
  judging_ = true;
  judgeCancel_ = false;
  json config = config_;
  if (judgeSupportsInApp(snapshot.judge) && wv_) {
    // In-app submission through the judge's window (UI thread). A throw here (e.g. a
    // mistyped id in config.json) must not leave judging_ stuck.
    struct ResetOnThrow { std::atomic<bool>& f; bool armed = true; ~ResetOnThrow() { if (armed) f = false; } } guard{judging_};
    std::string pid = snapshot.id, url = snapshot.url;
    judgeWeb(snapshot.judge).submit(
        SubmitRequest{snapshot, lang, code}, config,
        [this, pid](const SubmitProgress& sp) {
          emit({{"type", "judge"}, {"id", pid}, {"state", sp.state}, {"message", sp.message}, {"verdict", sp.verdict}, {"ok", sp.ok}});
        },
        [this, pid, lang, url](const SubmitProgress& sp) {
          judging_ = false;
          if (sp.state == "done") {
            recordVerdict(pid, lang, sp.verdict, sp.ok, url);
            emit({{"type", "judge"}, {"id", pid}, {"state", "done"}, {"message", sp.message}, {"verdict", sp.verdict}, {"ok", sp.ok},
                  {"history", historyJson()}});
          } else {
            emit({{"type", "judge"}, {"id", pid}, {"state", "error"}, {"message", sp.message}});
          }
        });
    guard.armed = false;
    return {{"ok", true}};
  }
  judgeThread_ = std::thread([this, snapshot, lang, code, config] {
    SubmitRequest req{snapshot, lang, code};
    auto judge = makeJudge(snapshot.judge);
    auto progress = [&](const SubmitProgress& sp) {
      emit({{"type", "judge"}, {"id", snapshot.id}, {"state", sp.state}, {"message", sp.message}, {"verdict", sp.verdict}, {"ok", sp.ok}});
    };
    auto finish = [&](const SubmitProgress& sp) {
      recordVerdict(snapshot.id, lang, sp.verdict, sp.ok, snapshot.url);
      emit({{"type", "judge"}, {"id", snapshot.id}, {"state", "done"}, {"message", sp.message}, {"verdict", sp.verdict}, {"ok", sp.ok},
            {"history", historyJson()}});
    };
    std::string note;
    if (judge->canAutoSubmit(config)) {
      auto sp = judge->submit(req, config, progress, judgeCancel_);
      if (sp.state == "done") {
        finish(sp);
        judging_ = false;
        return;
      }
      note = sp.message;
    }
    // Browser fallback: copy code, open the submit page, then poll if we can.
    copyToClipboard(code);
    openInBrowser(judge->submitUrl(snapshot));
    std::string msg = (note.empty() ? "" : note + ". ") + "Opened " + judge->name() + " in your browser — code copied to the clipboard, paste and submit.";
    emit({{"type", "judge"}, {"id", snapshot.id}, {"state", "browser"}, {"message", msg}});
    SubmitProgress final;
    int64_t since = util::nowSec();
    if (judge->pollAfterBrowserSubmit(req, config, since, progress, judgeCancel_, final) && final.state == "done") {
      finish(final);
    } else {
      emit({{"type", "judge"}, {"id", snapshot.id}, {"state", "idle"}, {"message", ""}});
    }
    judging_ = false;
  });
  return {{"ok", true}};
}

json App::rpcStress(const json& a, bool start) {
  if (!start) {
    stress_.stop();
    return {{"ok", true}};
  }
  std::string id = a.value("id", ""), lang = a.value("lang", "python");
  fs::path dir;
  double tl = 1.0;
  {
    std::lock_guard lk(mu_);
    auto* p = problemById(id);
    if (!p) return {{"ok", false}, {"error", "No problem selected"}};
    if (a.contains("code")) storage_.saveCode(contest_, *p, lang, a["code"].get<std::string>());
    dir = storage_.problemDir(contest_, *p);
    tl = p->timeLimitSec;
  }
  stress_.start(tc_, dir, lang, tl, [this, id](const StressEvent& ev) {
    emit({{"type", "stress"}, {"id", id}, {"state", ev.state}, {"iteration", ev.iteration}, {"input", ev.input},
          {"expected", ev.expected}, {"got", ev.got}, {"message", ev.message}});
  });
  return {{"ok", true}};
}

json App::rpcDebugStart(const json& a) {
  std::string id = a.value("id", ""), lang = a.value("lang", "python");
  if (lang != "python" && lang != "cpp") return {{"ok", false}, {"error", "The debugger supports Python and C++ for now"}};
  fs::path dir;
  std::string stdinData;
  std::vector<int> bps;
  {
    std::lock_guard lk(mu_);
    auto* p = problemById(id);
    if (!p) return {{"ok", false}, {"error", "No problem selected"}};
    if (a.contains("code")) storage_.saveCode(contest_, *p, lang, a["code"].get<std::string>());
    if (a.contains("bps"))
      for (auto& b : a["bps"]) if (b.is_number()) bps.push_back(b.get<int>());
    dir = storage_.problemDir(contest_, *p);
    int ti = a.value("testIndex", 0);
    if (ti >= 0 && ti < (int)p->tests.size()) stdinData = p->tests[ti].in;
  }
  std::string err;
  bool ok = debugger_.start(tc_, lang, dir, toolsDir_, bps, stdinData,
                            [this, id](const json& ev) {
                              json e = ev;
                              e["type"] = "debug";
                              e["event"] = ev.value("type", "");
                              e["id"] = id;
                              emit(e);
                            },
                            err);
  return {{"ok", ok}, {"error", err}};
}

json App::rpcFormat(const json& a) {
  std::string lang = a.value("lang", "python"), code = a.value("code", "");
  if (lang != "python" && lang != "cpp") return {{"ok", false}, {"message", "Formatting is available for Python (black) and C++ (clang-format)"}};
  fs::path tmp = fs::temp_directory_path() / (lang == "cpp" ? "cp_ide_fmt.cpp" : "cp_ide_fmt.py");
  util::writeFile(tmp, code);
  std::wstring cmd;
  if (lang == "cpp") {
    std::string cf = "clang-format";
    fs::path guess = util::upath(tc_.gpp).parent_path() / "clang-format.exe";
    std::error_code ec;
    if (fs::exists(guess, ec)) cf = util::pstr(guess);
    cmd = tc_.quote(cf) + L" --style=\"{BasedOnStyle: Google, IndentWidth: 4, ColumnLimit: 100}\" " + tc_.quote(util::pstr(tmp));
    auto r = runProcess(cmd, tmp.parent_path(), "", 20000);
    if (!r.started || r.exitCode != 0) return {{"ok", false}, {"message", "clang-format not available (install it with pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra)"}};
    return {{"ok", true}, {"code", r.out}};
  }
  cmd = tc_.quote(tc_.python) + L" -m black -q - ";
  auto r = runProcess(cmd, tmp.parent_path(), code, 20000);
  if (!r.started || r.exitCode != 0) return {{"ok", false}, {"message", "black not available (pip install black)"}};
  return {{"ok", true}, {"code", r.out}};
}
