// On-disk layout:
//   <root>/config.json                     tool paths + judge credentials
//   <root>/state.json                      active contest, layout, theme
//   <root>/history.json                    submission history
//   <root>/contests/<contest>/contest.json
//   <root>/contests/<contest>/<problem>/{problem.json,state.json,main.py,main.cpp,gen.py,brute.py,tests/}
//   <root>/templates/{main.py,main.cpp,Main.java,main.js}   your boilerplate, copied into every new problem
#pragma once
#include <mutex>
#include "model.hpp"

class Storage {
 public:
  explicit Storage(fs::path root);
  const fs::path& root() const { return root_; }
  fs::path contestsDir() const { return root_ / "contests"; }
  fs::path templatesDir() const { return root_ / "templates"; }
  fs::path contestDir(const Contest& c) const { return contestsDir() / util::upath(c.dir); }
  fs::path problemDir(const Contest& c, const Problem& p) const { return contestDir(c) / util::upath(p.dir); }

  // config
  json config();
  void saveConfig(const json& j);

  // global ui state
  json loadState();
  void saveState(const json& j);

  // contests
  std::vector<ContestSummary> listContests();
  bool loadContest(const std::string& dir, Contest& out);
  Contest createContest(const std::string& name, const std::string& kind);
  void saveContestMeta(const Contest& c);
  bool deleteContest(const std::string& dir);

  // problems
  void addProblem(Contest& c, Problem p);  // creates folder, files, tests; appends to c.problems
  bool deleteProblem(const Contest& c, const std::string& dir);  // removes the problem folder
  void saveProblemMeta(const Contest& c, const Problem& p);
  void saveProblemState(const Contest& c, const Problem& p);
  void saveTests(const Contest& c, const Problem& p);
  std::string loadCode(const Contest& c, const Problem& p, const std::string& lang);
  void saveCode(const Contest& c, const Problem& p, const std::string& lang, const std::string& code);
  static std::string codeFile(const std::string& lang) {
    if (lang == "cpp") return "main.cpp";
    if (lang == "java") return "Main.java";  // javac needs the file named after the public class
    if (lang == "js") return "main.js";
    return "main.py";
  }
  static const std::vector<std::string>& languages() {
    static const std::vector<std::string> l = {"python", "cpp", "java", "js"};
    return l;
  }

  // code templates: the boilerplate every new problem starts from (empty by default)
  json loadTemplates();
  void saveTemplate(const std::string& lang, const std::string& code);

  // history
  std::vector<HistoryEntry> loadHistory();
  void appendHistory(const HistoryEntry& h);

 private:
  fs::path root_;
  std::mutex mu_;
  bool loadProblem(const fs::path& dir, Problem& p);
  std::string templateFor(const std::string& lang);  // caller holds mu_
};
