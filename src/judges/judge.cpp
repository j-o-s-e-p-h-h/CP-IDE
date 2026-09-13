#include "judge.hpp"
#ifdef _WIN32
#include <shellapi.h>
#endif
#include "../companion.hpp"

std::unique_ptr<Judge> makeCodeforcesJudge();

namespace {

// Stub judges: browser submit only. Each knows its submit page.
class BrowserOnlyJudge : public Judge {
 public:
  BrowserOnlyJudge(std::string name, std::function<std::string(const Problem&)> urlFn)
      : name_(std::move(name)), urlFn_(std::move(urlFn)) {}
  std::string name() const override { return name_; }
  bool canAutoSubmit(const json&) const override { return false; }
  std::string submitUrl(const Problem& p) const override { return urlFn_(p); }
  SubmitProgress submit(const SubmitRequest&, const json&, const std::function<void(const SubmitProgress&)>&,
                        std::atomic<bool>&) override {
    SubmitProgress sp;
    sp.state = "error";
    sp.message = name_ + " auto-submit is not implemented yet; use the browser fallback.";
    return sp;
  }

 private:
  std::string name_;
  std::function<std::string(const Problem&)> urlFn_;
};

}  // namespace

std::unique_ptr<Judge> makeJudge(const std::string& judgeName) {
  if (judgeName == "codeforces") return makeCodeforcesJudge();
  if (judgeName == "atcoder")
    return std::make_unique<BrowserOnlyJudge>("AtCoder", [](const Problem& p) {
      // https://atcoder.jp/contests/abc412/tasks/abc412_a -> .../submit?taskScreenName=abc412_a
      auto pos = p.url.find("/tasks/");
      if (pos == std::string::npos) return p.url;
      std::string task = p.url.substr(pos + 7);
      auto q = task.find_first_of("?#/");
      if (q != std::string::npos) task = task.substr(0, q);
      return p.url.substr(0, pos) + "/submit?taskScreenName=" + task;
    });
  if (judgeName == "cses") return std::make_unique<BrowserOnlyJudge>("CSES", [](const Problem& p) {
      // https://cses.fi/problemset/task/1068 -> https://cses.fi/problemset/submit/1068
      return util::replaceAll(p.url, "/task/", "/submit/");
    });
  if (judgeName == "usaco") return std::make_unique<BrowserOnlyJudge>("USACO", [](const Problem& p) { return p.url; });
  if (judgeName == "hackerrank")
    return std::make_unique<BrowserOnlyJudge>("HackerRank", [](const Problem& p) { return p.url; });
  return std::make_unique<BrowserOnlyJudge>("Judge", [](const Problem& p) { return p.url; });
}

// Only http(s) URLs and existing local folders are opened: ShellExecute/xdg-open would
// happily run any other string (a local .exe path, a file: URL, ...).
static bool openable(const std::string& target) {
  if (util::isSafeHttpUrl(target)) return true;
  std::error_code ec;
  return fs::is_directory(util::upath(target), ec);
}

#ifdef _WIN32
void openInBrowser(const std::string& url) {
  if (!openable(url)) return;
  ShellExecuteW(nullptr, L"open", util::widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void copyToClipboard(const std::string& text) {
  std::wstring w = util::widen(text);
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (h) {
    auto* dst = (wchar_t*)GlobalLock(h);
    memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  }
  CloseClipboard();
}
#else
#include "../runner.hpp"

void openInBrowser(const std::string& url) {
  if (!openable(url)) return;
  std::string q = "'" + util::replaceAll(url, "'", "'\\''") + "'";
#ifdef __APPLE__
  std::string cmd = "open " + q;
#else
  std::string cmd = "xdg-open " + q + " >/dev/null 2>&1 &";
#endif
  Process p;
  p.start(util::widen(cmd), fs::temp_directory_path(), nullptr, nullptr);
  p.closeStdin();
  p.wait(3000);
}

void copyToClipboard(const std::string& text) {
#ifdef __APPLE__
  std::string cmd = "pbcopy";
#else
  // Wayland first, then X11; whichever exists on this desktop.
  std::string cmd = "if command -v wl-copy >/dev/null 2>&1; then wl-copy; elif command -v xclip >/dev/null 2>&1; then xclip -selection clipboard; else xsel --clipboard --input; fi";
#endif
  Process p;
  if (!p.start(util::widen(cmd), fs::temp_directory_path(), nullptr, nullptr)) return;
  p.write(text);
  p.closeStdin();
  p.wait(3000);
}
#endif
