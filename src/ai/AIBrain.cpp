// =============================================================================
//  ai/AIBrain.cpp
// =============================================================================

#include "ai/AIBrain.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <set>

using json = nlohmann::json;

namespace nobody {

// ── Constants ─────────────────────────────────────────────────────────────────
const std::string AIBrain::OLLAMA_API_URL =
    "http://localhost:11434/api/chat";

const std::string AIBrain::SYSTEM_PROMPT = R"(You are Nobody, an AI assistant.
You answer questions by reasoning over real-time information gathered from the web.
Your answers are:
  - Factual and grounded in the provided web context
  - Well-structured with clear sections when appropriate
  - Honest about uncertainty ("Based on available sources...", "I could not verify...")
  - Concise but thorough
  - Properly attributed — mention sources by domain name when citing facts

IMPORTANT RULES:
  1. Only state facts that appear in the web context provided below.
  2. If the context does not contain enough information, say so clearly.
  3. Never hallucinate URLs, names, or statistics.
  4. If sources conflict, note the discrepancy.
  5. End your answer with a "Sources" section listing the domains you relied on.
)";

// ── Constructor ───────────────────────────────────────────────────────────────
AIBrain::AIBrain(std::shared_ptr<HttpClient> http, AIConfig config)
    : http_(std::move(http)), config_(std::move(config)) {}

// ── synthesise ────────────────────────────────────────────────────────────────
AIResponse AIBrain::synthesise(
        const std::string&              user_query,
        const std::vector<SearchResult>& search_results,
        const std::vector<ScrapedPage>&  scraped_pages,
        const std::vector<Message>&      history) {

    std::string context = build_context_block(search_results, scraped_pages);

    // Build the user message: context + question
    std::string user_msg = fmt::format(
        "## Web Context\n\n{}\n\n---\n\n## Question\n\n{}",
        context, user_query
    );

    std::vector<Message> messages = history;
    messages.push_back({"user", user_msg});

    auto response = call_api(messages, SYSTEM_PROMPT);

    // Annotate with citations
    if (response.success) {
        response.citations =
            extract_citations(response.answer, search_results, scraped_pages);
    }
    return response;
}

// ── chat ──────────────────────────────────────────────────────────────────────
AIResponse AIBrain::chat(const std::string& user_message,
                          const std::vector<Message>& history) {
    std::vector<Message> messages = history;
    messages.push_back({"user", user_message});
    return call_api(messages, SYSTEM_PROMPT);
}

// ── call_api ─────────────────────────────────────────────────────────────────
AIResponse AIBrain::call_api(const std::vector<Message>& messages,
                               const std::string& system_prompt) {
    AIResponse result;
    result.model = config_.model;

    // ── Build JSON body ────────────────────────────────────────────────────
    json body;
    body["model"]      = config_.model;
    body["stream"]     = false;

    json options;
    options["num_predict"] = config_.max_tokens;
    if (config_.temperature >= 0.0) options["temperature"] = config_.temperature;
    body["options"] = options;

    json msgs_json = json::array();
    if (!system_prompt.empty()) {
        msgs_json.push_back({{"role", "system"}, {"content", system_prompt}});
    }

    for (const auto& m : messages) {
        msgs_json.push_back({{"role", m.role}, {"content", m.content}});
    }
    body["messages"] = msgs_json;

    // ── HTTP request ───────────────────────────────────────────────────────
    // Local LLM inference can take minutes; use a long timeout.
    HttpRequest req;
    req.method          = "POST";
    req.url             = OLLAMA_API_URL;
    req.body            = body.dump();
    req.headers         = {{"content-type", "application/json"}, {"accept", "application/json"}};
    req.timeout_seconds = 300; // 5 minutes for local model inference
    req.connect_timeout = 10;
    auto resp = http_->execute(req);

    if (!resp.is_ok()) {
        // Give a helpful error if Ollama isn't running
        if (resp.status_code == 0 || resp.error.find("refused") != std::string::npos
            || resp.error.find("connect") != std::string::npos) {
            result.error = "Cannot connect to Ollama at localhost:11434. "
                           "Is Ollama running? Start it with: ollama serve";
        } else {
            result.error = fmt::format("Ollama HTTP error {}: {}", resp.status_code, resp.body);
        }
        result.success = false;
        spdlog::error("[AIBrain] {}", result.error);
        return result;
    }

    // ── Parse response ─────────────────────────────────────────────────────
    try {
        auto j = json::parse(resp.body);

        // Usage mapping for Ollama
        result.input_tokens  = j.value("prompt_eval_count", 0);
        result.output_tokens = j.value("eval_count", 0);

        if (!j.contains("message") || !j["message"].contains("content")) {
            result.error   = "Unexpected API response: missing 'message.content'";
            result.success = false;
            return result;
        }

        result.answer  = j["message"].value("content", "");
        result.success = true;

        spdlog::info("[AIBrain] Response: {} input tokens, {} output tokens",
                     result.input_tokens, result.output_tokens);
        spdlog::debug("[AIBrain] Answer preview: {}",
                      result.answer.substr(0, std::min<size_t>(200, result.answer.size())));

    } catch (const std::exception& e) {
        result.error   = fmt::format("JSON parse error: {}", e.what());
        result.success = false;
        spdlog::error("[AIBrain] {}", result.error);
    }

    return result;
}

