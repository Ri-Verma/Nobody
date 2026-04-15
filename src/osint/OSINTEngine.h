#pragma once
// =============================================================================
//  osint/OSINTEngine.h
//  Orchestrates the full OSINT pipeline:
//    1. Classify user intent
//    2. Build optimised search queries
//    3. Run multi-backend search
//    4. Scrape top N results for full content
//    5. Feed all context to AIBrain for synthesis
//    6. Return a structured, cited answer
// =============================================================================

#include "core/HttpClient.h"
#include "search/SearchEngine.h"
#include "scraper/WebScraper.h"
#include "ai/AIBrain.h"

#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace osint {

// ── Intent classification ─────────────────────────────────────────────────────
enum class QueryIntent {
    FACT_LOOKUP,        // "What is X?" — quick answer, mostly snippets
    DEEP_RESEARCH,      // "Explain / Analyse" — full page scrape needed
    PERSON_LOOKUP,      // name + context = OSINT enrichment
    CURRENT_EVENTS,     // news-like, recency matters
    TECHNICAL,          // code, specs, documentation
    UNKNOWN
};

// ── Full pipeline result ──────────────────────────────────────────────────────
struct OSINTResult {
    std::string            query;
    QueryIntent            intent = QueryIntent::UNKNOWN;
    std::vector<SearchResult>  search_results;
    std::vector<ScrapedPage>   scraped_pages;
    AIResponse             ai_response;
    std::chrono::milliseconds total_time{0};
    bool                   success = false;
};

// ── Config ────────────────────────────────────────────────────────────────────
struct EngineConfig {
    int  max_search_results = 8;
    int  pages_to_scrape    = 3;    // how many top results to full-scrape
    bool enable_scraping    = true;
    bool verbose            = false;
};

// ── OSINTEngine ───────────────────────────────────────────────────────────────
class OSINTEngine {
public:
    OSINTEngine(std::shared_ptr<HttpClient>  http,
                std::shared_ptr<SearchEngine> search,
                std::shared_ptr<WebScraper>   scraper,
                std::shared_ptr<AIBrain>       ai,
                EngineConfig config = {});

    // Run the full OSINT pipeline for a user query
    OSINTResult run(const std::string& query,
                    const std::vector<Message>& history = {});

    // Just search + return raw results (no AI synthesis)
    std::vector<SearchResult> raw_search(const std::string& query);

    // Classify query intent (for strategy selection)
    static QueryIntent classify_intent(const std::string& query);

    void set_config(const EngineConfig& cfg) { config_ = cfg; }

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

    static std::string intent_to_string(QueryIntent i);
};

} // namespace osint