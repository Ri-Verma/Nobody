// =============================================================================
//  core/HttpClient.cpp
//  Features: User-Agent rotation, retry with exponential backoff,
//            per-domain rate limiting, gzip support
// =============================================================================

#include "core/HttpClient.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cassert>
#include <iomanip>
#include <random>
#include <thread>
#include <mutex>
#include <map>

namespace nobody {

// ── is_html / is_json helpers ─────────────────────────────────────────────────
bool HttpResponse::is_html() const {
    auto it = headers.find("content-type");
    if (it == headers.end()) return false;
    return it->second.find("text/html") != std::string::npos;
}
bool HttpResponse::is_json() const {
    auto it = headers.find("content-type");
    if (it == headers.end()) return false;
    return it->second.find("application/json") != std::string::npos ||
           it->second.find("text/json") != std::string::npos;
}

// ── User-Agent rotation pool ─────────────────────────────────────────────────
static const std::vector<std::string> UA_POOL = {
    // Chrome on Windows
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    // Chrome on macOS
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_5) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    // Chrome on Linux
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    // Firefox on Windows
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:127.0) Gecko/20100101 Firefox/127.0",
    // Firefox on macOS
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14.5; rv:127.0) Gecko/20100101 Firefox/127.0",
    // Firefox on Linux
    "Mozilla/5.0 (X11; Linux x86_64; rv:127.0) Gecko/20100101 Firefox/127.0",
    // Safari on macOS
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_5) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15",
    // Edge on Windows
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 Edg/125.0.0.0",
    // Chrome on Android
    "Mozilla/5.0 (Linux; Android 14; SM-S918B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Mobile Safari/537.36",
    // Safari on iPhone
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1",
    // Opera on Windows
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 OPR/111.0.0.0",
    // Vivaldi on Linux
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 Vivaldi/6.8",
};

// ── Pimpl ─────────────────────────────────────────────────────────────────────
struct HttpClient::Impl {
    CURL*       curl        = nullptr;
    std::string user_agent;
    int         default_timeout = 30;
    std::string proxy;
    std::map<std::string, std::string> default_headers;

    // Rate limiting
    bool rate_limiting_enabled = true;
    int  rate_limit_ms         = 500; // min ms between same-domain requests
    std::mutex rate_mutex;
    std::map<std::string, std::chrono::steady_clock::time_point> domain_last_request;

    // UA rotation
    bool ua_rotation_enabled   = true;

    Impl() {
        // Global init is idempotent if called multiple times; each instance
        // still gets its own handle.
        curl_global_init(CURL_GLOBAL_ALL);
        curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init() failed");
    }

    ~Impl() {
        if (curl) curl_easy_cleanup(curl);
        // NOTE: curl_global_cleanup() should be called once at program exit.
        // We leave that to main() or a global guard.
    }
};

// ── Static callbacks ──────────────────────────────────────────────────────────
size_t HttpClient::write_callback(void* ptr, size_t sz, size_t nmemb,
                                   std::string* out) {
    size_t total = sz * nmemb;
    out->append(static_cast<char*>(ptr), total);
    return total;
}

size_t HttpClient::header_callback(void* ptr, size_t sz, size_t nmemb,
                                    std::map<std::string, std::string>* hdrs) {
    size_t total = sz * nmemb;
    std::string line(static_cast<char*>(ptr), total);
    // Trim \r\n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim leading spaces from value
        val.erase(0, val.find_first_not_of(' '));
        // Lowercase key for consistent access
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        (*hdrs)[key] = val;
    }
    return total;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────
HttpClient::HttpClient(const std::string& user_agent)
    : impl_(std::make_unique<Impl>()) {
    impl_->user_agent = user_agent;
}

HttpClient::~HttpClient() = default;

// ── Configuration ─────────────────────────────────────────────────────────────
void HttpClient::set_user_agent(const std::string& ua) { impl_->user_agent = ua; }
void HttpClient::set_default_timeout(int s) { impl_->default_timeout = s; }
void HttpClient::set_proxy(const std::string& p) { impl_->proxy = p; }
void HttpClient::set_default_header(const std::string& k, const std::string& v) {
    impl_->default_headers[k] = v;
}

void HttpClient::set_rate_limit_ms(int ms) { impl_->rate_limit_ms = ms; }
void HttpClient::enable_rate_limiting(bool on) { impl_->rate_limiting_enabled = on; }
void HttpClient::enable_ua_rotation(bool on) { impl_->ua_rotation_enabled = on; }

// ── User-Agent rotation ───────────────────────────────────────────────────────
std::string HttpClient::get_random_ua() const {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, UA_POOL.size() - 1);
    return UA_POOL[dist(rng)];
}

// ── Domain extraction ─────────────────────────────────────────────────────────
std::string HttpClient::extract_domain(const std::string& url) {
    size_t after_scheme = url.find("://");
    if (after_scheme == std::string::npos) return url;
    std::string rest = url.substr(after_scheme + 3);
    size_t slash = rest.find('/');
    return slash != std::string::npos ? rest.substr(0, slash) : rest;
}

// ── Rate limiting ─────────────────────────────────────────────────────────────
void HttpClient::wait_for_rate_limit(const std::string& domain) {
    if (!impl_->rate_limiting_enabled || domain.empty()) return;

    std::lock_guard<std::mutex> lock(impl_->rate_mutex);
    auto now = std::chrono::steady_clock::now();
    auto it = impl_->domain_last_request.find(domain);

    if (it != impl_->domain_last_request.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second).count();
        if (elapsed < impl_->rate_limit_ms) {
            int wait = impl_->rate_limit_ms - static_cast<int>(elapsed);
            spdlog::debug("[HttpClient] Rate limiting: waiting {}ms for {}", wait, domain);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        }
    }
    impl_->domain_last_request[domain] = std::chrono::steady_clock::now();
}

