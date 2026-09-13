#include "storage.hpp"
#include <algorithm>

Storage::Storage(fs::path root) : root_(std::move(root)) {
  std::error_code ec;
  fs::create_directories(contestsDir(), ec);
}

json Storage::config() {
  std::lock_guard lk(mu_);
  json def = {
      {"python", "python"},
      {"cppCompiler", ""},  // empty = auto-detect (g++ or clang++); or a full path, chosen on the setup page
      {"cppFlags", "-O2 -std=c++23"},
      {"java", "java"},
      {"javac", "javac"},
      {"node", "node"},
      {"codeforces", {{"handle", ""}, {"password", ""}, {"pythonProgramTypeId", 31}, {"cppProgramTypeId", 91}, {"javaProgramTypeId", 87}, {"jsProgramTypeId", 55}}},
      {"atcoder", {{"pythonLangId", 5055}, {"cppLangId", 5028}, {"javaLangId", 5005}, {"jsLangId", 5009}}},
      {"_help", "Fill codeforces.handle/password to submit from the app. Without them Submit opens the browser and copies your code."}};
  auto j = util::readJson(root_ / "config.json");
  if (j && !j->is_object()) j.reset();
  if (!j) {
    util::writeJson(root_ / "config.json", def);
    return def;
  }
  // merge defaults for missing keys
  for (auto& [k, v] : def.items())
    if (!j->contains(k)) (*j)[k] = v;
  if ((*j)["codeforces"].is_object())
    for (auto& [k, v] : def["codeforces"].items())
      if (!(*j)["codeforces"].contains(k)) (*j)["codeforces"][k] = v;
  return *j;
}

void Storage::saveConfig(const json& j) {
  std::lock_guard lk(mu_);
  util::writeJson(root_ / "config.json", j);
}

json Storage::loadState() {
  std::lock_guard lk(mu_);
  auto j = util::readJson(root_ / "state.json");
  return j && j->is_object() ? *j : json::object();
}

void Storage::saveState(const json& j) {
  std::lock_guard lk(mu_);
  util::writeJson(root_ / "state.json", j);
}

std::vector<ContestSummary> Storage::listContests() {
  std::lock_guard lk(mu_);
  std::vector<ContestSummary> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(contestsDir(), ec)) {
    if (!e.is_directory()) continue;
    auto meta = util::readJson(e.path() / "contest.json");
    if (!meta) continue;
    ContestSummary s;
    s.dir = util::pstr(e.path().filename());
    s.name = meta->value("name", s.dir);
    s.kind = meta->value("kind", "contest");
    s.created = meta->value("created", (int64_t)0);
    for (auto& pe : fs::directory_iterator(e.path(), ec)) {
      if (!pe.is_directory()) continue;
      if (!fs::exists(pe.path() / "problem.json")) continue;
      s.total++;
      auto st = util::readJson(pe.path() / "state.json");
      if (st && st->value("solved", false)) s.solved++;
    }
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.created > b.created; });
  return out;
}

