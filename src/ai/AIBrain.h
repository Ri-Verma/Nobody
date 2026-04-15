#pragma once
// =============================================================================
//  ai/AIBrain.h
//  Sends gathered context (search snippets + scraped pages) to the
//  Anthropic Claude API and returns a structured, cited answer.
// =============================================================================

#include "core/HttpClient.h"
#include "search/SearchEngine.h"
#include "scraper/WebScraper.h"

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace osint {

// ── A source citation ─────────────────────────────────────────────────────────
struct Citation {
    std::string url;
    std::string title;
    std::string excerpt;
};

// ── The AI's synthesised response ─────────────────────────────────────────────
struct AIResponse {
    std::string answer;              // main answer text
    std::string reasoning;           // chain-of-thought (if requested)
    std::vector<Citation> citations; // sources used
    std::string model;               // which Claude model responded
    int input_tokens  = 0;
    int output_tokens = 0;
    bool success      = false;
    std::string error;
};

// ── Message in a conversation ────────────────────────────────────────────────
struct Message {
    std::string role;     // "user" | "assistant"
    std::string content;
};

// ── AIBrain config ────────────────────────────────────────────────────────────
struct AIConfig {
    std::string api_key;
    std::string model          = "claude-sonnet-4-20250514";
    int         max_tokens     = 2048;
    double      temperature    = 0.3;   // low = more factual
    bool        stream         = false;
    int         max_context_chars = 12000; // cap to keep prompts from exploding
};

// ── AIBrain ───────────────────────────────────────────────────────────────────
class AIBrain {
public:
    explicit AIBrain(std::shared_ptr<HttpClient> http, AIConfig config);

    // One-shot: synthesise an answer given search results + scraped pages
    AIResponse synthesise(
        const std::string&            user_query,
        const std::vector<SearchResult>&  search_results,
        const std::vector<ScrapedPage>&   scraped_pages,
        const std::vector<Message>&   history = {}
    );

    // Simple chat (no web context) — for follow-up questions
    AIResponse chat(const std::string& user_message,
                    const std::vector<Message>& history = {});

    // Raw API call
    AIResponse call_api(const std::vector<Message>& messages,
                        const std::string& system_prompt = "");

    void set_config(const AIConfig& cfg) { config_ = cfg; }
    const AIConfig& config() const { return config_; }

private:
    std::shared_ptr<HttpClient> http_;
    AIConfig config_;

    static const std::string ANTHROPIC_API_URL;
    static const std::string SYSTEM_PROMPT;

    std::string build_context_block(
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>&  pages) const;

    static std::vector<Citation> extract_citations(
        const std::string& answer_text,
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>&  pages);
};

} // namespace osint