// ── build_context_block ───────────────────────────────────────────────────────
std::string AIBrain::build_context_block(
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>&  pages) const {

    std::ostringstream ctx;
    int total_chars = 0;

    // ── Search result snippets (fast, always included) ────────────────────
    if (!results.empty()) {
        ctx << "### Search Results\n\n";
        int i = 1;
        for (const auto& r : results) {
            if (total_chars > config_.max_context_chars / 2) break;
            std::string entry = fmt::format(
                "{}. **{}**\n   URL: {}\n   {}\n\n", i++, r.title, r.url, r.snippet);
            ctx << entry;
            total_chars += static_cast<int>(entry.size());
        }
    }

    // ── Full scraped page content ─────────────────────────────────────────
    if (!pages.empty()) {
        ctx << "\n### Full Page Content\n\n";
        for (const auto& p : pages) {
            if (!p.success || p.text.empty()) continue;
            if (total_chars >= config_.max_context_chars) break;

            int remaining = config_.max_context_chars - total_chars;
            std::string text = p.text;
            if (static_cast<int>(text.size()) > remaining)
                text = text.substr(0, static_cast<size_t>(remaining)) + "\n[truncated]";

            std::string entry = fmt::format(
                "#### {} ({})\n\n{}\n\n---\n\n", p.title, p.url, text);
            ctx << entry;
            total_chars += static_cast<int>(entry.size());
        }
    }

    if (total_chars == 0) ctx << "_No web context available._\n";
    return ctx.str();
}

// ── extract_citations ─────────────────────────────────────────────────────────
std::vector<Citation> AIBrain::extract_citations(
        const std::string& answer_text,
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>&  pages) {

    std::vector<Citation> citations;

    // Check which source URLs appear (by domain) in the answer text
    auto domain_of = [](const std::string& url) -> std::string {
        size_t after_scheme = url.find("://");
        if (after_scheme == std::string::npos) return url;
        std::string rest = url.substr(after_scheme + 3);
        size_t slash = rest.find('/');
        return slash != std::string::npos ? rest.substr(0, slash) : rest;
    };

    auto ci_contains = [](const std::string& haystack, const std::string& needle) {
        return std::search(haystack.begin(), haystack.end(),
                           needle.begin(), needle.end(),
                           [](char a, char b){ return std::tolower(a)==std::tolower(b); })
               != haystack.end();
    };

    std::set<std::string> seen;
    for (const auto& r : results) {
        std::string dom = domain_of(r.url);
        if (seen.count(dom)) continue;
        if (ci_contains(answer_text, dom)) {
            citations.push_back({r.url, r.title, r.snippet});
            seen.insert(dom);
        }
    }
    for (const auto& p : pages) {
        std::string dom = domain_of(p.url);
        if (seen.count(dom)) continue;
        if (ci_contains(answer_text, dom)) {
            citations.push_back({p.url, p.title, ""});
            seen.insert(dom);
        }
    }
    return citations;
}

} // namespace nobody