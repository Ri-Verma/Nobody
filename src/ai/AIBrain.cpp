// =============================================================================
//  ai/AIBrain.cpp — Multi-pass synthesis, fact extraction, intent prompts
// =============================================================================

#include "ai/AIBrain.h"
#include "engine/IntelligenceEngine.h"

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

const std::string AIBrain::BASE_SYSTEM_PROMPT = R"(You are Nobody, an AI assistant.
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
  4. If sources conflict, note the discrepancy and present both viewpoints.
  5. End your answer with a "Sources" section listing the domains you relied on.
  6. When metadata (author, publish date) is available, factor recency and authority into your response.
  7. Prioritise information from authoritative sources (.edu, .gov, wikipedia, nature.com) over less reliable ones.
)";

// ── Intent-specific prompts ───────────────────────────────────────────────────
std::string AIBrain::get_system_prompt(QueryIntent intent) {
    std::string extra;
    switch (intent) {
        case QueryIntent::FACT_LOOKUP:
            extra = R"(
This is a factual lookup. Provide a direct, concise answer first, then elaborate.
Start with the key fact/answer in the first sentence. Keep the response focused.)";
            break;
        case QueryIntent::DEEP_RESEARCH:
            extra = R"(
This is a deep research query. Provide a comprehensive, well-structured analysis.
Use headers (##) to organize sections. Cover multiple perspectives and include
relevant data, statistics, and expert opinions from the sources.
Be thorough — this is a research-grade response.)";
            break;
        case QueryIntent::PERSON_LOOKUP:
            extra = R"(
This is a person/entity lookup. Structure your response as an intelligence briefing:
- **Identity**: Full name, aliases, key identifiers
- **Background**: Education, career history, key roles
- **Current Status**: Current position, recent activities
- **Notable**: Key achievements, controversies, or relevant facts
- **Connections**: Organisations, affiliations, known associates)";
            break;
        case QueryIntent::CURRENT_EVENTS:
            extra = R"(
This is a current events query. Prioritise the most recent information.
Lead with the latest developments. Include dates and timeline when possible.
Flag if any information might be outdated. Cross-reference multiple news sources.)";
            break;
        case QueryIntent::TECHNICAL:
            extra = R"(
This is a technical query. Provide precise, actionable information.
Include code examples, exact commands, or configuration snippets when relevant.
Reference official documentation. Mention version numbers and compatibility notes.)";
            break;
        default:
            break;
    }
    return BASE_SYSTEM_PROMPT + extra;
}

// ── Constructor ───────────────────────────────────────────────────────────────
AIBrain::AIBrain(std::shared_ptr<HttpClient> http, AIConfig config)
    : http_(std::move(http)), config_(std::move(config)) {}

// ── synthesise ────────────────────────────────────────────────────────────────
AIResponse AIBrain::synthesise(
        const std::string&              user_query,
        const std::vector<SearchResult>& search_results,
        const std::vector<ScrapedPage>&  scraped_pages,
        const std::vector<Message>&      history,
        QueryIntent intent) {

    // Decide synthesis mode
    SynthesisMode mode = config_.synthesis_mode;
    if (mode == SynthesisMode::DEEP || scraped_pages.size() > 5) {
        return synthesise_deep(user_query, search_results, scraped_pages, history, intent);
    }

    std::string context = build_context_block(search_results, scraped_pages);

    // Extract key facts for the response
    auto facts = extract_key_facts(scraped_pages, user_query);

    // Build fact summary if available
    std::string fact_section;
    if (!facts.empty()) {
        fact_section = "\n### Key Facts Extracted\n";
        for (size_t i = 0; i < facts.size() && i < 10; ++i) {
            fact_section += fmt::format("- {} (source: {})\n",
                                        facts[i].fact, facts[i].source_domain);
        }
    }

    std::string user_msg = fmt::format(
        "## Web Context\n\n{}{}\n\n---\n\n## Question\n\n{}",
        context, fact_section, user_query
    );

    std::vector<Message> messages = history;
    messages.push_back({"user", user_msg});

    std::string sys_prompt = get_system_prompt(intent);
    auto response = call_api(messages, sys_prompt);

    if (response.success) {
        response.citations = extract_citations(response.answer, search_results, scraped_pages);
        response.key_facts = facts;
        response.mode_used = SynthesisMode::STANDARD;
        response.passes_used = 1;
    }
    return response;
}

