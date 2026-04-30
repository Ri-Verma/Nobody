// =============================================================================
//  scraper/WebScraper.cpp — Concurrent scraping, meta extraction, JSON-LD,
//                            Wayback fallback
// =============================================================================

#include "scraper/WebScraper.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <regex>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <future>
#include <mutex>

namespace nobody {

WebScraper::WebScraper(std::shared_ptr<HttpClient> http, ScraperConfig config)
    : http_(std::move(http)), config_(std::move(config)) {}

// ── scrape ────────────────────────────────────────────────────────────────────
ScrapedPage WebScraper::scrape(const std::string& url) {
    ScrapedPage page;
    page.url = url;

    spdlog::debug("[Scraper] Fetching: {}", url);
    HttpRequest req;
    req.url             = url;
    req.timeout_seconds = config_.timeout_seconds;
    req.headers         = {
        {"Accept",          "text/html,application/xhtml+xml,*/*;q=0.8"},
        {"Accept-Language", "en-US,en;q=0.9"},
    };

    auto resp = http_->execute(req);

    // Wayback fallback on failure
    if (!resp.is_ok() && config_.use_wayback_fallback) {
        spdlog::debug("[Scraper] Trying Wayback fallback for: {}", url);
        std::string wb_url = "https://web.archive.org/web/2024/" + url;
        req.url = wb_url;
        req.max_retries = 1;
        resp = http_->execute(req);
        if (resp.is_ok()) {
            page.from_wayback = true;
            spdlog::info("[Scraper] Wayback fallback successful for: {}", url);
        }
    }

    if (!resp.is_ok()) {
        page.success = false;
        page.error   = fmt::format("HTTP {}: {}", resp.status_code, resp.error);
        spdlog::warn("[Scraper] Failed to fetch {}: {}", url, page.error);
        return page;
    }

    const std::string& html = resp.body;
    if (config_.retain_html) page.raw_html = html;

    page.title = extract_title(html);
    page.text  = extract_text(html, config_.strip_boilerplate);

    // Extract metadata
    if (config_.extract_meta)
        page.meta = extract_meta(html);

    // Extract JSON-LD structured data
    if (config_.extract_structured)
        page.structured_data = extract_json_ld(html);

    // Trim to max length
    if (static_cast<int>(page.text.size()) > config_.max_text_length)
        page.text = page.text.substr(0, static_cast<size_t>(config_.max_text_length))
                  + "\n[...content truncated...]";

    // Count words
    std::istringstream ss(page.text);
    std::string word;
    while (ss >> word) ++page.word_count;

    if (config_.extract_links)
        page.links = extract_links(html, url);

    page.success = true;
    spdlog::info("[Scraper] {} — \"{}\" ({} words{})", url, page.title,
                 page.word_count, page.from_wayback ? ", via Wayback" : "");
    return page;
}

// ── scrape_many (sequential, backward compat) ─────────────────────────────────
std::vector<ScrapedPage> WebScraper::scrape_many(
        const std::vector<std::string>& urls, int max_pages) {
    std::vector<ScrapedPage> results;
    int fetched = 0;
    for (const auto& url : urls) {
        if (fetched >= max_pages) break;
        auto page = scrape(url);
        if (page.success && page.word_count > 30) {
            results.push_back(std::move(page));
            ++fetched;
        }
    }
    return results;
}

// ── scrape_many_concurrent ────────────────────────────────────────────────────
std::vector<ScrapedPage> WebScraper::scrape_many_concurrent(
        const std::vector<std::string>& urls, int max_pages) {

    int batch_size = std::min(config_.max_concurrent, static_cast<int>(urls.size()));
    batch_size = std::min(batch_size, max_pages);

    // Each async task needs its own HttpClient since curl handles aren't thread-safe
    std::vector<std::future<ScrapedPage>> futures;
    std::mutex result_mutex;
    std::vector<ScrapedPage> results;

    // Process in batches
    for (size_t i = 0; i < urls.size() && static_cast<int>(results.size()) < max_pages; ) {
        futures.clear();
        int batch = std::min(batch_size,
                             static_cast<int>(urls.size() - i));

        for (int j = 0; j < batch && (i + j) < urls.size(); ++j) {
            const std::string& url = urls[i + j];
            futures.push_back(std::async(std::launch::async, [this, &url]() {
                // Create a temporary HttpClient for this thread
                auto thread_http = std::make_shared<HttpClient>();
                WebScraper thread_scraper(thread_http, config_);
                return thread_scraper.scrape(url);
            }));
        }

        for (auto& fut : futures) {
            try {
                auto page = fut.get();
                if (page.success && page.word_count > 30) {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    if (static_cast<int>(results.size()) < max_pages)
                        results.push_back(std::move(page));
                }
            } catch (const std::exception& e) {
                spdlog::warn("[Scraper] Async scrape failed: {}", e.what());
            }
        }

        i += static_cast<size_t>(batch);
    }

    spdlog::info("[Scraper] Concurrent scrape: {} pages successful", results.size());
    return results;
}

// ── extract_meta ──────────────────────────────────────────────────────────────
PageMeta WebScraper::extract_meta(const std::string& html) {
    PageMeta meta;

    auto extract_meta_content = [&](const std::string& h,
                                     const std::string& attr,
                                     const std::string& val) -> std::string {
        // Find <meta name="attr" content="..."> or <meta property="attr" content="...">
        std::string lower_h = h;
        std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), ::tolower);

