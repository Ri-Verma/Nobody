// =============================================================================
//  core/HttpClient.cpp
// =============================================================================

#include "core/HttpClient.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cassert>
#include <iomanip>

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

// ── Pimpl ─────────────────────────────────────────────────────────────────────
struct HttpClient::Impl {
    CURL*       curl        = nullptr;
    std::string user_agent;
    int         default_timeout = 30;
    std::string proxy;
    std::map<std::string, std::string> default_headers;

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

// ── Core execute ──────────────────────────────────────────────────────────────
HttpResponse HttpClient::execute(const HttpRequest& request) {
    HttpResponse response;
    CURL* curl = impl_->curl;

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
    // Add default headers
    for (const auto& [k, v] : impl_->default_headers) {
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    // Add request-specific headers (override defaults)
    for (const auto& [k, v] : request.headers) {
        std::string h = k + ": " + v;
        header_list = curl_slist_append(header_list, h.c_str());
    }
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    // ── User-Agent ──
    curl_easy_setopt(curl, CURLOPT_USERAGENT, impl_->user_agent.c_str());

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
        spdlog::warn("[HttpClient] {} {} → curl error: {}",
                     request.method, request.url, response.error);
        return response;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    response.status_code = static_cast<int>(http_code);
    response.success     = (http_code >= 200 && http_code < 400);

    spdlog::debug("[HttpClient] {} {} → {} ({} ms, {} bytes)",
                  request.method, request.url, http_code,
                  response.elapsed.count(), response.body.size());
    return response;
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