// =============================================================================
//  engine/IntelligenceEngine.cpp
// =============================================================================

#include "engine/IntelligenceEngine.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <regex>

namespace nobody {

// ── Constructor ───────────────────────────────────────────────────────────────
NobodyEngine::NobodyEngine(
        std::shared_ptr<HttpClient>   http,
        std::shared_ptr<SearchEngine> search,
        std::shared_ptr<WebScraper>   scraper,
        std::shared_ptr<AIBrain>      ai,
        EngineConfig config)
    : http_(std::move(http))
    , search_(std::move(search))
    , scraper_(std::move(scraper))
    , ai_(std::move(ai))
    , config_(std::move(config)) {}

// ── run (main pipeline) ───────────────────────────────────────────────────────
NobodyResult NobodyEngine::run(const std::string& query,
                               const std::vector<Message>& history) {
    auto t_start = std::chrono::steady_clock::now();

    NobodyResult result;
    result.query  = query;
    result.intent = classify_intent(query);

    spdlog::info("[Nobody] ── Pipeline start ───────────────────────────────");
    spdlog::info("[Nobody] Query  : \"{}\"", query);
    spdlog::info("[Nobody] Intent : {}", intent_to_string(result.intent));

    // ── Step 1: Search ────────────────────────────────────────────────────
    spdlog::info("[Nobody] Step 1/3: Web search...");
    result.search_results = search_->search(query);

    // Also try instant answer for fact lookups
    if (result.intent == QueryIntent::FACT_LOOKUP ||
        result.intent == QueryIntent::PERSON_LOOKUP) {
        auto instant = search_->ddg_instant_answer(query);
        if (instant && !instant->abstract_text.empty()) {
            // Prepend as a high-relevance result
            SearchResult sr;
            sr.title    = "DuckDuckGo Instant Answer";
            sr.url      = instant->abstract_url;
            sr.snippet  = instant->abstract_text;
            sr.source   = "duckduckgo-instant";
            sr.relevance = 1.0;
            result.search_results.insert(result.search_results.begin(), sr);
        }
    }

    spdlog::info("[Nobody] Got {} search results", result.search_results.size());

    // ── Step 2: Scrape ────────────────────────────────────────────────────
    if (config_.enable_scraping && !result.search_results.empty()) {
        spdlog::info("[Nobody] Step 2/3: Scraping web pages...");
        auto urls = select_urls_to_scrape(result.search_results, result.intent);
        result.scraped_pages = scraper_->scrape_many(urls, config_.pages_to_scrape);
        spdlog::info("[Nobody] Scraped {} pages successfully",
                     result.scraped_pages.size());
    } else {
        spdlog::info("[Nobody] Step 2/3: Scraping skipped");
    }

    // ── Step 3: Synthesis ─────────────────────────────────────────────────
    spdlog::info("[Nobody] Step 3/3: AI Synthesis...");
    result.ai_response = ai_->synthesise(
        query, result.search_results, result.scraped_pages, history
    );

    result.success    = result.ai_response.success;
    result.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start
    );

    spdlog::info("[Nobody] ── Pipeline complete ({} ms) ────────────────",
                 result.total_time.count());

    return result;
}

// ── raw_search ────────────────────────────────────────────────────────────────
std::vector<SearchResult> NobodyEngine::raw_search(const std::string& query) {
    return search_->search(query);
}

// ── Intent classification ─────────────────────────────────────────────────────
QueryIntent NobodyEngine::classify_intent(const std::string& query) {
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    // News/current events
    static const std::vector<std::string> news_kw =
        {"latest", "recent", "today", "breaking", "news", "now", "2024","2025","current"};
    for (const auto& kw : news_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::CURRENT_EVENTS;

    // Person lookup
    static const std::regex person_re(
        R"(\bwho\s+is\b|\bfind\s+(?:info|information)\s+(?:on|about)\b|\b(?:ceo|founder|cto|president|director)\s+of\b)",
        std::regex::icase);
    if (std::regex_search(query, person_re)) return QueryIntent::PERSON_LOOKUP;

    // Technical
    static const std::vector<std::string> tech_kw =
        {"how to","tutorial","code","api","library","install","configure",
         "error","exception","function","class","syntax","algorithm"};
    for (const auto& kw : tech_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::TECHNICAL;

    // Deep research
    static const std::vector<std::string> deep_kw =
        {"explain","analyse","analyze","compare","overview","history",
         "impact","effect","cause","relationship","difference","why","how does"};
    for (const auto& kw : deep_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::DEEP_RESEARCH;

    // Simple fact
    static const std::vector<std::string> fact_kw =
        {"what is","what are","define","when was","where is","who invented"};
    for (const auto& kw : fact_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::FACT_LOOKUP;

    return QueryIntent::UNKNOWN;
}

// ── select_urls_to_scrape ─────────────────────────────────────────────────────
std::vector<std::string> NobodyEngine::select_urls_to_scrape(
        const std::vector<SearchResult>& results,
        QueryIntent intent) const {

    // For fact lookups, only scrape top 1–2 (quick answer needed)
    // For deep research, scrape more
    int desired = config_.pages_to_scrape;
    if (intent == QueryIntent::FACT_LOOKUP) desired = std::min(desired, 2);

    // Prefer Wikipedia, official sites, authoritative domains
    static const std::vector<std::string> priority_domains =
        {"wikipedia.org","britannica.com","gov","edu","reuters.com","bbc.com","nature.com"};

    std::vector<std::string> priority_urls, other_urls;
    for (const auto& r : results) {
        bool is_priority = false;
        for (const auto& dom : priority_domains) {
            if (r.url.find(dom) != std::string::npos) { is_priority = true; break; }
        }
        if (is_priority) priority_urls.push_back(r.url);
        else             other_urls.push_back(r.url);
    }

    std::vector<std::string> out;
    for (const auto& u : priority_urls) { out.push_back(u); if ((int)out.size()>=desired) break; }
    for (const auto& u : other_urls)    { out.push_back(u); if ((int)out.size()>=desired) break; }
    return out;
}

// ── intent_to_string ─────────────────────────────────────────────────────────
std::string NobodyEngine::intent_to_string(QueryIntent i) {
    switch (i) {
        case QueryIntent::FACT_LOOKUP:     return "FACT_LOOKUP";
        case QueryIntent::DEEP_RESEARCH:   return "DEEP_RESEARCH";
        case QueryIntent::PERSON_LOOKUP:   return "PERSON_LOOKUP";
        case QueryIntent::CURRENT_EVENTS:  return "CURRENT_EVENTS";
        case QueryIntent::TECHNICAL:       return "TECHNICAL";
        default:                           return "UNKNOWN";
    }
}

} // namespace nobody