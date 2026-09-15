#include "judge_web.hpp"
#ifdef _WIN32
#include <commctrl.h>
#elif defined(__APPLE__)
#include <objc/objc-runtime.h>
#endif

namespace {
std::string js(const std::string& s) { return json(s).dump(); }

// Runs on every page the judge window loads and reports what it sees. %LOGGED_IN% and
// %HANDLE% are replaced with the site's expressions, %FORM% with the form selector.
const char* kInitTemplate = R"JS(
(function () {
  function report() {
    try {
      var loggedIn = false, handle = '';
      try { loggedIn = !!(%LOGGED_IN%); } catch (e) {}
      try { handle = String((%HANDLE%) || ''); } catch (e) {}
      var form = document.querySelector(%FORM%);
      var errEl = null;
      var errs = document.querySelectorAll('.error.for__source, .error.for__sourceFile, .error.for__programTypeId, span.error, .alert-danger, .alert.alert-warning, .error');
      for (var i = 0; i < errs.length; i++) { var tx = errs[i].textContent.replace(/ /g, ' ').trim(); if (tx && tx.length < 300) { errEl = errs[i]; break; } }
      var challenge = /Just a moment|Attention Required|Checking your browser/i.test(document.title);
      window.__cpReport(JSON.stringify({ url: location.href, title: document.title, loggedIn: loggedIn, handle: handle,
        hasForm: !!form, error: errEl ? errEl.textContent.replace(/ /g, ' ').trim() : '', challenge: challenge }));
    } catch (e) {}
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', report); else report();
  window.addEventListener('load', function () { setTimeout(report, 300); });
})();
)JS";
}  // namespace

bool judgeSupportsInApp(const std::string& judgeId) {
  return judgeId == "codeforces" || judgeId == "atcoder" || judgeId == "cses" || judgeId == "usaco";
}

JudgeWeb::~JudgeWeb() {
  cancel();
  if (pollThread_.joinable()) pollThread_.join();
}

// The judge window is hidden, never destroyed, when the user closes it: destroying it
// would drop the browser session and (in webview) can end the message loop.
void JudgeWeb::onUserClose() {
  show(false);
  if (phase_ == Phase::WaitingForUser || phase_ == Phase::Submitted)
    fail(drv_.name + " window closed before the submission went through");
}

#ifdef _WIN32
static LRESULT CALLBACK judgeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  if (msg == WM_CLOSE) {
    if (auto* self = (JudgeWeb*)ref) self->onUserClose();
    else ShowWindow(hwnd, SW_HIDE);
    return 0;
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

void JudgeWeb::installCloseHook() {
  SetWindowSubclass((HWND)hwnd_, judgeSubclassProc, 1, (DWORD_PTR)this);
  if (HICON ic = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR))
    SendMessageW((HWND)hwnd_, WM_SETICON, ICON_BIG, (LPARAM)ic);
}
void JudgeWeb::removeCloseHook() { RemoveWindowSubclass((HWND)hwnd_, judgeSubclassProc, 1); }

void JudgeWeb::show(bool visible) {
  if (!hwnd_) return;
  if (visible) {
    ShowWindow((HWND)hwnd_, SW_SHOW);
    SetForegroundWindow((HWND)hwnd_);
  } else ShowWindow((HWND)hwnd_, SW_HIDE);
}

#elif defined(__APPLE__)
// Cocoa through the Objective-C runtime (no Objective-C++ needed). Instead of intercepting
// the close button, the window simply has none: it is shown and hidden by the app.
namespace {
// Not id_t: <sys/_types/_id_t.h> already defines that in the global namespace,
// and an unqualified use in here is then ambiguous.
using objc_id = id;
template <typename R = void, typename... A> R msg(objc_id obj, const char* sel, A... a) {
  return ((R (*)(objc_id, SEL, A...))objc_msgSend)(obj, sel_registerName(sel), a...);
}
}  // namespace
void JudgeWeb::installCloseHook() {
  auto win = (objc_id)hwnd_;
  unsigned long mask = msg<unsigned long>(win, "styleMask");
  msg<void, unsigned long>(win, "setStyleMask:", mask & ~(unsigned long)2 /* NSWindowStyleMaskClosable */);
  msg<void, BOOL>(win, "setReleasedWhenClosed:", NO);
}
void JudgeWeb::removeCloseHook() {}
void JudgeWeb::show(bool visible) {
  if (!hwnd_) return;
  auto win = (objc_id)hwnd_;
  if (visible) msg<void, objc_id>(win, "makeKeyAndOrderFront:", nullptr);
  else msg<void, objc_id>(win, "orderOut:", nullptr);
}

