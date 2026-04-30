// =============================================================================
//  search/SearchEngine.cpp — Multi-engine search with Google Dorking & BM25
// =============================================================================

#include "search/SearchEngine.h"
#include "engine/IntelligenceEngine.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <cctype>
#include <cmath>
#include <ctime>
#include <chrono>

using json = nlohmann::json;

namespace nobody {

// ── Constructor ───────────────────────────────────────────────────────────────
SearchEngine::SearchEngine(std::shared_ptr<HttpClient> http, SearchConfig config)
    : http_(std::move(http)), config_(std::move(config)) {}

// ── Public search (single query) ──────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::search(const std::string& query) {
    spdlog::info("[Search] Query: \"{}\"", query);
    std::vector<std::vector<SearchResult>> all_engine_results;

    if (config_.use_duckduckgo) {
        auto ddg = ddg_html_search(query);
        if (!ddg.empty()) all_engine_results.push_back(std::move(ddg));
    }
    if (config_.use_google && !config_.google_api_key.empty()) {
        auto ggl = google_search(query);
        if (!ggl.empty()) all_engine_results.push_back(std::move(ggl));
    }
    if (config_.use_brave && !config_.brave_api_key.empty()) {
        auto brv = brave_search(query);
        if (!brv.empty()) all_engine_results.push_back(std::move(brv));
    }
    if (config_.use_bing && !config_.bing_api_key.empty()) {
        auto bng = bing_search(query);
        if (!bng.empty()) all_engine_results.push_back(std::move(bng));
    }
    if (config_.use_wayback) {
        auto wb = wayback_search(query);
        if (!wb.empty()) all_engine_results.push_back(std::move(wb));
    }

    // Fuse results from all engines
    auto fused = reciprocal_rank_fusion(all_engine_results);
    rank(fused, query);

    if (static_cast<int>(fused.size()) > config_.max_results)
        fused.resize(static_cast<size_t>(config_.max_results));

    spdlog::info("[Search] Returning {} results from {} engines",
                 fused.size(), all_engine_results.size());
    return fused;
}

// ── Multi-query search with dorking ───────────────────────────────────────────
std::vector<SearchResult> SearchEngine::search_with_dorking(
        const std::string& query, QueryIntent intent) {

    if (!config_.dorking.enabled) return search(query);

    auto dork_queries = generate_dork_queries(query, intent);
    spdlog::info("[Search] Generated {} dork queries", dork_queries.size());

    std::vector<std::vector<SearchResult>> all_results;
    for (const auto& dq : dork_queries) {
        spdlog::debug("[Search] Dorking: \"{}\"", dq);
        auto results = search(dq);
        if (!results.empty()) all_results.push_back(std::move(results));
    }

    auto fused = reciprocal_rank_fusion(all_results);
    rank(fused, query); // rank against original query

    if (static_cast<int>(fused.size()) > config_.max_results)
        fused.resize(static_cast<size_t>(config_.max_results));

    return fused;
}

// ── Google Dorking query generation ───────────────────────────────────────────
std::vector<std::string> SearchEngine::generate_dork_queries(
        const std::string& query, QueryIntent intent) const {

    std::vector<std::string> queries;
    queries.push_back(query); // always include original

    std::string neg = build_negative_filter();
    int max_dorks = config_.dorking.max_dork_queries;

    switch (intent) {
        case QueryIntent::FACT_LOOKUP: {
            queries.push_back(fmt::format("\"{}\" site:wikipedia.org OR site:britannica.com", query));
            if (max_dorks > 2)
                queries.push_back(fmt::format("\"{}\" {}", query, neg));
            break;
        }
        case QueryIntent::DEEP_RESEARCH: {
            queries.push_back(fmt::format("\"{}\" site:edu OR site:gov OR site:nature.com OR site:arxiv.org", query));
            if (max_dorks > 2)
                queries.push_back(fmt::format("intitle:\"{}\" {}", query, neg));
            if (max_dorks > 3)
                queries.push_back(fmt::format("\"{}\" filetype:pdf", query));
            break;
        }
        case QueryIntent::PERSON_LOOKUP: {
            queries.push_back(fmt::format("\"{}\" site:linkedin.com OR site:crunchbase.com OR site:wikipedia.org", query));
            if (max_dorks > 2)
                queries.push_back(fmt::format("\"{}\" CEO OR founder OR biography {}", query, neg));
            break;
        }
        case QueryIntent::CURRENT_EVENTS: {
            std::string date_filter = extract_date_filter(30);
            queries.push_back(fmt::format("\"{}\" site:reuters.com OR site:bbc.com OR site:apnews.com", query));
            if (max_dorks > 2)
                queries.push_back(fmt::format("\"{}\" {} {}", query, date_filter, neg));
            break;
        }
        case QueryIntent::TECHNICAL: {
            queries.push_back(fmt::format("\"{}\" site:stackoverflow.com OR site:github.com OR site:developer.mozilla.org", query));
            if (max_dorks > 2)
                queries.push_back(fmt::format("\"{}\" inurl:docs OR inurl:api OR inurl:reference {}", query, neg));
            break;
        }
        default: {
            queries.push_back(fmt::format("\"{}\" {}", query, neg));
            break;
        }
    }

    // Cap at configured max
    if (static_cast<int>(queries.size()) > max_dorks + 1)
        queries.resize(static_cast<size_t>(max_dorks + 1));

    return queries;
}

