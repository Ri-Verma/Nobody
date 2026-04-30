// =============================================================================
//  engine/IntelligenceEngine.cpp — Dorking pipeline, depth modes, concurrent
// =============================================================================

#include "engine/IntelligenceEngine.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <set>
#include <map>

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
    result.depth  = config_.depth_mode;

    spdlog::info("[Nobody] ── Pipeline start ───────────────────────────────");
    spdlog::info("[Nobody] Query  : \"{}\"", query);
    spdlog::info("[Nobody] Intent : {}", intent_to_string(result.intent));
    spdlog::info("[Nobody] Depth  : {}", depth_to_string(result.depth));

    // ── Step 1: Search (with optional dorking) ────────────────────────────
    spdlog::info("[Nobody] Step 1/3: Web search...");
    if (config_.enable_dorking) {
        result.search_results = search_->search_with_dorking(query, result.intent);
        result.dork_queries = search_->generate_dork_queries(query, result.intent);
        spdlog::info("[Nobody] Used {} dork queries", result.dork_queries.size());
    } else {
        result.search_results = search_->search(query);
    }

    // Also try instant answer for fact lookups
    if (result.intent == QueryIntent::FACT_LOOKUP ||
        result.intent == QueryIntent::PERSON_LOOKUP) {
        auto instant = search_->ddg_instant_answer(query);
        if (instant && !instant->abstract_text.empty()) {
            SearchResult sr;
            sr.title    = "DuckDuckGo Instant Answer";
            sr.url      = instant->abstract_url;
            sr.snippet  = instant->abstract_text;
            sr.source   = "duckduckgo-instant";
            sr.relevance = 1.0;
            result.search_results.insert(result.search_results.begin(), sr);
        }
    }

    // Adjust result count based on depth
    int max_results = get_max_results_for_depth(result.intent);
    if (static_cast<int>(result.search_results.size()) > max_results)
        result.search_results.resize(static_cast<size_t>(max_results));

    spdlog::info("[Nobody] Got {} search results", result.search_results.size());

    // ── Step 2: Scrape ────────────────────────────────────────────────────
    if (config_.enable_scraping && !result.search_results.empty()) {
        spdlog::info("[Nobody] Step 2/3: Scraping web pages...");
        auto urls = select_urls_to_scrape(result.search_results, result.intent);
        // Filter empty URLs
        urls.erase(std::remove_if(urls.begin(), urls.end(),
            [](const std::string& u){ return u.empty() || u.size() < 8; }), urls.end());

        int pages_to_scrape = get_pages_to_scrape_for_depth(result.intent);

        if (!urls.empty()) {
            // Use concurrent scraping for standard and deep modes
            if (config_.depth_mode == DepthMode::QUICK) {
                result.scraped_pages = scraper_->scrape_many(urls, pages_to_scrape);
            } else {
                result.scraped_pages = scraper_->scrape_many_concurrent(
                    urls, pages_to_scrape);
            }
        } else {
            spdlog::warn("[Nobody] No valid URLs to scrape");
        }
        spdlog::info("[Nobody] Scraped {} pages successfully",
                     result.scraped_pages.size());
    } else {
        spdlog::info("[Nobody] Step 2/3: Scraping skipped");
    }

    // ── Step 3: Synthesis ─────────────────────────────────────────────────
    spdlog::info("[Nobody] Step 3/3: AI Synthesis (mode: {})...",
                 depth_to_string(config_.depth_mode));

    SynthesisMode synth_mode = get_synthesis_mode();
    AIConfig ai_cfg = ai_->config();
    ai_cfg.synthesis_mode = synth_mode;
    ai_->set_config(ai_cfg);

    result.ai_response = ai_->synthesise(
        query, result.search_results, result.scraped_pages, history, result.intent
    );

    result.success    = result.ai_response.success;
    result.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start
    );

    spdlog::info("[Nobody] ── Pipeline complete ({} ms, {} passes) ─────────",
                 result.total_time.count(), result.ai_response.passes_used);

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

    // News/current events (dynamic year detection)
    static const std::vector<std::string> news_kw =
        {"latest", "recent", "today", "breaking", "news", "now",
         "current", "update", "happening", "announced", "released",
         "2024", "2025", "2026", "2027"};
    for (const auto& kw : news_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::CURRENT_EVENTS;

    // Person lookup
    static const std::regex person_re(
        R"(\bwho\s+is\b|\bfind\s+(?:info|information)\s+(?:on|about)\b|\b(?:ceo|founder|cto|president|director|author|creator)\s+of\b|\bbiography\b|\bprofile\b)",
        std::regex::icase);
    if (std::regex_search(query, person_re)) return QueryIntent::PERSON_LOOKUP;

    // Technical
    static const std::vector<std::string> tech_kw =
        {"how to","tutorial","code","api","library","install","configure",
         "error","exception","function","class","syntax","algorithm",
         "debug","compile","build","deploy","docker","kubernetes",
         "python","javascript","rust","golang","cpp","java"};
    for (const auto& kw : tech_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::TECHNICAL;

    // Deep research
    static const std::vector<std::string> deep_kw =
        {"explain","analyse","analyze","compare","overview","history",
         "impact","effect","cause","relationship","difference","why",
         "how does","research","study","evidence","theory","mechanism",
         "comprehensive","detailed","in-depth","pros and cons",
         "advantages","disadvantages"};
    for (const auto& kw : deep_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::DEEP_RESEARCH;

    // Simple fact
    static const std::vector<std::string> fact_kw =
        {"what is","what are","define","when was","where is","who invented",
         "who created","how many","how much","what does","meaning of",
         "definition of","when did","where did","capital of"};
    for (const auto& kw : fact_kw)
        if (q.find(kw) != std::string::npos) return QueryIntent::FACT_LOOKUP;

    return QueryIntent::UNKNOWN;
}

// ── select_urls_to_scrape ─────────────────────────────────────────────────────
std::vector<std::string> NobodyEngine::select_urls_to_scrape(
        const std::vector<SearchResult>& results,
        QueryIntent intent) const {

    int desired = get_pages_to_scrape_for_depth(intent);

    // Priority domains based on intent
    static const std::vector<std::string> authority_domains =
        {"wikipedia.org","britannica.com","nature.com","science.org",
         "arxiv.org","nih.gov","cdc.gov","who.int"};
    static const std::vector<std::string> news_domains =
        {"reuters.com","bbc.com","apnews.com","nytimes.com",
         "theguardian.com","washingtonpost.com","aljazeera.com"};
    static const std::vector<std::string> tech_domains =
        {"stackoverflow.com","github.com","developer.mozilla.org",
         "docs.python.org","cppreference.com","learn.microsoft.com"};

    const std::vector<std::string>* priority_list = &authority_domains;
    if (intent == QueryIntent::CURRENT_EVENTS) priority_list = &news_domains;
    if (intent == QueryIntent::TECHNICAL) priority_list = &tech_domains;

    std::vector<std::string> priority_urls, edu_gov_urls, other_urls;
    for (const auto& r : results) {
        bool is_priority = false;
        for (const auto& dom : *priority_list) {
            if (r.url.find(dom) != std::string::npos) { is_priority = true; break; }
        }
        if (is_priority) {
            priority_urls.push_back(r.url);
        } else if (r.url.find(".edu") != std::string::npos ||
                   r.url.find(".gov") != std::string::npos) {
            edu_gov_urls.push_back(r.url);
        } else {
            other_urls.push_back(r.url);
        }
    }

    std::vector<std::string> out;
    for (const auto& u : priority_urls) { out.push_back(u); if ((int)out.size()>=desired) break; }
    for (const auto& u : edu_gov_urls)  { out.push_back(u); if ((int)out.size()>=desired) break; }
    for (const auto& u : other_urls)    { out.push_back(u); if ((int)out.size()>=desired) break; }
    return out;
}

// ── Depth-adaptive parameters ─────────────────────────────────────────────────
int NobodyEngine::get_max_results_for_depth(QueryIntent intent) const {
    switch (config_.depth_mode) {
        case DepthMode::QUICK:    return (intent == QueryIntent::FACT_LOOKUP) ? 5 : 8;
        case DepthMode::STANDARD: return config_.max_search_results;
        case DepthMode::DEEP:     return std::max(config_.max_search_results, 20);
    }
    return config_.max_search_results;
}

int NobodyEngine::get_pages_to_scrape_for_depth(QueryIntent intent) const {
    switch (config_.depth_mode) {
        case DepthMode::QUICK:
            return (intent == QueryIntent::FACT_LOOKUP) ? 1 : 2;
        case DepthMode::STANDARD:
            return config_.pages_to_scrape;
        case DepthMode::DEEP:
            return std::max(config_.pages_to_scrape, 8);
    }
    return config_.pages_to_scrape;
}

SynthesisMode NobodyEngine::get_synthesis_mode() const {
    switch (config_.depth_mode) {
        case DepthMode::QUICK:    return SynthesisMode::QUICK;
        case DepthMode::STANDARD: return SynthesisMode::STANDARD;
        case DepthMode::DEEP:     return SynthesisMode::DEEP;
    }
    return SynthesisMode::STANDARD;
}

// ── String helpers ────────────────────────────────────────────────────────────
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

std::string NobodyEngine::depth_to_string(DepthMode d) {
    switch (d) {
        case DepthMode::QUICK:    return "QUICK";
        case DepthMode::STANDARD: return "STANDARD";
        case DepthMode::DEEP:     return "DEEP";
    }
    return "STANDARD";
}

// ── is_url ────────────────────────────────────────────────────────────────────
bool NobodyEngine::is_url(const std::string& input) {
    if (input.size() < 8) return false;
    // Check for common URL schemes
    if (input.substr(0, 7) == "http://" || input.substr(0, 8) == "https://")
        return true;
    // Check for www. prefix
    if (input.substr(0, 4) == "www." && input.find('.', 4) != std::string::npos)
        return true;
    return false;
}

// ── run_from_url ──────────────────────────────────────────────────────────────
NobodyResult NobodyEngine::run_from_url(const std::string& url,
                                         const std::vector<Message>& history) {
    auto t_start = std::chrono::steady_clock::now();

    NobodyResult result;
    result.seed_url = url;
    result.depth    = config_.depth_mode;

    // Normalise URL (add https:// if missing)
    std::string full_url = url;
    if (full_url.substr(0, 4) != "http") full_url = "https://" + full_url;

    spdlog::info("[Nobody] ── URL Research Pipeline ─────────────────────────");
    spdlog::info("[Nobody] Seed URL: {}", full_url);
    spdlog::info("[Nobody] Depth  : {}", depth_to_string(config_.depth_mode));

    // ── Step 1: Scrape the seed URL ───────────────────────────────────────
    spdlog::info("[Nobody] Step 1/4: Scraping seed URL...");
    auto seed_page = scraper_->scrape(full_url);

    if (!seed_page.success || seed_page.word_count < 10) {
        spdlog::error("[Nobody] Failed to scrape seed URL: {}", seed_page.error);
        result.ai_response.error = "Could not scrape the provided URL: " + seed_page.error;
        result.ai_response.success = false;
        result.success = false;
        result.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start);
        return result;
    }

    result.scraped_pages.push_back(seed_page);
    spdlog::info("[Nobody] Seed page: \"{}\" ({} words)", seed_page.title, seed_page.word_count);

    // ── Step 2: Extract topic and build search queries ────────────────────
    spdlog::info("[Nobody] Step 2/4: Extracting topic from seed page...");
    auto topic_queries = extract_topics_from_page(seed_page, 3);

    if (topic_queries.empty()) {
        // Fallback: use the page title as the query
        topic_queries.push_back(seed_page.title);
    }

    // Use the first extracted topic as the main query for the result
    result.query = topic_queries[0];
    result.intent = classify_intent(result.query);

    spdlog::info("[Nobody] Extracted topic: \"{}\" (intent: {})",
                 result.query, intent_to_string(result.intent));
    for (size_t i = 1; i < topic_queries.size(); ++i)
        spdlog::info("[Nobody] Additional query: \"{}\" ", topic_queries[i]);

    // ── Step 3: Search the web for related content ────────────────────────
    spdlog::info("[Nobody] Step 3/4: Searching web for related content...");
    std::vector<std::vector<SearchResult>> all_search_results;

    for (const auto& tq : topic_queries) {
        std::vector<SearchResult> sr;
        if (config_.enable_dorking) {
            sr = search_->search_with_dorking(tq, result.intent);
        } else {
            sr = search_->search(tq);
        }
        if (!sr.empty()) all_search_results.push_back(std::move(sr));
    }

    // Merge all search results (flatten + deduplicate by URL)
    std::set<std::string> seen_urls;
    seen_urls.insert(full_url); // exclude the seed URL itself
    for (const auto& engine_results : all_search_results) {
        for (const auto& sr : engine_results) {
            std::string key = sr.url;
            if (!key.empty() && key.back() == '/') key.pop_back();
            if (seen_urls.insert(key).second) {
                result.search_results.push_back(sr);
            }
        }
    }

    // Cap results
    int max_results = get_max_results_for_depth(result.intent);
    if (static_cast<int>(result.search_results.size()) > max_results)
        result.search_results.resize(static_cast<size_t>(max_results));

    spdlog::info("[Nobody] Found {} related web results", result.search_results.size());

    // ── Step 4: Scrape related pages ──────────────────────────────────────
    if (config_.enable_scraping && !result.search_results.empty()) {
        spdlog::info("[Nobody] Step 3.5/4: Scraping related pages...");
        auto urls_to_scrape = select_urls_to_scrape(result.search_results, result.intent);
        int pages = get_pages_to_scrape_for_depth(result.intent);

        std::vector<ScrapedPage> related_pages;
        if (config_.depth_mode == DepthMode::QUICK) {
            related_pages = scraper_->scrape_many(urls_to_scrape, pages);
        } else {
            related_pages = scraper_->scrape_many_concurrent(urls_to_scrape, pages);
        }

        // Add related pages after the seed page
        for (auto& p : related_pages)
            result.scraped_pages.push_back(std::move(p));

        spdlog::info("[Nobody] Scraped {} related pages", related_pages.size());
    }

    // ── Step 4: AI Synthesis ──────────────────────────────────────────────
    spdlog::info("[Nobody] Step 4/4: AI Synthesis...");
    SynthesisMode synth_mode = get_synthesis_mode();
    AIConfig ai_cfg = ai_->config();
    ai_cfg.synthesis_mode = synth_mode;
    ai_->set_config(ai_cfg);

    // Build a special query that references the seed URL
    std::string synth_query = fmt::format(
        "Based on the content from {} and related web sources, provide a comprehensive "
        "analysis and summary of the topic: {}",
        full_url, result.query);

    result.ai_response = ai_->synthesise(
        synth_query, result.search_results, result.scraped_pages, history, result.intent);

    result.success    = result.ai_response.success;
    result.total_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start);

    spdlog::info("[Nobody] ── URL Pipeline complete ({} ms, {} total pages) ────",
                 result.total_time.count(), result.scraped_pages.size());

    return result;
}

