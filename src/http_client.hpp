// Minimal WinHTTP client. One HttpClient = one cookie jar (WinHTTP keeps
// cookies per session handle), which is what judge logins need.
#pragma once
#include <map>
#include <string>

struct HttpResponse {
  int status = 0;
  std::string body;
  std::string headers;  // raw response headers
  std::string finalUrl;
  std::string error;
  bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers = {});
  HttpResponse post(const std::string& url, const std::string& body,
                    const std::string& contentType = "application/x-www-form-urlencoded",
                    const std::map<std::string, std::string>& headers = {});
  HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                       const std::map<std::string, std::string>& headers);

 private:
  void* session_ = nullptr;
};
