#pragma once
// =============================================================================
//  search/SearchEngine.h
//  Queries multiple search backends and returns a unified result list.
//  Backends: DuckDuckGo Instant Answer API + HTML scrape, Google CSE (optional)
// =============================================================================

#include "core/HttpClient.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace nobody {

// ── A single search result ────────────────────────────────────────────────────
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;    // short description/extract
    std::string source;     // "duckduckgo" | "google" | etc.
    double      relevance   = 1.0; // 0–1 after ranking
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

// ── Search configuration ──────────────────────────────────────────────────────
struct SearchConfig {
    int  max_results         = 10;
    bool use_duckduckgo      = true;
    bool use_google          = false;   // requires google_api_key + cx
    std::string google_api_key;
    std::string google_cx;              // Custom Search Engine ID
    std::string language     = "en";
    bool safe_search         = false;
    std::string region;                 // e.g. "us-en"
};

// ── SearchEngine ──────────────────────────────────────────────────────────────
class SearchEngine {
public:
    explicit SearchEngine(std::shared_ptr<HttpClient> http,
                          SearchConfig config = {});

    // Search across all configured backends, deduplicate, rank, return top N
    std::vector<SearchResult> search(const std::string& query);

    // DuckDuckGo-specific: rich instant answer (great for entities/facts)
    std::optional<DDGInstantAnswer> ddg_instant_answer(const std::string& query);

    // Google Custom Search (requires API key)
    std::vector<SearchResult> google_search(const std::string& query);

    // Build a focused Nobody query from raw user input
    static std::string build_nobody_query(const std::string& raw_query);

    void set_config(const SearchConfig& cfg) { config_ = cfg; }
    const SearchConfig& config() const { return config_; }

private:
    std::shared_ptr<HttpClient> http_;
    SearchConfig config_;

    std::vector<SearchResult> ddg_html_search(const std::string& query);
    std::vector<SearchResult> deduplicate(std::vector<SearchResult> results);
    void rank(std::vector<SearchResult>& results, const std::string& query);
    static double compute_relevance(const SearchResult& r,
                                     const std::string& query);
};

} // namespace nobody