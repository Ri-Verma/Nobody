#pragma once
// =============================================================================
//  core/HttpClient.h
//  Thread-safe libcurl wrapper. Every module that needs the internet goes here.
//  Features: User-Agent rotation, retry with backoff, rate limiting,
//            robots.txt respect, gzip support, cookie jar
// =============================================================================

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <mutex>

namespace nobody {

// ── Response ──────────────────────────────────────────────────────────────────
struct HttpResponse {
    int         status_code  = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    bool        success      = false;
    std::string error;
    std::chrono::milliseconds elapsed{0};
    int         retry_count  = 0;    // how many retries were needed

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
    int  max_retries        = 3;   // retry on 429/503/timeout
    bool rotate_user_agent  = true; // use UA rotation pool
};

// ── HttpClient ────────────────────────────────────────────────────────────────
class HttpClient {
public:
    explicit HttpClient(const std::string& user_agent =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");
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

    // ── Rate limiting ───────────────────────────────────────────────────────
    void set_rate_limit_ms(int ms);     // min ms between requests to same domain
    void enable_rate_limiting(bool on);

    // ── User-Agent rotation ─────────────────────────────────────────────────
    void enable_ua_rotation(bool on);
    std::string get_random_ua() const;

    // ── Utility ─────────────────────────────────────────────────────────────
    static std::string url_encode(const std::string& raw);
    static std::string build_query_string(
        const std::map<std::string, std::string>& params);
    static std::string extract_domain(const std::string& url);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    static size_t write_callback (void* ptr, size_t sz, size_t nmemb, std::string* out);
    static size_t header_callback(void* ptr, size_t sz, size_t nmemb,
                                   std::map<std::string, std::string>* hdrs);

    // Rate limiting
    void wait_for_rate_limit(const std::string& domain);

    // Retry logic
    HttpResponse execute_with_retry(const HttpRequest& request);
};

} // namespace nobody