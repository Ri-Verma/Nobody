// =============================================================================
//  search/SearchEngine.cpp
// =============================================================================

#include "search/SearchEngine.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <cctype>

using json = nlohmann::json;

namespace nobody {

// ── Constructor ───────────────────────────────────────────────────────────────
SearchEngine::SearchEngine(std::shared_ptr<HttpClient> http, SearchConfig config)
    : http_(std::move(http)), config_(std::move(config)) {}

// ── Public search ─────────────────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::search(const std::string& query) {
    spdlog::info("[Search] Query: \"{}\"", query);
    std::vector<SearchResult> all;

    if (config_.use_duckduckgo) {
        auto ddg = ddg_html_search(query);
        all.insert(all.end(), ddg.begin(), ddg.end());
    }

    if (config_.use_google && !config_.google_api_key.empty()) {
        auto ggl = google_search(query);
        all.insert(all.end(), ggl.begin(), ggl.end());
    }

    all = deduplicate(std::move(all));
    rank(all, query);

    if (static_cast<int>(all.size()) > config_.max_results)
        all.resize(static_cast<size_t>(config_.max_results));

    spdlog::info("[Search] Returning {} results", all.size());
    return all;
}

// ── DuckDuckGo Instant Answer API ────────────────────────────────────────────
// GET https://api.duckduckgo.com/?q=<query>&format=json&no_html=1&skip_disambig=1
std::optional<DDGInstantAnswer> SearchEngine::ddg_instant_answer(
        const std::string& query) {
    const std::string base = "https://api.duckduckgo.com/";
    auto params = HttpClient::build_query_string({
        {"q",              query},
        {"format",         "json"},
        {"no_html",        "1"},
        {"skip_disambig",  "1"},
        {"no_redirect",    "1"},
    });

    auto resp = http_->get(base + "?" + params, {
        {"Accept", "application/json"}
    });

    if (!resp.is_ok()) {
        spdlog::warn("[Search] DDG instant answer failed: {}", resp.error);
        return std::nullopt;
    }

    try {
        auto j = json::parse(resp.body);
        DDGInstantAnswer ans;
        ans.abstract_text = j.value("AbstractText", "");
        ans.abstract_url  = j.value("AbstractURL",  "");
        ans.answer        = j.value("Answer",        "");
        ans.answer_type   = j.value("AnswerType",    "");
        ans.entity        = j.value("Entity",        "");

        // Related topics
        if (j.contains("RelatedTopics") && j["RelatedTopics"].is_array()) {
            for (const auto& topic : j["RelatedTopics"]) {
                if (!topic.is_object()) continue;
                SearchResult sr;
                sr.title   = topic.value("Text",       "");
                sr.url     = topic.value("FirstURL",   "");
                sr.snippet = sr.title;
                sr.source  = "duckduckgo-instant";
                if (!sr.url.empty()) ans.related_topics.push_back(std::move(sr));
            }
        }
        return ans;
    } catch (const std::exception& e) {
        spdlog::warn("[Search] DDG JSON parse error: {}", e.what());
        return std::nullopt;
    }
}

// ── DuckDuckGo HTML scrape ────────────────────────────────────────────────────
// DuckDuckGo HTML results don't require an API key.
// We scrape the HTML search results page for <a class="result__a"> links.
std::vector<SearchResult> SearchEngine::ddg_html_search(const std::string& query) {
    const std::string url = "https://html.duckduckgo.com/html/";
    auto body = HttpClient::build_query_string({{"q", query}, {"kl", "us-en"}});

    // DDG HTML endpoint requires POST
    auto resp = http_->post(url, body, {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept",       "text/html,application/xhtml+xml"},
        // Act like a real browser to avoid blocks
        {"Accept-Language", "en-US,en;q=0.9"},
    });

    if (!resp.is_ok()) {
        spdlog::warn("[Search] DDG HTML search failed ({}): {}",
                     resp.status_code, resp.error);
        return {};
    }

    std::vector<SearchResult> results;

    // ── Regex-based HTML scraping (avoids libxml2 dependency) ────────────────
    // Match: <a class="result__a" href="URL">TITLE</a>
    std::regex link_re(
        "<a[^>]+class=\"result__a\"[^>]+href=\"([^\"]+)\"[^>]*>(.*?)</a>",
        std::regex::icase | std::regex::ECMAScript
    );
    // Match: <a class="result__snippet">SNIPPET</a>
    std::regex snip_re(
        "<a[^>]+class=\"result__snippet\"[^>]*>(.*?)</a>",
        std::regex::icase | std::regex::ECMAScript
    );

    // Strip HTML tags helper
    auto strip_tags = [](const std::string& html) {
        std::regex tag(R"(<[^>]+>)");
        return std::regex_replace(html, tag, "");
    };

    auto it_link = std::sregex_iterator(resp.body.begin(), resp.body.end(), link_re);
    auto it_snip = std::sregex_iterator(resp.body.begin(), resp.body.end(), snip_re);
    auto end     = std::sregex_iterator{};

    std::vector<std::pair<std::string,std::string>> links; // {url, title}
    for (auto it = it_link; it != end; ++it) {
        std::string href  = (*it)[1].str();
        std::string title = strip_tags((*it)[2].str());
        if (!href.empty() && !title.empty()) links.push_back({href, title});
    }

    std::vector<std::string> snippets;
    for (auto it = it_snip; it != end; ++it)
        snippets.push_back(strip_tags((*it)[1].str()));

    for (size_t i = 0; i < links.size(); ++i) {
        SearchResult sr;
        sr.url     = links[i].first;
        sr.title   = links[i].second;
        sr.snippet = (i < snippets.size()) ? snippets[i] : "";
        sr.source  = "duckduckgo";
        results.push_back(std::move(sr));

        if (static_cast<int>(results.size()) >= config_.max_results) break;
    }

    spdlog::debug("[Search] DDG HTML: {} results", results.size());
    return results;
}

