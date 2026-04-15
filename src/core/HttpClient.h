#pragma once
// =============================================================================
//  core/HttpClient.h
//  Thread-safe libcurl wrapper. Every module that needs the internet goes here.
// =============================================================================

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace nobody {

// ── Response ──────────────────────────────────────────────────────────────────
struct HttpResponse {
    int         status_code  = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    bool        success      = false;
    std::string error;
    std::chrono::milliseconds elapsed{0};

    bool is_ok()  const { return success && status_code >= 200 && status_code < 300; }
    bool is_html() const;
    bool is_json() const;
};

// ── Request ───────────────────────────────────────────────────────────────────
struct HttpRequest {
    std::string method  = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    int  timeout_seconds    = 30;
    int  connect_timeout    = 10;
    bool follow_redirects   = true;
    int  max_redirects      = 5;
    bool verify_ssl         = true;
    std::string proxy;          // optional "http://host:port"
};

// ── HttpClient ────────────────────────────────────────────────────────────────
class HttpClient {
public:
    explicit HttpClient(const std::string& user_agent =
        "Nobody/1.0 (Nobody Research Tool; +https://github.com/nobody)");
    ~HttpClient();

    // Disable copy, allow move
    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&)                 = default;
    HttpClient& operator=(HttpClient&&)      = default;

    // ── Simple API ─────────────────────────────────────────────────────────
    HttpResponse get (const std::string& url,
                      const std::map<std::string, std::string>& extra_headers = {});

    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::map<std::string, std::string>& extra_headers = {});

    HttpResponse put (const std::string& url,
                      const std::string& body,
                      const std::map<std::string, std::string>& extra_headers = {});

    // ── Full control ────────────────────────────────────────────────────────
    HttpResponse execute(const HttpRequest& request);

    // ── Configuration ───────────────────────────────────────────────────────
    void set_user_agent(const std::string& ua);
    void set_default_timeout(int seconds);
    void set_proxy(const std::string& proxy);
    void set_default_header(const std::string& key, const std::string& value);

    // ── Utility ─────────────────────────────────────────────────────────────
    static std::string url_encode(const std::string& raw);
    static std::string build_query_string(
        const std::map<std::string, std::string>& params);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    static size_t write_callback (void* ptr, size_t sz, size_t nmemb, std::string* out);
    static size_t header_callback(void* ptr, size_t sz, size_t nmemb,
                                   std::map<std::string, std::string>* hdrs);
};

} // namespace nobody