#pragma once
// =============================================================================
//  search/SearchEngine.h
//  Queries multiple search backends and returns a unified result list.
//  Backends: DuckDuckGo Instant Answer API + HTML scrape, Google CSE (optional),
//            Brave Search API, Bing Web Search API, Wayback Machine CDX
//  Features: Google Dorking query expansion, BM25 ranking, domain authority,
//            reciprocal rank fusion, negative domain filtering
// =============================================================================

#include "core/HttpClient.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <map>

namespace nobody {

// ── A single search result ────────────────────────────────────────────────────
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;    // short description/extract
    std::string source;     // "duckduckgo" | "google" | "brave" | "bing" | "wayback"
    double      relevance   = 1.0; // 0–1 after ranking
    double      rrf_score   = 0.0; // reciprocal rank fusion bonus
    int         engine_rank = 0;   // rank within its source engine
    int         engine_hits = 1;   // how many engines returned this URL
    std::string date_hint;         // optional date from snippet/metadata
};

// ── DuckDuckGo Instant Answer data ───────────────────────────────────────────
struct DDGInstantAnswer {
    std::string abstract_text;   // main paragraph
    std::string abstract_url;
    std::string answer;          // direct answer (e.g. "42")
    std::string answer_type;
    std::string entity;          // Named entity if detected
    std::vector<SearchResult> related_topics;
};

// ── Dork configuration ───────────────────────────────────────────────────────
struct DorkConfig {
    bool        enabled              = true;
    int         max_dork_queries     = 3;   // how many dork variants to generate
    bool        use_site_operators   = true;
    bool        use_filetype         = true;
    bool        use_intitle          = true;
    bool        use_date_filter      = true;
    bool        use_negative_filter  = true;
    std::vector<std::string> negative_domains = {
        "pinterest.com", "quora.com", "ask.com", "answers.yahoo.com",
        "slideshare.net", "scribd.com"
    };
    std::vector<std::string> negative_terms = {
        "buy now", "subscribe", "sign up free", "download free"
    };
};

// ── Search configuration ──────────────────────────────────────────────────────
struct SearchConfig {
    int  max_results         = 15;
    bool use_duckduckgo      = true;
    bool use_google          = false;   // requires google_api_key + cx
    bool use_brave           = false;   // requires brave_api_key
    bool use_bing            = false;   // requires bing_api_key
    bool use_wayback         = true;    // Wayback Machine CDX (free, no key)
    std::string google_api_key;
    std::string google_cx;              // Custom Search Engine ID
    std::string brave_api_key;
    std::string bing_api_key;
    std::string language     = "en";
    bool safe_search         = false;
    std::string region;                 // e.g. "us-en"
    DorkConfig  dorking;                // Google Dorking config
};

// ── Forward declaration for intent ───────────────────────────────────────────
enum class QueryIntent;

// ── SearchEngine ──────────────────────────────────────────────────────────────
class SearchEngine {
public:
    explicit SearchEngine(std::shared_ptr<HttpClient> http,
                          SearchConfig config = {});

    // Search across all configured backends, deduplicate, rank, return top N
    std::vector<SearchResult> search(const std::string& query);

    // Multi-query search: runs multiple dork queries and fuses results
    std::vector<SearchResult> search_with_dorking(
        const std::string& query, QueryIntent intent);

    // DuckDuckGo-specific: rich instant answer (great for entities/facts)
    std::optional<DDGInstantAnswer> ddg_instant_answer(const std::string& query);

    // Individual engine searches (public for testing/direct use)
    std::vector<SearchResult> google_search(const std::string& query);
    std::vector<SearchResult> brave_search(const std::string& query);
    std::vector<SearchResult> bing_search(const std::string& query);
    std::vector<SearchResult> wayback_search(const std::string& query);

    // Google Dorking: generate optimised search queries from raw input
    std::vector<std::string> generate_dork_queries(
        const std::string& query, QueryIntent intent) const;

    // Build a focused Nobody query from raw user input
    static std::string build_nobody_query(const std::string& raw_query);

    void set_config(const SearchConfig& cfg) { config_ = cfg; }
    const SearchConfig& config() const { return config_; }

private:
    std::shared_ptr<HttpClient> http_;
    SearchConfig config_;

    std::vector<SearchResult> ddg_html_search(const std::string& query);
    std::vector<SearchResult> deduplicate(std::vector<SearchResult> results);

    // ── Ranking ──────────────────────────────────────────────────────────────
    void rank(std::vector<SearchResult>& results, const std::string& query);
    static double compute_bm25(const SearchResult& r, const std::string& query);
    static double domain_authority_score(const std::string& url);
    static double freshness_score(const SearchResult& r);

    // ── Fusion ───────────────────────────────────────────────────────────────
    std::vector<SearchResult> reciprocal_rank_fusion(
        const std::vector<std::vector<SearchResult>>& engine_results);

    // ── Dorking helpers ──────────────────────────────────────────────────────
    std::string build_negative_filter() const;
    static std::string build_site_filter(const std::vector<std::string>& domains);
    static std::string extract_date_filter(int days_back = 365);
};

} // namespace nobody