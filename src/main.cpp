// CP IDE entry point: native window (webview/webview: WebView2 on Windows, WebKitGTK on
// Linux, WKWebView on macOS), local HTTP server on :10045 (Competitive Companion + UI
// files), RPC bridge.
#include <algorithm>
#include <cstdio>
#include <string>
#include "app.hpp"
#include "http_server.hpp"
#include "util.hpp"
#include "webview/webview.h"
#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#endif

namespace {

fs::path findUiDir() {
  fs::path exe = util::exeDir();
  std::error_code ec;
  for (auto cand : {exe / "ui", exe.parent_path() / "ui", exe.parent_path().parent_path() / "ui",
                    exe.parent_path() / "share" / "cp-ide" / "ui", exe / ".." / "Resources" / "ui"})
    if (fs::exists(cand / "index.html", ec)) return cand;
  return exe / "ui";
}

fs::path findToolsDir(const fs::path& uiDir) {
  std::error_code ec;
  fs::path exe = util::exeDir();
  for (auto cand : {exe / "tools", uiDir.parent_path() / "tools"})
    if (fs::exists(cand / "cp_debug.py", ec)) return cand;
  return exe / "tools";
}

fs::path dataRoot() {
  std::string env = util::envVar("CP_IDE_HOME");
  if (!env.empty()) return util::upath(env);
  std::string home = util::envVar(util::kWindows ? "USERPROFILE" : "HOME");
  if (home.empty()) home = util::kWindows ? "C:\\" : "/tmp";
  return util::upath(home) / "cp";
}

void fatal(const std::string& msg) {
#ifdef _WIN32
  MessageBoxW(nullptr, util::widen(msg).c_str(), L"CP IDE", MB_ICONERROR);
#else
  fprintf(stderr, "CP IDE: %s\n", msg.c_str());
#endif
}

#ifdef _WIN32
// WebView2 keeps a browser profile in %APPDATA%\cp-ide.exe\EBWebView for the judge
// logins. Its caches grow without bound, so trim them on start once they pass 30 MB.
// Cookies/sessions live in Default\Network and are left alone.
void trimWebViewCaches() {
  std::string appData = util::envVar("APPDATA");
  if (appData.empty()) return;
  fs::path prof = util::upath(appData) / "cp-ide.exe" / "EBWebView";
  const char* cacheDirs[] = {"Default\\Cache", "Default\\Code Cache", "Default\\GPUCache", "Default\\DawnWebGPUCache",
                             "Default\\DawnGraphiteCache", "GrShaderCache", "component_crx_cache", "Subresource Filter",
                             "Speech Recognition", "ShaderCache"};
  std::error_code ec;
  uintmax_t total = 0;
  for (auto* d : cacheDirs)
    for (auto& e : fs::recursive_directory_iterator(prof / util::upath(d), fs::directory_options::skip_permission_denied, ec))
      if (e.is_regular_file(ec)) total += e.file_size(ec);
  if (total < 30u * 1024 * 1024) return;
  for (auto* d : cacheDirs) fs::remove_all(prof / util::upath(d), ec);
}
#endif

int runApp() {
  fs::path uiDir = findUiDir();
  fs::path toolsDir = findToolsDir(uiDir);
  App app(dataRoot(), uiDir, toolsDir);

  HttpServer server(10045, uiDir);
  if (util::envVar("CP_IDE_DEV") == "1") server.setDevHandler([&](const HttpRequest& r) { return app.onDevRequest(r); });
  if (!server.start([&](const HttpRequest& r) { return app.onCompanionPost(r); })) {
    fatal("Could not listen on 127.0.0.1:10045.\n" + server.lastError() + "\n\nIs another CP IDE instance running?");
    return 1;
  }

  webview::webview w(true, nullptr);
  w.set_title("CP IDE");
  w.set_size(1280, 820, WEBVIEW_HINT_NONE);
  w.set_size(900, 560, WEBVIEW_HINT_MIN);
#ifdef _WIN32
  {
    RECT wa{0, 0, 1366, 768};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = std::min(1280L, (long)(wa.right - wa.left) - 40), wh = std::min(820L, (long)(wa.bottom - wa.top) - 60);
    w.set_size(std::max(ww, 900), std::max(wh, 560), WEBVIEW_HINT_NONE);
    // Center on the work area (the default cascade position can push the window off small screens).
    if (auto hw = w.window(); hw.ok()) {
      HWND hwnd = (HWND)hw.value();
      // Title-bar / taskbar icon from the embedded resource (src/res/app.rc).
      if (HICON big = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR))
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
      if (HICON small = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR))
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
      RECT r{};
      GetWindowRect(hwnd, &r);
      int cw = r.right - r.left, ch = r.bottom - r.top;
      SetWindowPos(hwnd, nullptr, wa.left + ((wa.right - wa.left) - cw) / 2, wa.top + ((wa.bottom - wa.top) - ch) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
  }
#endif
  app.attach(&w);
#ifdef _WIN32
  // When the main window closes, take the helper windows (judge logins) down with it,
  // otherwise webview keeps the message loop alive for them.
  if (auto hw = w.window(); hw.ok()) {
    SetWindowSubclass(
        (HWND)hw.value(),
        [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) -> LRESULT {
          if (msg == WM_CLOSE) ((App*)ref)->closeAuxWindows();
          return DefSubclassProc(hwnd, msg, wp, lp);
        },
        1, (DWORD_PTR)&app);
  }
#elif !defined(__APPLE__)
  // GTK: same idea through the window's close signal.
  if (auto hw = w.window(); hw.ok()) {
#if GTK_MAJOR_VERSION >= 4
    g_signal_connect(G_OBJECT(hw.value()), "close-request",
                     G_CALLBACK(+[](GtkWindow*, gpointer app) -> gboolean { ((App*)app)->closeAuxWindows(); return FALSE; }), &app);
#else
    g_signal_connect(G_OBJECT(hw.value()), "delete-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer app) -> gboolean { ((App*)app)->closeAuxWindows(); return FALSE; }), &app);
#endif
  }
#endif
  w.bind("cp_rpc", [&](std::string req) -> std::string {
    auto a = json::parse(req, nullptr, false);
    if (a.is_discarded() || !a.is_array() || a.empty() || !a[0].is_string()) return "{\"error\":\"bad rpc\"}";
    json args = a.size() > 1 && a[1].is_object() ? a[1] : json::object();
    return app.rpc(a[0].get<std::string>(), args);
  });
  w.navigate("http://127.0.0.1:10045/");
  w.run();
  app.closeAuxWindows();
  app.shutdown();
  server.stop();
  return 0;
}

}  // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  trimWebViewCaches();
  return runApp();
}
#else
int main() { return runApp(); }
#endif