// ── Simple wrappers ────────────────────────────────────────────────────────────
HttpResponse HttpClient::get(const std::string& url,
                              const std::map<std::string, std::string>& extra) {
    HttpRequest req;
    req.method  = "GET";
    req.url     = url;
    req.headers = extra;
    return execute(req);
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& extra) {
    HttpRequest req;
    req.method  = "POST";
    req.url     = url;
    req.body    = body;
    req.headers = extra;
    return execute(req);
}

HttpResponse HttpClient::put(const std::string& url, const std::string& body,
                              const std::map<std::string, std::string>& extra) {
    HttpRequest req;
    req.method  = "PUT";
    req.url     = url;
    req.body    = body;
    req.headers = extra;
    return execute(req);
}

// ── Retry with exponential backoff ────────────────────────────────────────────
HttpResponse HttpClient::execute_with_retry(const HttpRequest& request) {
    HttpResponse last_resp;
    int max_retries = std::max(0, request.max_retries);

    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        // Rate limit before each attempt
        std::string domain = extract_domain(request.url);
        wait_for_rate_limit(domain);

        CURL* curl = impl_->curl;
        HttpResponse response;

        // Reset state for reuse
        curl_easy_reset(curl);

        // ── URL ──
        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());

        // ── Method ──
        if (request.method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(request.body.size()));
        } else if (request.method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(request.body.size()));
        } else if (request.method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }

        // ── Headers ──
        curl_slist* header_list = nullptr;
        for (const auto& [k, v] : impl_->default_headers) {
            std::string h = k + ": " + v;
            header_list = curl_slist_append(header_list, h.c_str());
        }
        for (const auto& [k, v] : request.headers) {
            std::string h = k + ": " + v;
            header_list = curl_slist_append(header_list, h.c_str());
        }
        if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

        // ── User-Agent ── (rotation or fixed)
        std::string ua = (request.rotate_user_agent && impl_->ua_rotation_enabled)
                         ? get_random_ua()
                         : impl_->user_agent;
        curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

        // ── Accept-Encoding (gzip/deflate) ──
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

        // ── Timeouts ──
        int timeout = request.timeout_seconds > 0
                      ? request.timeout_seconds : impl_->default_timeout;
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(timeout));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(request.connect_timeout));

        // ── Redirects ──
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
                         request.follow_redirects ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS,
                         static_cast<long>(request.max_redirects));

        // ── SSL ──
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, request.verify_ssl ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, request.verify_ssl ? 2L : 0L);

        // ── Proxy ──
        std::string proxy = !request.proxy.empty() ? request.proxy : impl_->proxy;
        if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

        // ── Callbacks ──
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &response.headers);

        // ── Execute ──
        auto t0       = std::chrono::steady_clock::now();
        CURLcode res  = curl_easy_perform(curl);
        auto t1       = std::chrono::steady_clock::now();
        response.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

        if (header_list) curl_slist_free_all(header_list);

        if (res != CURLE_OK) {
            response.success    = false;
            response.error      = curl_easy_strerror(res);
            response.retry_count = attempt;

            if (attempt < max_retries) {
                int backoff_ms = (1 << attempt) * 1000; // 1s, 2s, 4s
                spdlog::debug("[HttpClient] {} {} → curl error: {} (retry {}/{} in {}ms)",
                             request.method, request.url, response.error,
                             attempt + 1, max_retries, backoff_ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                continue;
            }

            spdlog::warn("[HttpClient] {} {} → curl error: {} (all retries exhausted)",
                         request.method, request.url, response.error);
            return response;
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.status_code = static_cast<int>(http_code);
        response.success     = (http_code >= 200 && http_code < 400);
        response.retry_count = attempt;

        // Retry on 429 (rate limit) or 503 (service unavailable)
        if ((http_code == 429 || http_code == 503) && attempt < max_retries) {
            int backoff_ms = (1 << attempt) * 1000;
            spdlog::debug("[HttpClient] {} {} → HTTP {} (retry {}/{} in {}ms)",
                         request.method, request.url, http_code,
                         attempt + 1, max_retries, backoff_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            continue;
        }

        spdlog::debug("[HttpClient] {} {} → {} ({} ms, {} bytes, {} retries)",
                      request.method, request.url, http_code,
                      response.elapsed.count(), response.body.size(), attempt);
        return response;
    }

    // Should not reach here, but just in case
    return last_resp;
}

// ── Core execute (delegates to retry logic) ───────────────────────────────────
HttpResponse HttpClient::execute(const HttpRequest& request) {
    return execute_with_retry(request);
}

// ── URL utilities ─────────────────────────────────────────────────────────────
std::string HttpClient::url_encode(const std::string& raw) {
    // Use curl's encoder momentarily
    CURL* tmp = curl_easy_init();
    if (!tmp) return raw;
    char* enc = curl_easy_escape(tmp, raw.c_str(), static_cast<int>(raw.size()));
    std::string result = enc ? enc : raw;
    curl_free(enc);
    curl_easy_cleanup(tmp);
    return result;
}

std::string HttpClient::build_query_string(
        const std::map<std::string, std::string>& params) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& [k, v] : params) {
        if (!first) oss << '&';
        oss << url_encode(k) << '=' << url_encode(v);
        first = false;
    }
    return oss.str();
}

} // namespace nobody