bool Storage::loadProblem(const fs::path& dir, Problem& p) {
  auto meta = util::readJson(dir / "problem.json");
  if (!meta) return false;
  p.dir = util::pstr(dir.filename());
  p.id = meta->value("id", p.dir);
  p.title = meta->value("title", p.dir);
  p.url = meta->value("url", "");
  p.judge = meta->value("judge", "other");
  p.group = meta->value("group", "");
  p.rating = meta->value("rating", 0);
  p.timeLimitSec = meta->value("timeLimitSec", 1.0);
  p.memoryMB = meta->value("memoryMB", 256);
  p.interactive = meta->value("interactive", false);
  p.statementHtml = meta->value("statementHtml", "");
  p.statementExact = meta->value("statementExact", false);
  p.statementVersion = meta->value("statementVersion", 0);
  p.created = meta->value("created", (int64_t)0);
  p.cfContestId = meta->value("cfContestId", "");
  p.cfIndex = meta->value("cfIndex", "");
  auto st = util::readJson(dir / "state.json");
  if (st) {
    p.lang = st->value("lang", "python");
    p.timeSeconds = st->value("timeSeconds", (int64_t)0);
    p.timerMode = st->value("timerMode", "up");
    p.paused = st->value("paused", false);
    p.solved = st->value("solved", false);
    p.attempted = st->value("attempted", false);
    p.notes = st->value("notes", "");
    if (st->contains("bps") && (*st)["bps"].is_array())
      for (auto& b : (*st)["bps"])
        if (b.is_number()) p.bps.push_back(b.get<int>());
  }
  // tests: 1.in/1.out ... then custom1.in/custom1.out ...
  p.tests.clear();
  auto tdir = dir / "tests";
  auto readPair = [&](const std::string& stem, bool custom) -> bool {
    auto in = util::readFile(tdir / (stem + ".in"));
    if (!in) return false;
    auto out = util::readFile(tdir / (stem + ".out"));
    TestCase t;
    t.in = *in;
    t.out = out ? *out : "";
    t.custom = custom;
    p.tests.push_back(t);
    return true;
  };
  for (int i = 1; i < 1000; ++i)
    if (!readPair(std::to_string(i), false)) break;
  for (int i = 1; i < 1000; ++i)
    if (!readPair("custom" + std::to_string(i), true)) break;
  return true;
}

bool Storage::loadContest(const std::string& dir, Contest& out) {
  std::lock_guard lk(mu_);
  auto cdir = contestsDir() / util::upath(dir);
  auto meta = util::readJson(cdir / "contest.json");
  if (!meta) return false;
  out = Contest{};
  out.dir = dir;
  out.name = meta->value("name", dir);
  out.kind = meta->value("kind", "contest");
  out.created = meta->value("created", (int64_t)0);
  out.activeProblem = meta->value("active", "");
  std::vector<std::string> order;
  if (meta->contains("order") && (*meta)["order"].is_array())
    for (auto& o : (*meta)["order"])
      if (o.is_string()) order.push_back(o.get<std::string>());
  std::vector<Problem> found;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(cdir, ec)) {
    if (!e.is_directory()) continue;
    Problem p;
    if (loadProblem(e.path(), p)) found.push_back(std::move(p));
  }
  // keep saved order, then any others by creation time
  for (auto& o : order)
    for (auto it = found.begin(); it != found.end(); ++it)
      if (it->dir == o) {
        out.problems.push_back(std::move(*it));
        found.erase(it);
        break;
      }
  std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.created < b.created; });
  for (auto& p : found) out.problems.push_back(std::move(p));
  return true;
}

Contest Storage::createContest(const std::string& name, const std::string& kind) {
  std::lock_guard lk(mu_);
  Contest c;
  c.name = util::trim(name).empty() ? "Practice Session" : util::trim(name);
  c.kind = kind;
  c.created = util::nowSec();
  std::string base = util::safeName(c.name);
  c.dir = base;
  std::error_code ec;
  for (int i = 2; fs::exists(contestsDir() / util::upath(c.dir), ec); ++i) c.dir = base + " (" + std::to_string(i) + ")";
  fs::create_directories(contestsDir() / util::upath(c.dir), ec);
  json meta = {{"name", c.name}, {"kind", c.kind}, {"created", c.created}, {"active", ""}, {"order", json::array()}};
  util::writeJson(contestsDir() / util::upath(c.dir) / "contest.json", meta);
  return c;
}

void Storage::saveContestMeta(const Contest& c) {
  std::lock_guard lk(mu_);
  json order = json::array();
  for (auto& p : c.problems) order.push_back(p.dir);
  json meta = {{"name", c.name}, {"kind", c.kind}, {"created", c.created}, {"active", c.activeProblem}, {"order", order}};
  util::writeJson(contestDir(c) / "contest.json", meta);
}

bool Storage::deleteContest(const std::string& dir) {
  std::lock_guard lk(mu_);
  if (dir.empty() || util::contains(dir, "..") || util::contains(dir, "/") || util::contains(dir, "\\")) return false;
  std::error_code ec;
  auto path = contestsDir() / util::upath(dir);
  if (!fs::exists(path / "contest.json", ec)) return false;
  fs::remove_all(path, ec);
  return !ec;
}