// ── Dorking helpers ───────────────────────────────────────────────────────────
std::string SearchEngine::build_negative_filter() const {
    std::string filter;
    for (const auto& dom : config_.dorking.negative_domains)
        filter += " -site:" + dom;
    for (const auto& term : config_.dorking.negative_terms)
        filter += " -\"" + term + "\"";
    return filter;
}

std::string SearchEngine::build_site_filter(const std::vector<std::string>& domains) {
    if (domains.empty()) return "";
    std::string f = "site:" + domains[0];
    for (size_t i = 1; i < domains.size(); ++i)
        f += " OR site:" + domains[i];
    return f;
}

std::string SearchEngine::extract_date_filter(int days_back) {
    auto now = std::chrono::system_clock::now();
    auto past = now - std::chrono::hours(24 * days_back);
    auto t = std::chrono::system_clock::to_time_t(past);
    std::tm tm_val;
    localtime_r(&t, &tm_val);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_val);
    return fmt::format("after:{}", buf);
}

// ── DuckDuckGo Instant Answer API ────────────────────────────────────────────
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
std::vector<SearchResult> SearchEngine::ddg_html_search(const std::string& query) {
    const std::string url = "https://html.duckduckgo.com/html/";
    auto body = HttpClient::build_query_string({{"q", query}, {"kl", "us-en"}});

    auto resp = http_->post(url, body, {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept",       "text/html,application/xhtml+xml"},
        {"Accept-Language", "en-US,en;q=0.9"},
    });

    if (!resp.is_ok()) {
        spdlog::warn("[Search] DDG HTML search failed ({}): {}",
                     resp.status_code, resp.error);
        return {};
    }

    std::vector<SearchResult> results;

    std::regex link_re(
        "<a[^>]+class=\"result__a\"[^>]+href=\"([^\"]+)\"[^>]*>(.*?)</a>",
        std::regex::icase | std::regex::ECMAScript
    );
    std::regex snip_re(
        "<a[^>]+class=\"result__snippet\"[^>]*>(.*?)</a>",
        std::regex::icase | std::regex::ECMAScript
    );

    auto strip_tags = [](const std::string& html) {
        std::regex tag(R"(<[^>]+>)");
        return std::regex_replace(html, tag, "");
    };

    auto it_link = std::sregex_iterator(resp.body.begin(), resp.body.end(), link_re);
    auto it_snip = std::sregex_iterator(resp.body.begin(), resp.body.end(), snip_re);
    auto end     = std::sregex_iterator{};

    std::vector<std::pair<std::string,std::string>> links;
    for (auto it = it_link; it != end; ++it) {
        std::string href  = (*it)[1].str();
        std::string title = strip_tags((*it)[2].str());
        if (!href.empty() && !title.empty()) links.push_back({href, title});
    }

    std::vector<std::string> snippets;
    for (auto it = it_snip; it != end; ++it)
        snippets.push_back(strip_tags((*it)[1].str()));

    int rank_idx = 1;
    for (size_t i = 0; i < links.size(); ++i) {
        SearchResult sr;
        sr.url     = links[i].first;
        sr.title   = links[i].second;
        sr.snippet = (i < snippets.size()) ? snippets[i] : "";
        sr.source  = "duckduckgo";
        sr.engine_rank = rank_idx++;
        results.push_back(std::move(sr));

        if (static_cast<int>(results.size()) >= config_.max_results) break;
    }

    spdlog::debug("[Search] DDG HTML: {} results", results.size());
    return results;
}