#else
// GTK (WebKitGTK). webview.h already pulled in <gtk/gtk.h>.
#if GTK_MAJOR_VERSION >= 4
static gboolean judgeCloseRequest(GtkWindow*, gpointer self) {
  ((JudgeWeb*)self)->onUserClose();
  return TRUE;  // handled: do not destroy
}
#else
static gboolean judgeDeleteEvent(GtkWidget*, GdkEvent*, gpointer self) {
  ((JudgeWeb*)self)->onUserClose();
  return TRUE;
}
#endif
void JudgeWeb::installCloseHook() {
#if GTK_MAJOR_VERSION >= 4
  g_signal_connect(G_OBJECT(hwnd_), "close-request", G_CALLBACK(judgeCloseRequest), this);
#else
  g_signal_connect(G_OBJECT(hwnd_), "delete-event", G_CALLBACK(judgeDeleteEvent), this);
#endif
}
void JudgeWeb::removeCloseHook() {}
void JudgeWeb::show(bool visible) {
  if (!hwnd_) return;
  if (visible) {
    gtk_widget_set_visible(GTK_WIDGET(hwnd_), TRUE);
    gtk_window_present(GTK_WINDOW(hwnd_));
  } else gtk_widget_set_visible(GTK_WIDGET(hwnd_), FALSE);
}
#endif

void JudgeWeb::ensureView() {
  if (view_) return;
  view_ = std::make_unique<webview::webview>(false, nullptr);
  view_->set_title(drv_.name + " — CP IDE");
  view_->set_size(1000, 740, WEBVIEW_HINT_NONE);
  if (auto hw = view_->window(); hw.ok()) {
    hwnd_ = hw.value();
    installCloseHook();
    show(false);
  }
  view_->bind("__cpReport", [this](std::string args) -> std::string {
    auto a = json::parse(args, nullptr, false);
    if (a.is_array() && !a.empty() && a[0].is_string()) {
      auto r = json::parse(a[0].get<std::string>(), nullptr, false);
      if (r.is_object()) onReport(r);
    }
    return "{}";
  });
  std::string init = kInitTemplate;
  init = util::replaceAll(init, "%LOGGED_IN%", drv_.loggedInJs.empty() ? "false" : drv_.loggedInJs);
  init = util::replaceAll(init, "%HANDLE%", drv_.handleJs.empty() ? "''" : drv_.handleJs);
  init = util::replaceAll(init, "%FORM%", js(drv_.formSelector.empty() ? "form" : drv_.formSelector));
  view_->init(init);
  initInstalled_ = true;
}

void JudgeWeb::showLogin(const json& config) {
  if (!view_) {
    SubmitRequest dummy;
    drv_ = makeSiteDriver(judgeId_, dummy, config);
  }
  ensureView();
  view_->navigate(drv_.loginUrl);
  show(true);
}

void JudgeWeb::checkLogin(const json& config, std::function<void(bool, const std::string&)> cb) {
  if (phase_ != Phase::Idle) {  // a submission owns the window right now
    cb(!handle_.empty(), handle_);
    return;
  }
  if (!view_) {
    SubmitRequest dummy;
    drv_ = makeSiteDriver(judgeId_, dummy, config);
  }
  ensureView();
  checkCb_ = std::move(cb);
  view_->navigate(drv_.homeUrl.empty() ? drv_.loginUrl : drv_.homeUrl);
  // Do not leave the caller hanging if the page never reports (offline).
  std::thread([this] {
    std::this_thread::sleep_for(std::chrono::seconds(15));
    dispatch_([this] {
      if (checkCb_) {
        auto cb = std::move(checkCb_);
        checkCb_ = nullptr;
        cb(false, "");
      }
    });
  }).detach();
}

void JudgeWeb::devEval(const std::string& script) {
  if (!view_) {
    SubmitRequest dummy;
    drv_ = makeSiteDriver(judgeId_, dummy, json::object());
  }
  ensureView();
  lastDev_.clear();
  view_->eval(script);
}

void JudgeWeb::say(const std::string& message) {
  SubmitProgress sp;
  sp.state = "judging";
  sp.message = message;
  if (progress_) progress_(sp);
}

void JudgeWeb::fail(const std::string& message) {
  if (phase_ == Phase::Idle) return;
  phase_ = Phase::Idle;
  ++generation_;
  SubmitProgress sp;
  sp.state = "error";
  sp.message = message;
  if (finish_) finish_(sp);
}

void JudgeWeb::cancel() {
  cancel_ = true;
  ++generation_;
  bool wasActive = phase_ != Phase::Idle;
  phase_ = Phase::Idle;
  if (wasActive) show(false);  // a replaced/cancelled submission should not leave its window up
}

void JudgeWeb::destroyWindow() {
  cancel();
  if (hwnd_) removeCloseHook();
  hwnd_ = nullptr;
  view_.reset();
}