// ── synthesise_deep (map-reduce) ──────────────────────────────────────────────
AIResponse AIBrain::synthesise_deep(
        const std::string&              user_query,
        const std::vector<SearchResult>& search_results,
        const std::vector<ScrapedPage>&  scraped_pages,
        const std::vector<Message>&      history,
        QueryIntent intent) {

    spdlog::info("[AIBrain] Deep synthesis: map-reduce over {} pages", scraped_pages.size());

    // MAP phase: summarise each page individually
    auto summaries = map_summarise(user_query, scraped_pages);

    // REDUCE phase: combine all summaries into final answer
    auto response = reduce_synthesise(user_query, summaries, search_results,
                                       scraped_pages, history, intent);

    response.mode_used = SynthesisMode::DEEP;
    response.passes_used = static_cast<int>(summaries.size()) + 1;

    if (response.success) {
        response.citations = extract_citations(response.answer, search_results, scraped_pages);
        response.key_facts = extract_key_facts(scraped_pages, user_query);
    }

    return response;
}

// ── map_summarise ─────────────────────────────────────────────────────────────
std::vector<std::string> AIBrain::map_summarise(
        const std::string& query,
        const std::vector<ScrapedPage>& pages) {

    std::vector<std::string> summaries;
    std::string map_prompt = R"(You are a research assistant. Summarise the following web page content,
focusing specifically on information relevant to the query. Extract key facts, dates, numbers, and claims.
Keep your summary concise (200-400 words) but include all relevant details.
Format: bullet points for key facts, then a brief narrative summary.)";

    for (const auto& page : pages) {
        if (!page.success || page.text.empty()) continue;

        std::string text = page.text;
        if (static_cast<int>(text.size()) > config_.max_context_chars / 2)
            text = text.substr(0, static_cast<size_t>(config_.max_context_chars / 2));

        std::string user_msg = fmt::format(
            "Query: {}\n\nPage: {} ({})\n\n{}", query, page.title, page.url, text);

        std::vector<Message> msgs = {{"user", user_msg}};
        auto resp = call_api(msgs, map_prompt);

        if (resp.success && !resp.answer.empty()) {
            summaries.push_back(fmt::format("### {} ({})\n{}\n",
                                            page.title, page.url, resp.answer));
            spdlog::debug("[AIBrain] Map: summarised \"{}\"", page.title);
        }
    }

    spdlog::info("[AIBrain] Map phase complete: {} summaries", summaries.size());
    return summaries;
}

// ── reduce_synthesise ─────────────────────────────────────────────────────────
AIResponse AIBrain::reduce_synthesise(
        const std::string& query,
        const std::vector<std::string>& summaries,
        const std::vector<SearchResult>& results,
        const std::vector<ScrapedPage>& pages,
        const std::vector<Message>& history,
        QueryIntent intent) {

    // Build context from summaries + search snippets
    std::ostringstream ctx;
    ctx << "### Search Result Snippets\n\n";
    int i = 1;
    for (const auto& r : results) {
        if (i > 10) break;
        ctx << fmt::format("{}. **{}** ({})\n   {}\n\n", i++, r.title, r.url, r.snippet);
    }

    ctx << "\n### Detailed Page Summaries\n\n";
    for (const auto& s : summaries) {
        ctx << s << "\n---\n\n";
    }

    std::string user_msg = fmt::format(
        "## Synthesised Web Research\n\n{}\n\n---\n\n## Question\n\n{}\n\n"
        "Please provide a comprehensive, well-cited answer based on ALL the research above. "
        "Note any conflicts between sources.",
        ctx.str(), query
    );

    std::vector<Message> messages = history;
    messages.push_back({"user", user_msg});

    std::string sys_prompt = get_system_prompt(intent);
    return call_api(messages, sys_prompt);
}