// ── Google Custom Search API ──────────────────────────────────────────────────
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

    const std::string url = "https://www.googleapis.com/customsearch/v1?" + params;
    auto resp = http_->get(url, {{"Accept", "application/json"}});
    if (!resp.is_ok()) {
        spdlog::warn("[Search] Google CSE failed: {}", resp.error);
        return {};
    }

    std::vector<SearchResult> results;
    try {
        auto j = json::parse(resp.body);
        if (!j.contains("items")) return results;
        int rank_idx = 1;
        for (const auto& item : j["items"]) {
            SearchResult sr;
            sr.title   = item.value("title",   "");
            sr.url     = item.value("link",    "");
            sr.snippet = item.value("snippet", "");
            sr.source  = "google";
            sr.engine_rank = rank_idx++;
            results.push_back(std::move(sr));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Search] Google JSON parse error: {}", e.what());
    }
    return results;
}

// ── Brave Search API ──────────────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::brave_search(const std::string& query) {
    if (config_.brave_api_key.empty()) {
        spdlog::warn("[Search] Brave API key not set");
        return {};
    }

    auto params = HttpClient::build_query_string({
        {"q",      query},
        {"count",  std::to_string(std::min(config_.max_results, 20))},
    });

    const std::string url = "https://api.search.brave.com/res/v1/web/search?" + params;
    auto resp = http_->get(url, {
        {"Accept", "application/json"},
        {"Accept-Encoding", "gzip"},
        {"X-Subscription-Token", config_.brave_api_key},
    });

    if (!resp.is_ok()) {
        spdlog::warn("[Search] Brave search failed ({}): {}", resp.status_code, resp.error);
        return {};
    }

    std::vector<SearchResult> results;
    try {
        auto j = json::parse(resp.body);
        if (!j.contains("web") || !j["web"].contains("results")) return results;
        int rank_idx = 1;
        for (const auto& item : j["web"]["results"]) {
            SearchResult sr;
            sr.title   = item.value("title",       "");
            sr.url     = item.value("url",          "");
            sr.snippet = item.value("description",  "");
            sr.source  = "brave";
            sr.engine_rank = rank_idx++;
            if (item.contains("age")) sr.date_hint = item.value("age", "");
            results.push_back(std::move(sr));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Search] Brave JSON parse error: {}", e.what());
    }

    spdlog::debug("[Search] Brave: {} results", results.size());
    return results;
}

// ── Bing Web Search API ───────────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::bing_search(const std::string& query) {
    if (config_.bing_api_key.empty()) {
        spdlog::warn("[Search] Bing API key not set");
        return {};
    }

    auto params = HttpClient::build_query_string({
        {"q",      query},
        {"count",  std::to_string(std::min(config_.max_results, 50))},
        {"mkt",    "en-US"},
    });

    const std::string url = "https://api.bing.microsoft.com/v7.0/search?" + params;
    auto resp = http_->get(url, {
        {"Ocp-Apim-Subscription-Key", config_.bing_api_key},
    });

    if (!resp.is_ok()) {
        spdlog::warn("[Search] Bing search failed ({}): {}", resp.status_code, resp.error);
        return {};
    }

    std::vector<SearchResult> results;
    try {
        auto j = json::parse(resp.body);
        if (!j.contains("webPages") || !j["webPages"].contains("value")) return results;
        int rank_idx = 1;
        for (const auto& item : j["webPages"]["value"]) {
            SearchResult sr;
            sr.title   = item.value("name",    "");
            sr.url     = item.value("url",     "");
            sr.snippet = item.value("snippet", "");
            sr.source  = "bing";
            sr.engine_rank = rank_idx++;
            if (item.contains("dateLastCrawled"))
                sr.date_hint = item.value("dateLastCrawled", "");
            results.push_back(std::move(sr));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Search] Bing JSON parse error: {}", e.what());
    }

    spdlog::debug("[Search] Bing: {} results", results.size());
    return results;
}