void Storage::addProblem(Contest& c, Problem p) {
  {
    std::lock_guard lk(mu_);
    if (p.created == 0) p.created = util::nowSec();
    if (p.dir.empty()) p.dir = util::safeName(p.id);
    std::error_code ec;
    std::string base = p.dir;
    for (int i = 2; fs::exists(contestDir(c) / util::upath(p.dir), ec); ++i) p.dir = base + "_" + std::to_string(i);
    auto pdir = contestDir(c) / util::upath(p.dir);
    fs::create_directories(pdir / "tests", ec);
    for (auto& lang : languages())
      if (!fs::exists(pdir / codeFile(lang), ec)) util::writeFile(pdir / codeFile(lang), "");
    if (!fs::exists(pdir / "gen.py", ec)) util::writeFile(pdir / "gen.py", GEN_TEMPLATE);
    if (!fs::exists(pdir / "brute.py", ec)) util::writeFile(pdir / "brute.py", BRUTE_TEMPLATE);
  }
  saveProblemMeta(c, p);
  saveProblemState(c, p);
  saveTests(c, p);
  c.problems.push_back(std::move(p));
  saveContestMeta(c);
}

void Storage::saveProblemMeta(const Contest& c, const Problem& p) {
  std::lock_guard lk(mu_);
  json meta = {{"id", p.id},
               {"title", p.title},
               {"url", p.url},
               {"judge", p.judge},
               {"group", p.group},
               {"rating", p.rating},
               {"timeLimitSec", p.timeLimitSec},
               {"memoryMB", p.memoryMB},
               {"interactive", p.interactive},
               {"statementHtml", p.statementHtml},
               {"statementExact", p.statementExact},
               {"statementVersion", p.statementVersion},
               {"created", p.created},
               {"cfContestId", p.cfContestId},
               {"cfIndex", p.cfIndex}};
  util::writeJson(problemDir(c, p) / "problem.json", meta);
}

void Storage::saveProblemState(const Contest& c, const Problem& p) {
  std::lock_guard lk(mu_);
  json st = {{"lang", p.lang},     {"timeSeconds", p.timeSeconds}, {"timerMode", p.timerMode}, {"paused", p.paused},
             {"solved", p.solved}, {"attempted", p.attempted},     {"notes", p.notes},         {"bps", p.bps}};
  util::writeJson(problemDir(c, p) / "state.json", st);
}

void Storage::saveTests(const Contest& c, const Problem& p) {
  std::lock_guard lk(mu_);
  auto tdir = problemDir(c, p) / "tests";
  std::error_code ec;
  fs::remove_all(tdir, ec);
  fs::create_directories(tdir, ec);
  int s = 0, cu = 0;
  for (auto& t : p.tests) {
    std::string stem = t.custom ? "custom" + std::to_string(++cu) : std::to_string(++s);
    util::writeFile(tdir / (stem + ".in"), t.in);
    util::writeFile(tdir / (stem + ".out"), t.out);
  }
}

std::string Storage::loadCode(const Contest& c, const Problem& p, const std::string& lang) {
  std::lock_guard lk(mu_);
  auto s = util::readFile(problemDir(c, p) / codeFile(lang));
  if (s) return *s;
  return "";
}

void Storage::saveCode(const Contest& c, const Problem& p, const std::string& lang, const std::string& code) {
  std::lock_guard lk(mu_);
  util::writeFile(problemDir(c, p) / codeFile(lang), code);
}

std::vector<HistoryEntry> Storage::loadHistory() {
  std::lock_guard lk(mu_);
  std::vector<HistoryEntry> out;
  auto j = util::readJson(root_ / "history.json");
  if (j && j->is_array())
    for (auto& e : *j) out.push_back(historyFromJson(e));
  return out;
}

void Storage::appendHistory(const HistoryEntry& h) {
  std::lock_guard lk(mu_);
  auto j = util::readJson(root_ / "history.json");
  json arr = (j && j->is_array()) ? *j : json::array();
  arr.push_back(toJson(h));
  util::writeJson(root_ / "history.json", arr);
}
