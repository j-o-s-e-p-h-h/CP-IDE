#include "http_client.hpp"
#include "util.hpp"

#ifndef _WIN32
// ================================================================ POSIX: curl
// Linux and macOS ship curl; it also handles proxies and certificates for us.
#include "runner.hpp"

HttpClient::HttpClient() {}
HttpClient::~HttpClient() {}

HttpResponse HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
  return request("GET", url, "", headers);
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body, const std::string& contentType,
                              const std::map<std::string, std::string>& headers) {
  auto h = headers;
  h["Content-Type"] = contentType;
  return request("POST", url, body, h);
}

HttpResponse HttpClient::request(const std::string& method, const std::string& url, const std::string& body,
                                 const std::map<std::string, std::string>& headers) {
  HttpResponse res;
  std::string cmd = "curl -sS -L --compressed -m 60 -X " + method + " -w '\\n__CP_STATUS__%{http_code} %{url_effective}'";
  cmd += " -A 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36 CP-IDE/0.1'";
  for (auto& [k, v] : headers) {
    std::string hv = k + ": " + v;
    cmd += " -H '" + util::replaceAll(hv, "'", "'\\''") + "'";
  }
  if (!body.empty()) cmd += " --data-binary @-";
  cmd += " '" + util::replaceAll(url, "'", "'\\''") + "'";
  auto r = runProcess(util::widen(cmd), fs::temp_directory_path(), body, 65000);
  if (!r.started) {
    res.error = "could not run curl";
    return res;
  }
  auto pos = r.out.rfind("__CP_STATUS__");
  if (pos == std::string::npos) {
    res.error = util::trim(r.err).empty() ? "curl failed" : util::trim(r.err);
    return res;
  }
  std::string tail = r.out.substr(pos + 13);
  res.status = atoi(tail.c_str());
  auto sp = tail.find(' ');
  if (sp != std::string::npos) res.finalUrl = util::trim(tail.substr(sp + 1));
  res.body = r.out.substr(0, pos);
  if (!res.body.empty() && res.body.back() == '\n') res.body.pop_back();
  return res;
}

#else
// ============================================================ Windows: WinHTTP
#include <windows.h>
#include <winhttp.h>

namespace {
const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36 CP-IDE/0.1";
}

HttpClient::HttpClient() {
  session_ = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_) session_ = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_) {
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session_, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    WinHttpSetTimeouts(session_, 10000, 10000, 30000, 60000);
  }
}

HttpClient::~HttpClient() {
  if (session_) WinHttpCloseHandle(session_);
}

HttpResponse HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
  return request("GET", url, "", headers);
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body, const std::string& contentType,
                              const std::map<std::string, std::string>& headers) {
  auto h = headers;
  h["Content-Type"] = contentType;
  return request("POST", url, body, h);
}

HttpResponse HttpClient::request(const std::string& method, const std::string& url, const std::string& body,
                                 const std::map<std::string, std::string>& headers) {
  HttpResponse res;
  if (!session_) {
    res.error = "WinHttpOpen failed";
    return res;
  }
  std::wstring wurl = util::widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256], path[4096];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    res.error = "Bad URL: " + url;
    return res;
  }
  std::wstring hostName(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring urlPath(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength && uc.lpszExtraInfo) urlPath += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET conn = WinHttpConnect(session_, hostName.c_str(), uc.nPort, 0);
  if (!conn) {
    res.error = "WinHttpConnect failed (" + std::to_string(GetLastError()) + ")";
    return res;
  }
  HINTERNET req = WinHttpOpenRequest(conn, util::widen(method).c_str(), urlPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    res.error = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ")";
    WinHttpCloseHandle(conn);
    return res;
  }
  std::wstring hdrs;
  for (auto& [k, v] : headers) hdrs += util::widen(k) + L": " + util::widen(v) + L"\r\n";
  if (!headers.count("Accept")) hdrs += L"Accept: text/html,application/json,*/*\r\n";
  if (!headers.count("Accept-Language")) hdrs += L"Accept-Language: en-US,en;q=0.9\r\n";
  BOOL ok = WinHttpSendRequest(req, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(), (DWORD)-1,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                               (DWORD)body.size(), 0);
  if (ok) ok = WinHttpReceiveResponse(req, nullptr);
  if (!ok) {
    res.error = "Request failed (" + std::to_string(GetLastError()) + ")";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return res;
  }
  DWORD status = 0, sz = sizeof status;
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                      WINHTTP_NO_HEADER_INDEX);
  res.status = (int)status;
  DWORD hsz = 0;
  WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hsz, WINHTTP_NO_HEADER_INDEX);
  if (hsz) {
    std::wstring wh(hsz / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, wh.data(), &hsz, WINHTTP_NO_HEADER_INDEX))
      res.headers = util::narrow(wh.c_str());
  }
  {
    DWORD usz = 0;
    WinHttpQueryOption(req, WINHTTP_OPTION_URL, nullptr, &usz);
    if (usz) {
      std::wstring wu(usz / sizeof(wchar_t), L'\0');
      if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, wu.data(), &usz)) res.finalUrl = util::narrow(wu.c_str());
    }
  }
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
    std::string chunk(avail, '\0');
    DWORD got = 0;
    if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
    chunk.resize(got);
    res.body += chunk;
    if (res.body.size() > (64u << 20)) break;
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  return res;
}

#endif  // _WIN32