// ── Wayback Machine CDX search ────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::wayback_search(const std::string& query) {
    // Use DuckDuckGo to find relevant URLs, then check Wayback for archives
    // This is a lightweight supplementary source
    auto params = HttpClient::build_query_string({
        {"url",       "*." + query + ".*"},
        {"output",    "json"},
        {"limit",     "5"},
        {"fl",        "original,timestamp,statuscode"},
        {"filter",    "statuscode:200"},
        {"collapse",  "urlkey"},
    });

    const std::string url = "http://web.archive.org/cdx/search/cdx?" + params;
    HttpRequest req;
    req.url = url;
    req.timeout_seconds = 10;
    req.max_retries = 1;
    auto resp = http_->execute(req);

    if (!resp.is_ok()) {
        spdlog::debug("[Search] Wayback CDX failed: {}", resp.error);
        return {};
    }

    std::vector<SearchResult> results;
    try {
        auto j = json::parse(resp.body);
        if (!j.is_array() || j.size() < 2) return results;
        // Skip header row
        for (size_t i = 1; i < j.size() && results.size() < 5; ++i) {
            if (!j[i].is_array() || j[i].size() < 3) continue;
            SearchResult sr;
            sr.url     = j[i][0].get<std::string>();
            sr.title   = "Archived: " + sr.url;
            sr.snippet = "Wayback Machine archive from " + j[i][1].get<std::string>();
            sr.source  = "wayback";
            sr.date_hint = j[i][1].get<std::string>();
            sr.engine_rank = static_cast<int>(i);
            results.push_back(std::move(sr));
        }
    } catch (...) {
        spdlog::debug("[Search] Wayback parse error");
    }

    spdlog::debug("[Search] Wayback: {} results", results.size());
    return results;
}

// ── Reciprocal Rank Fusion ────────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::reciprocal_rank_fusion(
        const std::vector<std::vector<SearchResult>>& engine_results) {

    const double k = 60.0; // RRF constant
    std::map<std::string, SearchResult> url_map;  // URL -> merged result
    std::map<std::string, double> rrf_scores;

    for (const auto& results : engine_results) {
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            std::string key = r.url;
            // Normalize URL
            if (!key.empty() && key.back() == '/') key.pop_back();
            auto hash_pos = key.find('#');
            if (hash_pos != std::string::npos) key = key.substr(0, hash_pos);

            double score = 1.0 / (k + static_cast<double>(i + 1));
            rrf_scores[key] += score;

            if (url_map.find(key) == url_map.end()) {
                url_map[key] = r;
                url_map[key].engine_hits = 1;
            } else {
                url_map[key].engine_hits++;
                // Keep the better snippet
                if (r.snippet.size() > url_map[key].snippet.size())
                    url_map[key].snippet = r.snippet;
                if (url_map[key].title.empty() && !r.title.empty())
                    url_map[key].title = r.title;
            }
        }
    }

    std::vector<SearchResult> fused;
    for (auto& [url, sr] : url_map) {
        sr.rrf_score = rrf_scores[url];
        fused.push_back(std::move(sr));
    }

    // Sort by RRF score (descending)
    std::stable_sort(fused.begin(), fused.end(),
        [](const SearchResult& a, const SearchResult& b) {
            return a.rrf_score > b.rrf_score;
        });

    return fused;
}

// ── Deduplication ─────────────────────────────────────────────────────────────
std::vector<SearchResult> SearchEngine::deduplicate(
        std::vector<SearchResult> results) {
    std::set<std::string> seen_urls;
    std::vector<SearchResult> out;
    for (auto& r : results) {
        std::string key = r.url;
        if (!key.empty() && key.back() == '/') key.pop_back();
        auto hash_pos = key.find('#');
        if (hash_pos != std::string::npos) key = key.substr(0, hash_pos);
        if (seen_urls.insert(key).second) out.push_back(std::move(r));
    }
    return out;
}

