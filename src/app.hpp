// Application core: owns the current contest, dispatches RPC calls coming from
// the UI (webview bind), runs long operations on threads and pushes events
// back to the page.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include "debugger.hpp"
#include "http_server.hpp"
#include "runner.hpp"
#include "storage.hpp"
#include "stress.hpp"
#include "tools_detect.hpp"
#include "webview/webview.h"

class App {
 public:
  App(fs::path root, fs::path uiDir, fs::path toolsDir);
  ~App();
  void attach(webview::webview* w) { wv_ = w; }
  void shutdown();
  // Called when the main window is closing: destroys helper windows so the loop can end.
  void closeAuxWindows();

  // JS -> C++
  std::string rpc(const std::string& name, const json& args);
  // Competitive Companion -> C++ (called on the HTTP server thread)
  std::string onCompanionPost(const HttpRequest& req);
  std::string onCompanionPostImpl(const HttpRequest& req);
  // C++ -> JS
  void emit(const json& ev);
  // Developer hook (only wired when CP_IDE_DEV=1): POST /__eval runs JS in the page and
  // returns its (awaited) result; used by the automated checks.
  std::string onDevRequest(const HttpRequest& req);
  // Creates the per-run secret local tools must send as X-CP-Dev (written to <root>/dev.token).
  void enableDevHook();

 private:
  Storage storage_;
  fs::path uiDir_, toolsDir_;
  std::recursive_mutex mu_;
  Contest contest_;
  bool hasContest_ = false;
  json config_;
  Toolchain tc_;
  std::vector<ToolStatus> tools_;
  webview::webview* wv_ = nullptr;

  std::atomic<bool> runCancel_{false}, running_{false};
  std::thread runThread_;
  std::atomic<bool> judgeCancel_{false}, judging_{false};
  std::thread judgeThread_;
  std::atomic<bool> sessionBusy_{false};
  std::atomic<int> bgCount_{0};
  std::mutex devMu_;
  std::condition_variable devCv_;
  std::string devResult_, devToken_;
  bool devHasResult_ = false;
  void spawn(std::function<void()> fn);  // detached worker; shutdown() waits for all of them
  StressRunner stress_;
  // Elaborated type specifier: macOS's MacTypes.h declares a function Debugger(),
  // which hides our class and makes the plain name ambiguous there.
  class Debugger debugger_;
  std::map<std::string, std::unique_ptr<class JudgeWeb>> judgeWebs_;  // in-app judge windows, one per site
  class JudgeWeb& judgeWeb(const std::string& judgeId);
  std::string defaultLang();  // from state.json (set on the setup page)

  // helpers
  json sessionJson();
  json problemJson(const Problem& p);
  json contestsJson();
  json historyJson();
  json toast(const std::string& msg, const std::string& color) { return {{"msg", msg}, {"color", color}}; }
  Problem* problemById(const std::string& id);
  std::string nextFreeId(const Contest& c, const std::string& wanted);
  bool openContestDir(const std::string& dir);
  void switchToContest(Contest c);
  Contest* contestForIncoming(const Problem& p, int batchSize);
  void addProblemAndFetch(Problem p, const std::string& toastMsg);
  void fetchStatementAsync(std::string contestDir, std::string problemDir, std::string problemId);
  void applyProblemFields(Problem& p, const json& a);

  // rpc handlers
  json rpcInit();
  json rpcNewSession(const json& a);
  json rpcRun(const json& a);
  json rpcSubmit(const json& a);
  json rpcStress(const json& a, bool start);
  json rpcDebugStart(const json& a);
  json rpcImportUrl(const json& a);
  json rpcImportContest(const std::string& url, const std::string& judge, const std::string& contestId);
  json rpcFormat(const json& a);
  void recordVerdict(const std::string& probId, const std::string& lang, const std::string& verdict, bool ok, const std::string& url);
};
