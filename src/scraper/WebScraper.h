#pragma once
// =============================================================================
//  scraper/WebScraper.h
//  Fetches a URL and extracts clean readable text from the HTML.
//  No external parser needed: uses regex + heuristics.
// =============================================================================

#include "core/HttpClient.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace nobody {

struct ScrapedPage {
    std::string url;
    std::string title;
    std::string text;           // cleaned body text
    std::string raw_html;       // original HTML (if retained)
    std::vector<std::string> links;     // outbound hrefs
    std::vector<std::string> images;    // src attributes
    int     word_count  = 0;
    bool    success     = false;
    std::string error;
};

struct ScraperConfig {
    bool retain_html        = false;
    bool extract_links      = true;
    bool extract_images     = false;
    int  max_text_length    = 8000;   // chars, to cap context sent to LLM
    int  timeout_seconds    = 20;
    bool strip_boilerplate  = true;   // remove nav/footer/ads heuristically
};

class WebScraper {
public:
    explicit WebScraper(std::shared_ptr<HttpClient> http,
                        ScraperConfig config = {});

    // Fetch and scrape a single URL
    ScrapedPage scrape(const std::string& url);

    // Scrape multiple URLs in sequence (returns non-empty successes first)
    std::vector<ScrapedPage> scrape_many(const std::vector<std::string>& urls,
                                          int max_pages = 3);

    // Extract just the text from raw HTML (static, reusable)
    static std::string extract_text(const std::string& html,
                                     bool strip_boilerplate = true);

    static std::string extract_title(const std::string& html);
    static std::vector<std::string> extract_links(const std::string& html,
                                                    const std::string& base_url = "");

private:
    std::shared_ptr<HttpClient> http_;
    ScraperConfig config_;

    static std::string strip_tags(const std::string& html);
    static std::string collapse_whitespace(const std::string& text);
    static std::string remove_section(const std::string& html,
                                       const std::string& tag,
                                       const std::string& attr,
                                       const std::string& val);
};

} // namespace nobody