// ── extract_key_facts ─────────────────────────────────────────────────────────
std::vector<ExtractedFact> AIBrain::extract_key_facts(
        const std::vector<ScrapedPage>& pages,
        const std::string& query) const {

    std::vector<ExtractedFact> facts;

    // Extract sentences containing query terms from each page
    std::vector<std::string> query_words;
    std::istringstream qss(query);
    std::string w;
    while (qss >> w) {
        std::transform(w.begin(), w.end(), w.begin(), ::tolower);
        if (w.size() > 3) query_words.push_back(w); // skip short words
    }

    for (const auto& page : pages) {
        if (!page.success || page.text.empty()) continue;

        std::string domain = HttpClient::extract_domain(page.url);

        // Split text into sentences (simple split on . ! ?)
        std::istringstream ss(page.text);
        std::string line;
        while (std::getline(ss, line, '.')) {
            if (line.size() < 20 || line.size() > 500) continue;

            std::string lower_line = line;
            std::transform(lower_line.begin(), lower_line.end(),
                          lower_line.begin(), ::tolower);

            // Check if sentence contains query terms
            int matches = 0;
            for (const auto& qw : query_words) {
                if (lower_line.find(qw) != std::string::npos) ++matches;
            }

            if (matches >= 1 && !query_words.empty()) {
                ExtractedFact fact;
                // Trim the line
                size_t first = line.find_first_not_of(" \t\n\r");
                size_t last = line.find_last_not_of(" \t\n\r");
                if (first != std::string::npos)
                    fact.fact = line.substr(first, last - first + 1) + ".";
                fact.source_url = page.url;
                fact.source_domain = domain;
                fact.confidence = static_cast<double>(matches) / static_cast<double>(query_words.size());
                facts.push_back(std::move(fact));

                if (facts.size() >= 20) break; // cap
            }
        }
    }

    // Sort by confidence
    std::stable_sort(facts.begin(), facts.end(),
        [](const ExtractedFact& a, const ExtractedFact& b) {
            return a.confidence > b.confidence;
        });

    // Cap at 15 facts
    if (facts.size() > 15) facts.resize(15);
    return facts;
}

// ── chat ──────────────────────────────────────────────────────────────────────
AIResponse AIBrain::chat(const std::string& user_message,
                          const std::vector<Message>& history) {
    std::vector<Message> messages = history;
    messages.push_back({"user", user_message});
    return call_api(messages, BASE_SYSTEM_PROMPT);
}

// ── call_api ─────────────────────────────────────────────────────────────────
AIResponse AIBrain::call_api(const std::vector<Message>& messages,
                               const std::string& system_prompt) {
    AIResponse result;
    result.model = config_.model;

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

    HttpRequest req;
    req.method          = "POST";
    req.url             = OLLAMA_API_URL;
    req.body            = body.dump();
    req.headers         = {{"content-type", "application/json"}, {"accept", "application/json"}};
    req.timeout_seconds = 300;
    req.connect_timeout = 10;
    req.rotate_user_agent = false; // Don't rotate UA for local Ollama
    req.max_retries     = 1;
    auto resp = http_->execute(req);

    if (!resp.is_ok()) {
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

    try {
        auto j = json::parse(resp.body);
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

    if (!results.empty()) {
        ctx << "### Search Results\n\n";
        int i = 1;
        for (const auto& r : results) {
            if (total_chars > config_.max_context_chars / 3) break;
            std::string entry = fmt::format(
                "{}. **{}**\n   URL: {}\n   {}\n\n", i++, r.title, r.url, r.snippet);
            ctx << entry;
            total_chars += static_cast<int>(entry.size());
        }
    }

    if (!pages.empty()) {
        ctx << "\n### Full Page Content\n\n";
        for (const auto& p : pages) {
            if (!p.success || p.text.empty()) continue;
            if (total_chars >= config_.max_context_chars) break;

            int remaining = config_.max_context_chars - total_chars;
            std::string text = p.text;
            if (static_cast<int>(text.size()) > remaining)
                text = text.substr(0, static_cast<size_t>(remaining)) + "\n[truncated]";

            // Include metadata if available
            std::string meta_info;
            if (!p.meta.author.empty())
                meta_info += fmt::format(" | Author: {}", p.meta.author);
            if (!p.meta.publish_date.empty())
                meta_info += fmt::format(" | Date: {}", p.meta.publish_date);

            std::string entry = fmt::format(
                "#### {} ({}{})\n\n{}\n\n---\n\n",
                p.title, p.url, meta_info, text);
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

    // If no citations found in text, add top sources anyway
    if (citations.empty()) {
        for (const auto& r : results) {
            if (citations.size() >= 3) break;
            std::string dom = domain_of(r.url);
            if (seen.insert(dom).second)
                citations.push_back({r.url, r.title, r.snippet});
        }
        for (const auto& p : pages) {
            if (citations.size() >= 5) break;
            std::string dom = domain_of(p.url);
            if (seen.insert(dom).second)
                citations.push_back({p.url, p.title, ""});
        }
    }

    return citations;
}

} // namespace nobody