// ── Ranking (BM25 + domain authority + freshness) ─────────────────────────────
void SearchEngine::rank(std::vector<SearchResult>& results,
                         const std::string& query) {
    for (auto& r : results) {
        double bm25  = compute_bm25(r, query);
        double auth  = domain_authority_score(r.url);
        double fresh = freshness_score(r);
        double multi = (r.engine_hits > 1) ? 0.1 * r.engine_hits : 0.0;

        // Weighted combination
        r.relevance = 0.45 * bm25 + 0.25 * auth + 0.15 * fresh
                    + 0.10 * r.rrf_score * 10.0 + 0.05 * multi;
    }
    std::stable_sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b){
            return a.relevance > b.relevance;
        });
}

// ── BM25 scoring ──────────────────────────────────────────────────────────────
double SearchEngine::compute_bm25(const SearchResult& r,
                                    const std::string& query) {
    const double k1 = 1.5;
    const double b  = 0.75;

    std::string text = r.title + " " + r.snippet;
    // Tokenize
    std::istringstream text_ss(text);
    std::vector<std::string> doc_tokens;
    std::string tok;
    while (text_ss >> tok) {
        std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
        doc_tokens.push_back(tok);
    }

    double avg_dl = 50.0; // assumed average doc length
    double dl = static_cast<double>(doc_tokens.size());
    double score = 0.0;

    std::istringstream q_ss(query);
    std::string q_word;
    while (q_ss >> q_word) {
        std::transform(q_word.begin(), q_word.end(), q_word.begin(), ::tolower);
        int tf = 0;
        for (const auto& t : doc_tokens)
            if (t.find(q_word) != std::string::npos) ++tf;

        if (tf > 0) {
            double idf = std::log(1.0 + 1.0); // simplified IDF (single doc)
            double num = static_cast<double>(tf) * (k1 + 1.0);
            double den = static_cast<double>(tf) + k1 * (1.0 - b + b * dl / avg_dl);
            score += idf * (num / den);
        }
    }

    // Normalize to 0-1 range approximately
    return std::min(1.0, score / 5.0);
}

// ── Domain authority scoring ──────────────────────────────────────────────────
double SearchEngine::domain_authority_score(const std::string& url) {
    // Tier 1: highest authority
    static const std::vector<std::string> tier1 = {
        "wikipedia.org", "britannica.com", "nature.com", "science.org",
        "arxiv.org", "nih.gov", "cdc.gov", "who.int", "un.org"
    };
    // Tier 2: high authority
    static const std::vector<std::string> tier2 = {
        "bbc.com", "reuters.com", "apnews.com", "nytimes.com",
        "theguardian.com", "washingtonpost.com", "github.com",
        "developer.mozilla.org", "docs.python.org", "cppreference.com",
        "stackoverflow.com", "microsoft.com", "oracle.com"
    };
    // Tier penalty: low quality
    static const std::vector<std::string> penalty = {
        "pinterest.com", "quora.com", "ask.com", "answers.yahoo.com",
        "slideshare.net", "scribd.com", "medium.com"
    };

    for (const auto& d : tier1)
        if (url.find(d) != std::string::npos) return 1.0;
    for (const auto& d : tier2)
        if (url.find(d) != std::string::npos) return 0.8;
    // .edu and .gov domains
    if (url.find(".edu") != std::string::npos) return 0.9;
    if (url.find(".gov") != std::string::npos) return 0.9;
    for (const auto& d : penalty)
        if (url.find(d) != std::string::npos) return 0.2;

    return 0.5; // default
}

// ── Freshness scoring ─────────────────────────────────────────────────────────
double SearchEngine::freshness_score(const SearchResult& r) {
    if (r.date_hint.empty()) return 0.5; // unknown date = neutral
    // Simple heuristic: if date_hint contains recent year, boost
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val;
    localtime_r(&t, &tm_val);
    std::string current_year = std::to_string(1900 + tm_val.tm_year);
    std::string last_year = std::to_string(1899 + tm_val.tm_year);

    if (r.date_hint.find(current_year) != std::string::npos) return 1.0;
    if (r.date_hint.find(last_year) != std::string::npos) return 0.8;
    return 0.4;
}

// ── Nobody query builder ──────────────────────────────────────────────────────
std::string SearchEngine::build_nobody_query(const std::string& raw) {
    return raw;
}

} // namespace nobody