        std::string search_name = attr + "=\"" + val + "\"";
        size_t pos = lower_h.find(search_name);
        if (pos == std::string::npos) {
            search_name = attr + "='" + val + "'";
            pos = lower_h.find(search_name);
        }
        if (pos == std::string::npos) return "";

        // Find the content attribute nearby
        size_t tag_start = lower_h.rfind('<', pos);
        size_t tag_end = lower_h.find('>', pos);
        if (tag_start == std::string::npos || tag_end == std::string::npos) return "";

        std::string tag = h.substr(tag_start, tag_end - tag_start + 1);
        std::string tag_lower = lower_h.substr(tag_start, tag_end - tag_start + 1);

        size_t content_pos = tag_lower.find("content=\"");
        if (content_pos == std::string::npos) {
            content_pos = tag_lower.find("content='");
            if (content_pos == std::string::npos) return "";
            content_pos += 9;
            size_t end_pos = tag.find('\'', content_pos);
            return (end_pos != std::string::npos) ? tag.substr(content_pos, end_pos - content_pos) : "";
        }
        content_pos += 9;
        size_t end_pos = tag.find('"', content_pos);
        return (end_pos != std::string::npos) ? tag.substr(content_pos, end_pos - content_pos) : "";
    };

    meta.description    = extract_meta_content(html, "name",     "description");
    meta.author         = extract_meta_content(html, "name",     "author");
    meta.keywords       = extract_meta_content(html, "name",     "keywords");
    meta.og_title       = extract_meta_content(html, "property", "og:title");
    meta.og_description = extract_meta_content(html, "property", "og:description");
    meta.og_image       = extract_meta_content(html, "property", "og:image");
    meta.publish_date   = extract_meta_content(html, "property", "article:published_time");

    if (meta.publish_date.empty())
        meta.publish_date = extract_meta_content(html, "name", "date");
    if (meta.publish_date.empty())
        meta.publish_date = extract_meta_content(html, "property", "article:modified_time");

    // Canonical URL
    std::string lower_html = html;
    std::transform(lower_html.begin(), lower_html.end(), lower_html.begin(), ::tolower);
    size_t canon_pos = lower_html.find("rel=\"canonical\"");
    if (canon_pos != std::string::npos) {
        size_t tag_start = lower_html.rfind('<', canon_pos);
        size_t tag_end = lower_html.find('>', canon_pos);
        if (tag_start != std::string::npos && tag_end != std::string::npos) {
            std::string tag = html.substr(tag_start, tag_end - tag_start + 1);
            size_t href_pos = tag.find("href=\"");
            if (href_pos != std::string::npos) {
                href_pos += 6;
                size_t end_pos = tag.find('"', href_pos);
                if (end_pos != std::string::npos)
                    meta.canonical_url = tag.substr(href_pos, end_pos - href_pos);
            }
        }
    }

    return meta;
}