// ── Google Custom Search API ──────────────────────────────────────────────────
// https://developers.google.com/custom-search/v1/using_rest
std::vector<SearchResult> SearchEngine::google_search(const std::string& query) {
    if (config_.google_api_key.empty() || config_.google_cx.empty()) {
        spdlog::warn("[Search] Google API key or CX not set");
        return {};
    }

    auto params = HttpClient::build_query_string({
        {"key",    config_.google_api_key},
        {"cx",     config_.google_cx},
        {"q",      query},
        {"num",    std::to_string(std::min(config_.max_results, 10))},
        {"hl",     config_.language},
    });

    const std::string url =
        "https://www.googleapis.com/customsearch/v1?" + params;

    auto resp = http_->get(url, {{"Accept", "application/json"}});
    if (!resp.is_ok()) {
        spdlog::warn("[Search] Google CSE failed: {}", resp.error);
        return {};
    }

    std::vector<SearchResult> results;
    try {
        auto j = json::parse(resp.body);
        if (!j.contains("items")) return results;
        for (const auto& item : j["items"]) {
            SearchResult sr;
            sr.title   = item.value("title",   "");
            sr.url     = item.value("link",    "");
            sr.snippet = item.value("snippet", "");
            sr.source  = "google";
            results.push_back(std::move(sr));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Search] Google JSON parse error: {}", e.what());
    }

    return results;
}

// ── Deduplication ─────────────────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::deduplicate(
        std::vector<SearchResult> results) {
    std::set<std::string> seen_urls;
    std::vector<SearchResult> out;
    for (auto& r : results) {
        // Normalise: strip trailing slash and fragment
        std::string key = r.url;
        if (!key.empty() && key.back() == '/') key.pop_back();
        auto hash_pos = key.find('#');
        if (hash_pos != std::string::npos) key = key.substr(0, hash_pos);

        if (seen_urls.insert(key).second) out.push_back(std::move(r));
    }
    return out;
}

// ── Ranking ───────────────────────────────────────────────────────────────────
void SearchEngine::rank(std::vector<SearchResult>& results,
                         const std::string& query) {
    for (auto& r : results) r.relevance = compute_relevance(r, query);
    std::stable_sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b){
            return a.relevance > b.relevance;
        });
}

double SearchEngine::compute_relevance(const SearchResult& r,
                                        const std::string& query) {
    // Simple TF-like relevance: how many query words appear in title+snippet
    std::istringstream ss(query);
    std::string word;
    double score = 0.0;
    int n = 0;
    while (ss >> word) {
        ++n;
        // Case-insensitive search
        auto ci_find = [](const std::string& haystack, const std::string& needle) {
            auto it = std::search(haystack.begin(), haystack.end(),
                                   needle.begin(),  needle.end(),
                                   [](char a, char b){ return ::tolower(a)==::tolower(b); });
            return it != haystack.end();
        };
        if (ci_find(r.title,   word)) score += 2.0;
        if (ci_find(r.snippet, word)) score += 1.0;
    }
    return n > 0 ? score / (n * 3.0) : 0.5;
}

// ── Nobody query builder ──────────────────────────────────────────────────────
std::string SearchEngine::build_nobody_query(const std::string& raw) {
    // Could be extended with dork-style operators, entity detection, etc.
    return raw;
}

} // namespace nobody