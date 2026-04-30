#pragma once
// =============================================================================
//  scraper/WebScraper.h
//  Fetches URLs and extracts clean readable text from HTML.
//  Features: concurrent scraping, meta extraction, JSON-LD parsing,
//            Wayback Machine fallback, recursive link-follow
// =============================================================================

#include "core/HttpClient.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <map>

namespace nobody {

// ── Metadata extracted from a page ───────────────────────────────────────────
struct PageMeta {
    std::string description;         // <meta name="description">
    std::string author;              // <meta name="author">
    std::string publish_date;        // article:published_time or datePublished
    std::string keywords;            // <meta name="keywords">
    std::string og_title;            // og:title
    std::string og_description;      // og:description
    std::string og_image;            // og:image
    std::string canonical_url;       // <link rel="canonical">
};

struct ScrapedPage {
    std::string url;
    std::string title;
    std::string text;           // cleaned body text
    std::string raw_html;       // original HTML (if retained)
    std::vector<std::string> links;     // outbound hrefs
    std::vector<std::string> images;    // src attributes
    PageMeta    meta;                   // extracted metadata
    std::string structured_data;        // JSON-LD content (raw JSON string)
    int     word_count  = 0;
    bool    success     = false;
    bool    from_wayback = false;       // fetched via Wayback Machine fallback
    std::string error;
};

struct ScraperConfig {
    bool retain_html        = false;
    bool extract_links      = true;
    bool extract_images     = false;
    bool extract_meta       = true;         // extract meta tags
    bool extract_structured = true;         // extract JSON-LD
    int  max_text_length    = 12000;        // chars, to cap context sent to LLM
    int  timeout_seconds    = 20;
    bool strip_boilerplate  = true;         // remove nav/footer/ads heuristically
    int  max_depth          = 1;            // recursive link-follow depth (0=none)
    int  max_concurrent     = 4;            // thread pool size for concurrent scraping
    bool follow_links       = false;        // enable recursive link-follow
    bool use_wayback_fallback = true;       // try Wayback Machine on 403/404
    int  max_follow_links   = 3;            // max links to follow per page
};

class WebScraper {
public:
    explicit WebScraper(std::shared_ptr<HttpClient> http,
                        ScraperConfig config = {});

    // Fetch and scrape a single URL
    ScrapedPage scrape(const std::string& url);

    // Scrape multiple URLs in sequence (returns non-empty successes first)
    std::vector<ScrapedPage> scrape_many(const std::vector<std::string>& urls,
                                          int max_pages = 5);

    // Scrape multiple URLs concurrently using std::async
    std::vector<ScrapedPage> scrape_many_concurrent(
        const std::vector<std::string>& urls, int max_pages = 5);

    // Extract just the text from raw HTML (static, reusable)
    static std::string extract_text(const std::string& html,
                                     bool strip_boilerplate = true);

    static std::string extract_title(const std::string& html);
    static std::vector<std::string> extract_links(const std::string& html,
                                                    const std::string& base_url = "");

    // Meta and structured data extraction
    static PageMeta extract_meta(const std::string& html);
    static std::string extract_json_ld(const std::string& html);

private:
    std::shared_ptr<HttpClient> http_;
    ScraperConfig config_;

    // Wayback Machine fallback
    ScrapedPage scrape_with_wayback_fallback(const std::string& url);

    static std::string strip_tags(const std::string& html);
    static std::string collapse_whitespace(const std::string& text);
    static std::string remove_section(const std::string& html,
                                       const std::string& tag,
                                       const std::string& attr,
                                       const std::string& val);
};

} // namespace nobody