// ── extract_topics_from_page ──────────────────────────────────────────────────
std::vector<std::string> NobodyEngine::extract_topics_from_page(
        const ScrapedPage& page, int max_queries) {

    std::vector<std::string> queries;

    // Strategy 1: Use the page title (cleaned)
    std::string title = page.title;
    if (title != "(no title)" && !title.empty()) {
        // Remove common suffixes like " - Wikipedia", " | CNN"
        for (const auto& sep : {" - ", " | ", " :: ", " — ", " – "}) {
            size_t pos = title.rfind(sep);
            if (pos != std::string::npos && pos > 5)
                title = title.substr(0, pos);
        }
        queries.push_back(title);
    }

    // Strategy 2: Use meta description keywords
    if (!page.meta.description.empty() && page.meta.description.size() > 20) {
        // Extract first sentence of description
        std::string desc = page.meta.description;
        size_t period = desc.find('.');
        if (period != std::string::npos && period > 10 && period < 150)
            desc = desc.substr(0, period);
        if (desc.size() > 10 && desc != title)
            queries.push_back(desc);
    }

    // Strategy 3: TF-based keyword extraction from body text
    if (!page.text.empty()) {
        // Stopwords to exclude
        static const std::set<std::string> stopwords = {
            "the","a","an","is","are","was","were","be","been","being",
            "have","has","had","do","does","did","will","would","could",
            "should","may","might","shall","can","need","dare","ought",
            "used","to","of","in","for","on","with","at","by","from",
            "up","about","into","through","during","before","after",
            "above","below","between","under","again","further","then",
            "once","here","there","when","where","why","how","all",
            "both","each","few","more","most","other","some","such",
            "no","nor","not","only","own","same","so","than","too",
            "very","just","because","as","until","while","that","this",
            "these","those","it","its","he","she","they","we","you",
            "his","her","their","our","your","my","and","but","or",
            "if","also","which","who","what","whom","whose","new",
            "one","two","first","last","many","much","any","every",
            "said","says","like","get","got","see","take","make",
            "know","think","come","go","want","use","find","give",
            "tell","work","call","try","ask","well","back","even",
            "way","still","over","them","him","out","now","look",
            "people","year","could","been","made","after","com","www",
            "http","https","org","html","content","page","site"
        };

        // Count word frequencies
        std::map<std::string, int> word_freq;
        std::istringstream ss(page.text);
        std::string word;
        int total_words = 0;

        while (ss >> word && total_words < 2000) {
            // Clean: lowercase, remove non-alpha
            std::string clean;
            for (char c : word) {
                if (std::isalpha(static_cast<unsigned char>(c)))
                    clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (clean.size() >= 4 && stopwords.find(clean) == stopwords.end()) {
                word_freq[clean]++;
                total_words++;
            }
        }

        // Sort by frequency
        std::vector<std::pair<std::string, int>> sorted_words(
            word_freq.begin(), word_freq.end());
        std::sort(sorted_words.begin(), sorted_words.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        // Build a keyword query from top terms
        if (sorted_words.size() >= 3) {
            int take = std::min(5, static_cast<int>(sorted_words.size()));
            std::string kw_query;
            for (int i = 0; i < take; ++i) {
                if (i > 0) kw_query += " ";
                kw_query += sorted_words[static_cast<size_t>(i)].first;
            }
            // Only add if it's different from existing queries
            bool duplicate = false;
            for (const auto& q : queries)
                if (q.find(kw_query.substr(0, 10)) != std::string::npos) { duplicate = true; break; }
            if (!duplicate) queries.push_back(kw_query);
        }
    }

    // Strategy 4: Use meta keywords if available
    if (!page.meta.keywords.empty() && static_cast<int>(queries.size()) < max_queries) {
        queries.push_back(page.meta.keywords);
    }

    // Cap at max
    if (static_cast<int>(queries.size()) > max_queries)
        queries.resize(static_cast<size_t>(max_queries));

    return queries;
}

} // namespace nobody