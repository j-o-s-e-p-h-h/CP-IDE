// In-app judge submission through a WebView2 window (one per judge site).
//
// The judge keeps a real browser session: the user logs in once in the window
// (cookies persist in the WebView2 profile). After that Submit navigates the window
// to the submit page, fills the form with JavaScript, presses the site's button and
// reads the verdict back (public API for Codeforces, the site's own status page for
// the others). All methods run on the UI thread except where noted.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include "../util.hpp"
#include "judge.hpp"
#include "webview/webview.h"

// Everything site-specific. Strings marked (js) are JavaScript run inside the judge page.
struct SiteDriver {
  std::string id;            // codeforces | atcoder | cses | usaco
  std::string name;          // "AtCoder"
  std::string loginUrl;      // page shown to the user when a login is needed
  std::string homeUrl;       // page loaded (hidden) to check whether the user is logged in
  std::string loginUrlPart;  // URL substring that identifies the login page
  std::string loggedInJs;    // (js) expression -> bool
  std::string handleJs;      // (js) expression -> handle string or ''
  std::string submitUrl;     // page with the submit form (resolved for this request)
  std::string formSelector;  // CSS selector of the form
  std::string fillJs;        // (js) statements; `form` and `src` are in scope; must press the button
  std::string submittedUrlPart;     // URL substring reached after a successful submit
  bool pollViaApi = false;          // Codeforces: verdict from api/user.status
  bool pollInPlace = false;         // USACO: results appear on the same page, no navigation
  bool statusFromSubmittedUrl = false;  // CSES: poll the result page we were redirected to
  std::string statusUrl;            // AtCoder: page reloaded until the verdict is final
  std::string verdictJs;            // (js) statements; must set window.__cpV = {final, text, ok}
  int pollSeconds = 3;
};

SiteDriver makeSiteDriver(const std::string& judgeId, const SubmitRequest& req, const json& config);
bool judgeSupportsInApp(const std::string& judgeId);

class JudgeWeb {
 public:
  using Dispatch = std::function<void(std::function<void()>)>;  // runs fn on the UI thread
  using Progress = std::function<void(const SubmitProgress&)>;   // may be called from any thread

  JudgeWeb(std::string judgeId, Dispatch dispatch) : judgeId_(std::move(judgeId)), dispatch_(std::move(dispatch)) {}
  ~JudgeWeb();

  void submit(const SubmitRequest& req, const json& config, Progress progress, Progress finish);
  void cancel();
  bool busy() const { return phase_ != Phase::Idle; }
  void showLogin(const json& config);
  // Loads the site's home page hidden and reports whether a session exists (UI thread; cb on UI thread).
  void checkLogin(const json& config, std::function<void(bool loggedIn, const std::string& handle)> cb);
  void destroyWindow();
  // Developer hook: runs JS in the judge window; scripts may report {event:'dev', value}.
  void devEval(const std::string& js);
  std::string lastDev() const { return lastDev_; }
  void onUserClose();  // the user pressed the window's close button (called from the platform hook)

 private:
  enum class Phase { Idle, WaitingForUser, Submitting, Submitted, Polling };
  std::string judgeId_;
  Dispatch dispatch_;
  std::unique_ptr<webview::webview> view_;
  void* hwnd_ = nullptr;  // HWND / GtkWindow* / NSWindow*
  SiteDriver drv_;
  bool initInstalled_ = false;
  std::atomic<Phase> phase_{Phase::Idle};
  SubmitRequest req_;
  json config_;
  Progress progress_, finish_;
  std::string handle_, statusUrl_, lastDev_;
  std::function<void(bool, const std::string&)> checkCb_;
  int64_t submittedAt_ = 0;
  std::thread pollThread_;
  std::atomic<bool> cancel_{false};
  std::atomic<int> generation_{0};

  void ensureView();
  void show(bool visible);
  void onReport(const json& r);
  void fail(const std::string& message);
  void say(const std::string& message);
  void startPolling(const std::string& landedUrl);
  void pollStatusPage();
  void runVerdictScript();
  void installCloseHook();
  void removeCloseHook();
};
