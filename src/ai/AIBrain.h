#pragma once
// =============================================================================
//  ai/AIBrain.h
//  Sends gathered context (search snippets + scraped pages) to the
//  Ollama Local API and returns a structured, cited answer.
//  Features: multi-pass map-reduce synthesis, fact extraction,
//            intent-specific prompts, conflict detection
// =============================================================================

#include "core/HttpClient.h"
#include "search/SearchEngine.h"
#include "scraper/WebScraper.h"

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace nobody {

// ── Synthesis mode ────────────────────────────────────────────────────────────
enum class SynthesisMode {
    QUICK,      // snippets only, single pass, fast
    STANDARD,   // full scrape + single pass synthesis
    DEEP        // full scrape + map-reduce multi-pass synthesis
};

// ── A source citation ─────────────────────────────────────────────────────────
struct Citation {
    std::string url;
    std::string title;
    std::string excerpt;
};

// ── Extracted fact from a source ──────────────────────────────────────────────
struct ExtractedFact {
    std::string fact;           // the factual statement
    std::string source_url;     // where it came from
    std::string source_domain;  // domain name
    double      confidence = 1.0;  // how many sources corroborate
};

// ── The AI's synthesised response ─────────────────────────────────────────────
struct AIResponse {
    std::string answer;              // main answer text
    std::string reasoning;           // chain-of-thought (if requested)
    std::vector<Citation> citations; // sources used
    std::vector<ExtractedFact> key_facts;  // extracted facts for verification
    std::string model;               // which model responded
    SynthesisMode mode_used = SynthesisMode::STANDARD;
    int input_tokens  = 0;
    int output_tokens = 0;
    bool success      = false;
    std::string error;
    int  passes_used  = 1;           // for map-reduce, how many LLM calls
};

// ── Message in a conversation ────────────────────────────────────────────────
struct Message {
    std::string role;     // "user" | "assistant" | "system"
    std::string content;
};

// ── AIBrain config ────────────────────────────────────────────────────────────
struct AIConfig {
    std::string api_key;
    std::string model          = "llama3.1:latest";
    int         max_tokens     = 2500;
    double      temperature    = 0.2;   // low = more factual
    bool        stream         = false;
    int         max_context_chars = 24000; // cap to keep prompts from exploding
    SynthesisMode synthesis_mode = SynthesisMode::STANDARD;
};

// Forward declaration
enum class QueryIntent;

// ── AIBrain ───────────────────────────────────────────────────────────────────
class AIBrain {
public:
    explicit AIBrain(std::shared_ptr<HttpClient> http, AIConfig config);

    // One-shot: synthesise an answer given search results + scraped pages
    AIResponse synthesise(
        const std::string&            user_query,
        const std::vector<SearchResult>&  search_results,
        const std::vector<ScrapedPage>&   scraped_pages,
        const std::vector<Message>&   history = {},
        QueryIntent intent = static_cast<QueryIntent>(5) // UNKNOWN
    );

    // Deep synthesis: map-reduce multi-pass for large context
    AIResponse synthesise_deep(
        const std::string&            user_query,
        const std::vector<SearchResult>&  search_results,
        const std::vector<ScrapedPage>&   scraped_pages,
        const std::vector<Message>&   history = {},
        QueryIntent intent = static_cast<QueryIntent>(5)
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

    static const std::string OLLAMA_API_URL;

    // System prompts per intent
    static std::string get_system_prompt(QueryIntent intent);
    static const std::string BASE_SYSTEM_PROMPT;

    // Context building
    std::string build_context_block(
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>&  pages) const;

    // Fact extraction from scraped content
    std::vector<ExtractedFact> extract_key_facts(
        const std::vector<ScrapedPage>& pages,
        const std::string& query) const;

    // Map phase: summarise each page individually
    std::vector<std::string> map_summarise(
        const std::string& query,
        const std::vector<ScrapedPage>& pages);

    // Reduce phase: combine summaries into final answer
    AIResponse reduce_synthesise(
        const std::string& query,
        const std::vector<std::string>& summaries,
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>& pages,
        const std::vector<Message>& history,
        QueryIntent intent);

    static std::vector<Citation> extract_citations(
        const std::string& answer_text,
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>&  pages);
};

} // namespace nobody