#pragma once
// =============================================================================
//  engine/IntelligenceEngine.h
//  Orchestrates the full Nobody pipeline:
//    1. Classify user intent
//    2. Generate dork queries based on intent
//    3. Run multi-backend search with dorking
//    4. Fusion-rank and deduplicate all results
//    5. Scrape top N results concurrently for full content
//    6. Multi-pass AI synthesis (map-reduce for deep mode)
//    7. Return a structured, cited answer
// =============================================================================

#include "core/HttpClient.h"
#include "search/SearchEngine.h"
#include "scraper/WebScraper.h"
#include "ai/AIBrain.h"

#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace nobody {

// ── Intent classification ─────────────────────────────────────────────────────
enum class QueryIntent {
    FACT_LOOKUP,        // "What is X?" — quick answer, mostly snippets
    DEEP_RESEARCH,      // "Explain / Analyse" — full page scrape needed
    PERSON_LOOKUP,      // name + context = Intelligence enrichment
    CURRENT_EVENTS,     // news-like, recency matters
    TECHNICAL,          // code, specs, documentation
    UNKNOWN
};

// ── Depth mode ────────────────────────────────────────────────────────────────
enum class DepthMode {
    QUICK,      // fast: fewer results, no deep scraping, single-pass AI
    STANDARD,   // balanced: moderate scraping, standard synthesis
    DEEP        // thorough: max results, link-following, map-reduce AI
};

// ── Full pipeline result ──────────────────────────────────────────────────────
struct NobodyResult {
    std::string            query;
    std::string            seed_url;        // if started from a URL
    QueryIntent            intent = QueryIntent::UNKNOWN;
    DepthMode              depth  = DepthMode::STANDARD;
    std::vector<SearchResult>  search_results;
    std::vector<ScrapedPage>   scraped_pages;
    std::vector<std::string>   dork_queries;    // generated dork queries
    AIResponse             ai_response;
    std::chrono::milliseconds total_time{0};
    int                    engines_used = 0;     // how many search engines returned results
    bool                   success = false;
};

// ── Config ────────────────────────────────────────────────────────────────────
struct EngineConfig {
    int  max_search_results = 15;
    int  pages_to_scrape    = 5;    // how many top results to full-scrape
    bool enable_scraping    = true;
    bool enable_dorking     = true;
    bool verbose            = false;
    DepthMode depth_mode    = DepthMode::STANDARD;
};

// ── NobodyEngine ───────────────────────────────────────────────────────────────
class NobodyEngine {
public:
    NobodyEngine(std::shared_ptr<HttpClient>  http,
                std::shared_ptr<SearchEngine> search,
                std::shared_ptr<WebScraper>   scraper,
                std::shared_ptr<AIBrain>       ai,
                EngineConfig config = {});

    // Run the full Nobody pipeline for a user query
    NobodyResult run(const std::string& query,
                    const std::vector<Message>& history = {});

    // Run pipeline starting from a seed URL — scrape it, extract topic,
    // then search the web for related content and synthesize everything
    NobodyResult run_from_url(const std::string& url,
                              const std::vector<Message>& history = {});

    // Check if a string looks like a URL
    static bool is_url(const std::string& input);

    // Just search + return raw results (no AI synthesis)
    std::vector<SearchResult> raw_search(const std::string& query);

    // Classify query intent (for strategy selection)
    static QueryIntent classify_intent(const std::string& query);

    void set_config(const EngineConfig& cfg) { config_ = cfg; }
    const EngineConfig& config() const { return config_; }

private:
    std::shared_ptr<HttpClient>   http_;
    std::shared_ptr<SearchEngine> search_;
    std::shared_ptr<WebScraper>   scraper_;
    std::shared_ptr<AIBrain>      ai_;
    EngineConfig config_;

    // Select which URLs to scrape based on intent
    std::vector<std::string> select_urls_to_scrape(
        const std::vector<SearchResult>& results,
        QueryIntent intent) const;

    // Get scraping and search parameters based on depth mode
    int get_max_results_for_depth(QueryIntent intent) const;
    int get_pages_to_scrape_for_depth(QueryIntent intent) const;
    SynthesisMode get_synthesis_mode() const;

    // Extract search queries from a scraped page (keyword-based, no LLM)
    static std::vector<std::string> extract_topics_from_page(
        const ScrapedPage& page, int max_queries = 3);

    static std::string intent_to_string(QueryIntent i);
    static std::string depth_to_string(DepthMode d);
};

} // namespace nobody