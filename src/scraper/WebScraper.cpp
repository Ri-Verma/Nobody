// =============================================================================
//  scraper/WebScraper.cpp
// =============================================================================

#include "scraper/WebScraper.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <regex>
#include <algorithm>
#include <sstream>
#include <cctype>

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
    if (!resp.is_ok()) {
        page.success = false;
        page.error   = fmt::format("HTTP {}: {}", resp.status_code, resp.error);
        spdlog::warn("[Scraper] Failed to fetch {}: {}", url, page.error);
        return page;
    }

    // Only process HTML
    const std::string& html = resp.body;
    if (config_.retain_html) page.raw_html = html;

    page.title = extract_title(html);
    page.text  = extract_text(html, config_.strip_boilerplate);

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
    spdlog::info("[Scraper] {} — \"{}\" ({} words)", url, page.title, page.word_count);
    return page;
}

// ── scrape_many ───────────────────────────────────────────────────────────────
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

// ── remove_section ────────────────────────────────────────────────────────────
// Removes <tag ... attr="val" ...>...</tag> blocks (handles nesting via simple
// bracket counting — good enough for boilerplate removal).
std::string WebScraper::remove_section(const std::string& html,
                                        const std::string& tag,
                                        const std::string& attr,
                                        const std::string& val) {
    std::string out;
    out.reserve(html.size());
    size_t pos = 0;

    // Pattern: <tag ...attr="val"...
    std::string open_pat = "<" + tag;

    while (pos < html.size()) {
        size_t found = html.find(open_pat, pos);
        if (found == std::string::npos) { out.append(html, pos, html.size()-pos); break; }

        // Check if this open tag contains the attribute
        size_t tag_end = html.find('>', found);
        if (tag_end == std::string::npos) { out.append(html, pos, html.size()-pos); break; }

        std::string opening = html.substr(found, tag_end - found + 1);
        if (opening.find(attr) == std::string::npos ||
            opening.find(val)  == std::string::npos) {
            out.append(html, pos, found - pos + 1);
            pos = found + 1;
            continue;
        }

        // Skip everything until matching close tag (simple depth counter)
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
        // ── Safe iterative tag-pair stripper ──────────────────────────────────
        // NOTE: GCC std::regex with [\s\S]*? causes exponential backtracking
        // and stack overflow (SIGSEGV) on large HTML pages. We use linear
        // string search instead.
        auto remove_tag_blocks = [](std::string& s, const std::string& tag) {
            std::string open  = "<" + tag;
            std::string close = "</" + tag + ">";
            size_t pos = 0;
            while (pos < s.size()) {
                // Find next occurrence of opening tag
                size_t found = s.find(open, pos);
                if (found == std::string::npos) break;
                // Verify next char after tag name is '>' or space (not e.g. <header matching <head>)
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

        for (const std::string& tag : {"script","style","noscript","head","iframe","svg"})
            remove_tag_blocks(h, tag);

        // ── Extract main content via simple find (no regex) ───────────────────
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

        std::string extracted;
        extracted = extract_between(h, "<main",    "</main>");
        if (extracted.empty()) extracted = extract_between(h, "<article", "</article>");
        if (extracted.empty()) extracted = extract_between(h, "<body",    "</body>");
        if (!extracted.empty()) h = extracted;

        for (const std::string& tag : {"nav","footer","header","aside"})
            remove_tag_blocks(h, tag);
    }

    // Strip all remaining HTML tags
    h = strip_tags(h);

    // Decode common HTML entities
    struct Entity { const char* enc; const char* dec; };
    static const Entity entities[] = {
        {"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},
        {"&apos;","'"},{"&nbsp;"," "},{"&#39;","'"},{"&mdash;","—"},
        {"&ndash;","–"},{"&hellip;","…"},
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
    std::regex re(R"(<title[^>]*>(.*?)</title>)",
                  std::regex::icase | std::regex::ECMAScript);
    std::smatch m;
    if (std::regex_search(html, m, re)) {
        std::string title = m[1].str();
        // Strip tags inside title (rare but possible)
        title = strip_tags(title);
        return collapse_whitespace(title);
    }
    return "(no title)";
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
    // Replace block-level tags with newlines for readability
    std::string h = html;
    std::regex block(R"(</?(?:p|div|h[1-6]|li|tr|br|hr|blockquote|pre)[^>]*>)",
                     std::regex::icase);
    h = std::regex_replace(h, block, "\n");
    // Remove all remaining tags
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

    // Trim leading/trailing
    size_t start = out.find_first_not_of(" \n");
    size_t end   = out.find_last_not_of(" \n");
    if (start == std::string::npos) return "";
    return out.substr(start, end - start + 1);
}

} // namespace nobody