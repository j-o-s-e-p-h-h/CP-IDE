// Data model shared by storage, runner, judges and the UI bridge.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "util.hpp"

struct TestCase {
  std::string in, out;
  // Fed to the program before `in`, but never shown in the test box. A sample split
  // out of a Codeforces multi-case block keeps the "number of test cases" line here,
  // so the case reads exactly as it does on the site while still running correctly
  // against a solution that starts with `cin >> t`.
  std::string pre;
  bool custom = false;
  std::string status = "idle";  // idle | running | pass | fail | tle | re
  std::string got;
  int ms = 0;
};

struct Problem {
  std::string id;      // tab letter shown in the UI, e.g. "A"
  std::string dir;     // folder name inside the contest folder
  std::string title;   // "Even Split"
  std::string url;
  std::string judge;   // codeforces | atcoder | cses | usaco | hackerrank | other
  std::string group;   // Competitive Companion "group"
  int rating = 0;
  double timeLimitSec = 1.0;
  int memoryMB = 256;
  bool interactive = false;
  std::string statementHtml;  // fetched from the problem page; may be empty
  bool statementExact = false; // statementHtml is the judge's full problem block (own title/samples)
  bool statementSamples = false; // statementHtml already shows the samples (do not append them again)
  int statementVersion = 0;    // bumped when the fetch format changes so old data is refetched
  std::vector<TestCase> tests;
  // per-problem persisted state
  std::string lang = "python";
  int64_t timeSeconds = 0;
  std::string timerMode = "up";  // up | down
  bool paused = false;
  bool solved = false;
  bool attempted = false;
  std::vector<int> bps;
  std::string notes;
  int64_t created = 0;
  // Codeforces identifiers when known
  std::string cfContestId, cfIndex;
};

struct Contest {
  std::string name;
  std::string dir;
  std::string kind = "contest";  // contest | session
  int64_t created = 0;
  // Judge's own schedule, when known: drives the "time left" countdown for a live
  // or virtual round. Both zero means no countdown.
  int64_t startSec = 0, durationSec = 0;
  std::string activeProblem;
  std::vector<Problem> problems;

  Problem* find(const std::string& id) {
    for (auto& p : problems)
      if (p.id == id) return &p;
    return nullptr;
  }
};

struct ContestSummary {
  std::string name, dir, kind;
  int64_t created = 0;
  int solved = 0, total = 0;
};

struct HistoryEntry {
  std::string contest, prob, lang, verdict;
  int64_t at = 0;
  bool ok = false;
  std::string url;
};

inline json toJson(const TestCase& t) {
  return {{"in", t.in}, {"out", t.out}, {"pre", t.pre}, {"custom", t.custom}, {"status", t.status}, {"got", t.got}, {"ms", t.ms}};
}

inline TestCase testFromJson(const json& j) {
  TestCase t;
  t.in = j.value("in", "");
  t.out = j.value("out", "");
  t.pre = j.value("pre", "");
  t.custom = j.value("custom", false);
  t.status = j.value("status", "idle");
  t.got = j.value("got", "");
  t.ms = j.value("ms", 0);
  return t;
}

inline json toJson(const HistoryEntry& h) {
  return {{"contest", h.contest}, {"prob", h.prob}, {"lang", h.lang}, {"verdict", h.verdict},
          {"at", h.at}, {"ok", h.ok}, {"url", h.url}};
}

inline HistoryEntry historyFromJson(const json& j) {
  HistoryEntry h;
  h.contest = j.value("contest", "");
  h.prob = j.value("prob", "");
  h.lang = j.value("lang", "");
  h.verdict = j.value("verdict", "");
  h.at = j.value("at", (int64_t)0);
  h.ok = j.value("ok", false);
  h.url = j.value("url", "");
  return h;
}

// New problems start from <root>/templates/<file>, which is empty until you put
// something there. These are the starters the setup page offers to fill it with.
inline const char* PY_STARTER =
    "import sys\ninput = sys.stdin.readline\n\n\ndef solve():\n    pass\n\n\n"
    "for _ in range(int(input())):\n    solve()\n";
inline const char* CPP_STARTER =
    "#include <bits/stdc++.h>\nusing namespace std;\n\nusing ll = long long;\n\n"
    "void solve() {\n}\n\nint main() {\n    ios::sync_with_stdio(false);\n    cin.tie(nullptr);\n"
    "    int t = 1;\n    cin >> t;\n    while (t--) solve();\n    return 0;\n}\n";
inline const char* JAVA_STARTER =
    "import java.io.*;\nimport java.util.*;\n\npublic class Main {\n"
    "    static BufferedReader in = new BufferedReader(new InputStreamReader(System.in));\n"
    "    static StringBuilder out = new StringBuilder();\n\n"
    "    static void solve() throws IOException {\n    }\n\n"
    "    public static void main(String[] args) throws IOException {\n"
    "        int t = Integer.parseInt(in.readLine().trim());\n"
    "        while (t-- > 0) solve();\n        System.out.print(out);\n    }\n}\n";
inline const char* JS_STARTER =
    "const data = require('fs').readFileSync(0, 'utf8').split('\\n');\nlet ptr = 0;\n"
    "const next = () => data[ptr++];\nconst out = [];\n\nfunction solve() {\n}\n\n"
    "let t = Number(next());\nwhile (t--) solve();\nconsole.log(out.join('\\n'));\n";
inline const char* GEN_TEMPLATE =
    "# gen.py - prints one random test to stdout (used by Stress)\n"
    "import random\n\nt = 1\nprint(t)\nfor _ in range(t):\n    n = random.randint(1, 10)\n    print(n)\n";
inline const char* BRUTE_TEMPLATE =
    "# brute.py - slow but obviously-correct reference solution (used by Stress)\n"
    "import sys\ninput = sys.stdin.readline\n\ndef solve():\n    n = int(input())\n    # brute force here\n\n"
    "t = int(input())\nfor _ in range(t):\n    solve()\n";