// ── extract_json_ld ───────────────────────────────────────────────────────────
std::string WebScraper::extract_json_ld(const std::string& html) {
    std::string result;
    std::string lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::string open_tag = "<script type=\"application/ld+json\"";
    std::string close_tag = "</script>";
    size_t pos = 0;

    while (pos < lower.size()) {
        size_t start = lower.find(open_tag, pos);
        if (start == std::string::npos) break;

        size_t gt = html.find('>', start);
        if (gt == std::string::npos) break;
        size_t content_start = gt + 1;

        size_t end = lower.find(close_tag, content_start);
        if (end == std::string::npos) break;

        std::string json_content = html.substr(content_start, end - content_start);
        // Trim whitespace
        size_t first = json_content.find_first_not_of(" \t\r\n");
        size_t last = json_content.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && last != std::string::npos) {
            json_content = json_content.substr(first, last - first + 1);
            if (!result.empty()) result += "\n---\n";
            result += json_content;
        }

        pos = end + close_tag.size();
    }

    return result;
}

// ── remove_section ────────────────────────────────────────────────────────────
std::string WebScraper::remove_section(const std::string& html,
                                        const std::string& tag,
                                        const std::string& attr,
                                        const std::string& val) {
    std::string out;
    out.reserve(html.size());
    size_t pos = 0;

    std::string open_pat = "<" + tag;

    while (pos < html.size()) {
        size_t found = html.find(open_pat, pos);
        if (found == std::string::npos) { out.append(html, pos, html.size()-pos); break; }

        size_t tag_end = html.find('>', found);
        if (tag_end == std::string::npos) { out.append(html, pos, html.size()-pos); break; }

        std::string opening = html.substr(found, tag_end - found + 1);
        if (opening.find(attr) == std::string::npos ||
            opening.find(val)  == std::string::npos) {
            out.append(html, pos, found - pos + 1);
            pos = found + 1;
            continue;
        }

        out.append(html, pos, found - pos);
        int depth  = 1;
        size_t cur = tag_end + 1;
        std::string close_tag = "</" + tag;
        while (depth > 0 && cur < html.size()) {
            size_t next_open  = html.find("<"  + tag, cur);
            size_t next_close = html.find(close_tag,  cur);
            if (next_close == std::string::npos) break;
            if (next_open != std::string::npos && next_open < next_close) {
                ++depth;
                cur = next_open + tag.size() + 1;
            } else {
                --depth;
                cur = next_close + close_tag.size();
                size_t gt = html.find('>', cur);
                if (gt != std::string::npos) cur = gt + 1;
            }
        }
        pos = cur;
    }
    return out;
}

