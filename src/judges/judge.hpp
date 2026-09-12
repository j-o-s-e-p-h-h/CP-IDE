// One interface for every online judge. Codeforces is implemented; the
// others fall back to "submit in the browser" (code copied to clipboard,
// submit page opened) which every judge supports.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include "../model.hpp"

struct SubmitRequest {
  Problem problem;
  std::string lang;  // python | cpp
  std::string code;
};

struct SubmitProgress {
  std::string state;    // judging | done | browser | error
  std::string verdict;  // final verdict text, e.g. "Accepted", "Wrong answer on test 3"
  std::string message;  // human-readable progress/error message
  bool ok = false;      // verdict == Accepted
};

class Judge {
 public:
  virtual ~Judge() = default;
  virtual std::string name() const = 0;
  // True when the judge can be driven from inside the app with the given config.
  virtual bool canAutoSubmit(const json& config) const = 0;
  // Web page where the user can submit by hand.
  virtual std::string submitUrl(const Problem& p) const = 0;
  // Blocking auto-submit + verdict polling; progress() may be called many times.
  virtual SubmitProgress submit(const SubmitRequest& req, const json& config,
                                const std::function<void(const SubmitProgress&)>& progress,
                                std::atomic<bool>& cancel) = 0;
  // After a browser submit, judges that expose a public API can still fetch the
  // verdict; return false when unsupported.
  virtual bool pollAfterBrowserSubmit(const SubmitRequest& req, const json& config, int64_t sinceEpoch,
                                      const std::function<void(const SubmitProgress&)>& progress,
                                      std::atomic<bool>& cancel, SubmitProgress& final) {
    return false;
  }
};

std::unique_ptr<Judge> makeJudge(const std::string& judgeName);

// Helpers shared by judges.
void openInBrowser(const std::string& url);
void copyToClipboard(const std::string& text);