void JudgeWeb::submit(const SubmitRequest& req, const json& config, Progress progress, Progress finish) {
  cancel();
  if (pollThread_.joinable()) pollThread_.join();
  cancel_ = false;
  req_ = req;
  config_ = config;
  progress_ = std::move(progress);
  finish_ = std::move(finish);
  handle_.clear();
  statusUrl_.clear();
  drv_ = makeSiteDriver(judgeId_, req, config);
  if (drv_.submitUrl.empty()) {
    SubmitProgress sp;
    sp.state = "error";
    sp.message = "Could not work out the " + drv_.name + " submit page for this problem's URL";
    finish_(sp);
    return;
  }
  // The window is created with the site's scripts; a different site gets its own JudgeWeb.
  ensureView();
  phase_ = Phase::Submitting;
  say("Opening the " + drv_.name + " submit page…");
  // Cloudflare's Turnstile (Codeforces) only runs in a visible page; showing the window for
  // every site keeps the behaviour predictable and lets the user see what is happening.
  show(true);
  view_->navigate(drv_.submitUrl);
  int gen = generation_;
  std::thread([this, gen] {
    for (int i = 0; i < 600 && generation_ == gen && phase_ != Phase::Idle && phase_ != Phase::Polling; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    if (generation_ == gen && phase_ != Phase::Idle && phase_ != Phase::Polling)
      dispatch_([this, gen] { if (generation_ == gen) fail("Timed out waiting for " + drv_.name + " (log in and try again)"); });
  }).detach();
}

void JudgeWeb::onReport(const json& r) {
  std::string event = r.value("event", "");
  if (event == "dev") {
    lastDev_ = r.contains("value") ? r["value"].dump() : "null";
    return;
  }
  Phase ph = phase_;
  std::string url = r.value("url", "");
  bool loggedIn = r.value("loggedIn", false), hasForm = r.value("hasForm", false), challenge = r.value("challenge", false);
  std::string handle = r.value("handle", ""), error = r.value("error", "");
  if (!handle.empty()) handle_ = handle;
  if (ph == Phase::Idle) {
    if (checkCb_ && event.empty() && !challenge) {
      auto cb = std::move(checkCb_);
      checkCb_ = nullptr;
      cb(loggedIn, loggedIn ? handle : "");
    }
    return;
  }

  if (event == "verdict") {
    if (ph != Phase::Polling) return;
    std::string text = r.value("text", "");
    if (r.value("final", false)) {
      phase_ = Phase::Idle;
      ++generation_;
      SubmitProgress sp;
      sp.state = "done";
      sp.verdict = text.empty() ? "Judged" : text;
      sp.ok = r.value("ok", false);
      sp.message = sp.verdict;
      show(false);
      if (finish_) finish_(sp);
      return;
    }
    if (!text.empty()) say(text);
    int gen = generation_;
    std::thread([this, gen] {
      std::this_thread::sleep_for(std::chrono::seconds(drv_.pollSeconds));
      dispatch_([this, gen] { if (generation_ == gen && phase_ == Phase::Polling) pollStatusPage(); });
    }).detach();
    return;
  }
  if (ph == Phase::Polling) {
    // a page report on the status page -> read the verdict from it
    if (!drv_.pollInPlace && !statusUrl_.empty() && util::contains(url, statusUrl_.substr(0, statusUrl_.find('?')))) runVerdictScript();
    return;
  }
  if (event == "clicked") {
    if (ph != Phase::Submitted) return;
    say("Submitting " + req_.problem.id + " as " + (handle_.empty() ? "you" : handle_) + "…");
    if (drv_.pollInPlace) startPolling(url);
    return;
  }
  if (event == "filled") {  // the site needs the user's own click (anti-bot check not passed)
    if (ph != Phase::Submitted) return;
    if (!error.empty()) {
      show(true);
      fail("Could not fill the submit form: " + error);
      return;
    }
    show(true);
    say("Form filled — finish the check and press Submit in the " + drv_.name + " window; the verdict comes back here");
    return;
  }
  if (challenge) {
    phase_ = Phase::WaitingForUser;
    show(true);
    say(drv_.name + " is running its browser check — complete it in the " + drv_.name + " window");
    return;
  }
  bool onLoginPage = !drv_.loginUrlPart.empty() && util::contains(url, drv_.loginUrlPart);
  if (!loggedIn && (onLoginPage || (!hasForm && ph != Phase::Submitted))) {
    phase_ = Phase::WaitingForUser;
    show(true);
    say("Log in to " + drv_.name + " in the window that just opened (only needed once)");
    return;
  }
  if (ph == Phase::WaitingForUser && loggedIn && !(hasForm && util::contains(url, drv_.submitUrl.substr(0, drv_.submitUrl.find('?'))))) {
    phase_ = Phase::Submitting;
    say("Logged in as " + handle_ + " — opening the submit page…");
    view_->navigate(drv_.submitUrl);
    return;
  }
  if (hasForm && loggedIn && util::contains(url, drv_.submitUrl.substr(0, drv_.submitUrl.find('?')))) {
    if (ph == Phase::Submitted) {
      if (!error.empty()) {
        show(true);
        say(drv_.name + " says: " + error + " — fix it in the " + drv_.name + " window and press Submit again");
      }
      return;
    }
    phase_ = Phase::Submitted;
    say("Filling in the submit form for " + req_.problem.id + " as " + (handle_.empty() ? "you" : handle_) + "…");
    std::string fill =
        "(function(){try{"
        "var form=document.querySelector(" + js(drv_.formSelector) + ");"
        "if(!form){var why='';try{why=" + (drv_.noFormJs.empty() ? std::string("''") : drv_.noFormJs) + "||'';}catch(e){}"
        "window.__cpReport(JSON.stringify({event:'filled',error:why||'submit form not found',url:location.href}));return;}"
        "var src=" + js(req_.code) + ";" + drv_.fillJs +
        "}catch(e){window.__cpReport(JSON.stringify({event:'filled',error:String(e),url:location.href}));}})();";
    view_->eval(fill);
    return;
  }
  if (ph == Phase::Submitted && !drv_.submittedUrlPart.empty() && util::contains(url, drv_.submittedUrlPart)) {
    startPolling(url);
    return;
  }
  if (ph == Phase::Submitting && loggedIn && !hasForm) {
    view_->navigate(drv_.submitUrl);  // landed elsewhere (home page after login): try again
  }
}

void JudgeWeb::startPolling(const std::string& landedUrl) {
  phase_ = Phase::Polling;
  submittedAt_ = util::nowSec();
  say("Submitted — waiting for the verdict…");
  if (drv_.pollViaApi) {
    show(false);
    if (handle_.empty()) {
      fail("Submitted, but the handle could not be read from the page — check the verdict on the site");
      return;
    }
    int gen = generation_;
    if (pollThread_.joinable()) pollThread_.join();
    json cfg = config_;
    cfg["codeforces"]["handle"] = handle_;
    pollThread_ = std::thread([this, gen, cfg] {
      auto judge = makeJudge("codeforces");
      SubmitProgress final;
      auto progress = [this, gen](const SubmitProgress& sp) { if (generation_ == gen && progress_) progress_(sp); };
      bool ok = judge->pollAfterBrowserSubmit(req_, cfg, submittedAt_ - 10, progress, cancel_, final);
      if (generation_ != gen) return;
      phase_ = Phase::Idle;
      if (ok && final.state == "done") {
        if (finish_) finish_(final);
      } else {
        SubmitProgress sp;
        sp.state = "error";
        sp.message = cancel_ ? "Cancelled" : "Timed out waiting for the verdict — check the site";
        if (finish_) finish_(sp);
      }
    });
    return;
  }
  statusUrl_ = drv_.statusFromSubmittedUrl ? landedUrl : drv_.statusUrl;
  if (drv_.pollInPlace) {
    int gen = generation_;
    std::thread([this, gen] {
      std::this_thread::sleep_for(std::chrono::seconds(drv_.pollSeconds));
      dispatch_([this, gen] { if (generation_ == gen && phase_ == Phase::Polling) runVerdictScript(); });
    }).detach();
  } else {
    // We are already on the status page (redirect) for CSES; AtCoder needs a navigation.
    if (drv_.statusFromSubmittedUrl) runVerdictScript();
    else pollStatusPage();
  }
  // give up after 6 minutes of polling
  int gen = generation_;
  std::thread([this, gen] {
    for (int i = 0; i < 360 && generation_ == gen && phase_ == Phase::Polling; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
    if (generation_ == gen && phase_ == Phase::Polling)
      dispatch_([this, gen] { if (generation_ == gen) fail("Timed out waiting for the verdict — check the site"); });
  }).detach();
}

void JudgeWeb::pollStatusPage() {
  if (phase_ != Phase::Polling || !view_) return;
  if (drv_.pollInPlace) {
    runVerdictScript();
    return;
  }
  view_->navigate(statusUrl_);
}

void JudgeWeb::runVerdictScript() {
  if (phase_ != Phase::Polling || !view_) return;
  std::string script =
      "(function(){try{window.__cpV={final:false,text:''};" + drv_.verdictJs +
      "var v=window.__cpV||{};window.__cpReport(JSON.stringify({event:'verdict',final:!!v.final,text:String(v.text||''),ok:!!v.ok}));"
      "}catch(e){window.__cpReport(JSON.stringify({event:'verdict',final:false,text:'reading verdict: '+String(e)}));}})();";
  view_->eval(script);
}