// ── extract_text ──────────────────────────────────────────────────────────────
std::string WebScraper::extract_text(const std::string& html,
                                      bool strip_boilerplate) {
    std::string h = html;

    if (strip_boilerplate) {
        auto remove_tag_blocks = [](std::string& s, const std::string& tag) {
            std::string open  = "<" + tag;
            std::string close = "</" + tag + ">";
            size_t pos = 0;
            while (pos < s.size()) {
                size_t found = s.find(open, pos);
                if (found == std::string::npos) break;
                if (found + open.size() < s.size()) {
                    char next = s[found + open.size()];
                    if (next != '>' && next != ' ' && next != '\t' && next != '\n' && next != '\r') {
                        pos = found + 1; continue;
                    }
                }
                size_t close_pos = s.find(close, found);
                if (close_pos == std::string::npos) break;
                s.erase(found, close_pos + close.size() - found);
                pos = found;
            }
        };

        for (const auto& tag : std::vector<std::string>{"script","style","noscript","head","iframe","svg"})
            remove_tag_blocks(h, tag);

        auto extract_between = [](const std::string& src,
                                  const std::string& open_tag,
                                  const std::string& close_tag) -> std::string {
            std::string s_low = src;
            std::transform(s_low.begin(), s_low.end(), s_low.begin(), ::tolower);
            size_t start = s_low.find(open_tag);
            if (start == std::string::npos) return "";
            size_t gt = src.find('>', start);
            if (gt == std::string::npos) return "";
            size_t content_start = gt + 1;
            size_t end = s_low.find(close_tag, content_start);
            if (end == std::string::npos) return "";
            return src.substr(content_start, end - content_start);
        };

        // Try multiple content selectors for better extraction
        std::string extracted;
        extracted = extract_between(h, "<main",    "</main>");
        if (extracted.empty()) extracted = extract_between(h, "<article", "</article>");
        // Common content div IDs
        if (extracted.empty()) {
            std::string h_low = h;
            std::transform(h_low.begin(), h_low.end(), h_low.begin(), ::tolower);
            // Wikipedia content
            if (h_low.find("id=\"mw-content-text\"") != std::string::npos)
                extracted = extract_between(h, "id=\"mw-content-text\"", "</div>");
            // Generic content divs
            if (extracted.empty() && h_low.find("id=\"content\"") != std::string::npos)
                extracted = extract_between(h, "id=\"content\"", "</div>");
            if (extracted.empty() && h_low.find("class=\"post-content\"") != std::string::npos)
                extracted = extract_between(h, "class=\"post-content\"", "</div>");
            if (extracted.empty() && h_low.find("class=\"entry-content\"") != std::string::npos)
                extracted = extract_between(h, "class=\"entry-content\"", "</div>");
        }
        if (extracted.empty()) extracted = extract_between(h, "<body",    "</body>");
        if (!extracted.empty()) h = extracted;

        for (const auto& tag : std::vector<std::string>{"nav","footer","header","aside","sidebar"})
            remove_tag_blocks(h, tag);
    }

    // Strip all remaining HTML tags
    h = strip_tags(h);

    // Decode common HTML entities
    struct Entity { const char* enc; const char* dec; };
    static const Entity entities[] = {
        {"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},
        {"&apos;","'"},{"&nbsp;"," "},{"&#39;","'"},{"&mdash;","—"},
        {"&ndash;","–"},{"&hellip;","…"},{"&#x27;","'"},{"&#34;","\""},
        {"&laquo;","«"},{"&raquo;","»"},{"&bull;","•"},{"&copy;","©"},
    };
    for (const auto& e : entities) {
        size_t p = 0;
        while ((p = h.find(e.enc, p)) != std::string::npos) {
            h.replace(p, strlen(e.enc), e.dec);
            p += strlen(e.dec);
        }
    }

    return collapse_whitespace(h);
}

// ── extract_title ─────────────────────────────────────────────────────────────
std::string WebScraper::extract_title(const std::string& html) {
    // Safe linear search instead of regex for title
    std::string lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    size_t start = lower.find("<title");
    if (start == std::string::npos) return "(no title)";
    size_t gt = lower.find('>', start);
    if (gt == std::string::npos) return "(no title)";
    size_t content_start = gt + 1;
    size_t end = lower.find("</title>", content_start);
    if (end == std::string::npos) return "(no title)";

    std::string title = html.substr(content_start, end - content_start);
    title = strip_tags(title);
    return collapse_whitespace(title);
}

// ── extract_links ─────────────────────────────────────────────────────────────
std::vector<std::string> WebScraper::extract_links(const std::string& html,
                                                    const std::string& /*base_url*/) {
    std::vector<std::string> links;
    std::regex re("<a[^>]+href=\"([^\"#][^\"]*)\"",
                  std::regex::icase | std::regex::ECMAScript);
    auto it  = std::sregex_iterator(html.begin(), html.end(), re);
    auto end = std::sregex_iterator{};
    for (; it != end; ++it) {
        std::string href = (*it)[1].str();
        if (href.substr(0,4) == "http") links.push_back(href);
    }
    return links;
}

// ── strip_tags ────────────────────────────────────────────────────────────────
std::string WebScraper::strip_tags(const std::string& html) {
    std::string h = html;
    std::regex block(R"(</?(?:p|div|h[1-6]|li|tr|br|hr|blockquote|pre)[^>]*>)",
                     std::regex::icase);
    h = std::regex_replace(h, block, "\n");
    std::regex any_tag("<[^>]+>");
    return std::regex_replace(h, any_tag, "");
}

// ── collapse_whitespace ───────────────────────────────────────────────────────
std::string WebScraper::collapse_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_newline = false;
    bool last_space   = false;

    for (char c : text) {
        if (c == '\n' || c == '\r') {
            if (!last_newline) { out += '\n'; last_newline = true; last_space = false; }
        } else if (c == ' ' || c == '\t') {
            if (!last_space && !last_newline) { out += ' '; last_space = true; }
        } else {
            out += c;
            last_newline = last_space = false;
        }
    }

    size_t start = out.find_first_not_of(" \n");
    size_t end   = out.find_last_not_of(" \n");
    if (start == std::string::npos) return "";
    return out.substr(start, end - start + 1);
}

} // namespace nobody