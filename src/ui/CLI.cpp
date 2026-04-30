// =============================================================================
//  ui/CLI.cpp — Enhanced with /depth, /engines commands, dork display
// =============================================================================

#include "ui/CLI.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <iostream>
#include <sstream>
#include <algorithm>

namespace nobody {

namespace col {
    constexpr const char* RESET  = "\033[0m";
    constexpr const char* BOLD   = "\033[1m";
    constexpr const char* DIM    = "\033[2m";
    constexpr const char* CYAN   = "\033[36m";
    constexpr const char* GREEN  = "\033[32m";
    constexpr const char* YELLOW = "\033[33m";
    constexpr const char* BLUE   = "\033[34m";
    constexpr const char* RED    = "\033[31m";
    constexpr const char* MAGENTA= "\033[35m";
    constexpr const char* WHITE  = "\033[37m";
}

CLI::CLI(std::shared_ptr<NobodyEngine> engine, CLIConfig cfg)
    : engine_(std::move(engine)), config_(std::move(cfg)) {}

std::string CLI::c(const std::string& code, const std::string& text) const {
    if (!config_.color) return text;
    return code + text + col::RESET;
}

// ── Banner ────────────────────────────────────────────────────────────────────
void CLI::print_banner() {
    if (config_.color) std::cout << col::CYAN;
    std::cout << R"(
 ███╗   ██╗ ██████╗ ██████╗  ██████╗ ██████╗ ██╗   ██╗
 ████╗  ██║██╔═══██╗██╔══██╗██╔═══██╗██╔══██╗╚██╗ ██╔╝
 ██╔██╗ ██║██║   ██║██████╔╝██║   ██║██║  ██║ ╚████╔╝
 ██║╚██╗██║██║   ██║██╔══██╗██║   ██║██║  ██║  ╚██╔╝
 ██║ ╚████║╚██████╔╝██████╔╝╚██████╔╝██████╔╝   ██║
 ╚═╝  ╚═══╝ ╚═════╝ ╚═════╝  ╚═════╝ ╚═════╝    ╚═╝
)" << col::RESET << "\n";

    std::cout << c(col::BOLD, fmt::format("  {}Nobody{}  ", col::CYAN, col::RESET))
              << c(col::DIM,  "v2.0.0\n")
              << c(col::DIM,  "  Powered by live web search + Local Ollama AI\n")
              << c(col::DIM,  "  Features: Google Dorking | Multi-Engine | BM25 | Map-Reduce AI\n\n")
              << "  Type a question or command. Type "
              << c(col::YELLOW, "/help") << " for help.\n"
              << "  " << c(col::DIM, std::string(60,'-')) << "\n\n";
}

// ── Help ──────────────────────────────────────────────────────────────────────
void CLI::print_help() {
    std::cout << c(col::BOLD, "\n  Commands:\n\n");
    const std::vector<std::pair<std::string,std::string>> cmds = {
        {"/help",              "Show this help"},
        {"/clear",             "Clear conversation history"},
        {"/history",           "Show conversation history"},
        {"/sources on|off",    "Toggle sources display"},
        {"/search on|off",     "Toggle search results display"},
        {"/timing on|off",     "Toggle timing info"},
        {"/depth quick|std|deep", "Set scraping depth mode"},
        {"/quit or /exit",     "Exit the program"},
    };
    for (const auto& [cmd, desc] : cmds)
        std::cout << "    " << c(col::YELLOW, fmt::format("{:<26}", cmd))
                  << c(col::DIM, desc) << "\n";
    std::cout << "\n"
              << c(col::DIM, "    Tip: Paste a URL (http://...) to research a topic from that page.\n")
              << "\n";
}

// ── Main REPL ─────────────────────────────────────────────────────────────────
void CLI::run() {
    print_banner();
    std::string input;

    while (true) {
        std::string prompt_color = fmt::format("{}{}", col::BOLD, col::CYAN);
        std::cout << c(prompt_color, "nobody> ") << std::flush;

        if (!std::getline(std::cin, input)) break;

        input.erase(0, input.find_first_not_of(" \t\r\n"));
        input.erase(input.find_last_not_of(" \t\r\n") + 1);

        if (input.empty()) continue;

        if (is_command(input)) {
            if (!handle_command(input)) break;
            continue;
        }

        // Auto-detect URLs: route to URL research pipeline
        if (NobodyEngine::is_url(input)) {
            process_url(input);
        } else {
            process(input);
        }
    }

    std::cout << "\n" << c(col::DIM, "  Goodbye.\n\n");
}

// ── Single query handler ──────────────────────────────────────────────────────
void CLI::process(const std::string& query) {
    std::cout << "\n" << c(col::DIM, std::string(60,'-')) << "\n";
    std::cout << c(col::BOLD, "  Searching & analysing: ")
              << c(col::CYAN, "\"" + query + "\"") << "\n\n";

    std::cout << "  " << c(col::DIM, "[ searching... ]") << "\r" << std::flush;

    auto result = engine_->run(query, history_);

    std::cout << "                           \r";

    // ── Dork queries (verbose) ──────────────────────────────────────────
    if (!result.dork_queries.empty() && config_.show_search_results) {
        std::cout << c(col::BOLD, "  Dork Queries Used:\n");
        for (size_t i = 0; i < result.dork_queries.size(); ++i) {
            std::cout << "  " << c(col::DIM, std::to_string(i+1) + ". ")
                      << c(col::MAGENTA, result.dork_queries[i]) << "\n";
        }
        std::cout << "\n";
    }

    // ── Search results ──────────────────────────────────────────────────
    if (config_.show_search_results && !result.search_results.empty())
        print_search_results(result.search_results);

    // ── AI Answer ───────────────────────────────────────────────────────
    print_answer(result.ai_response);

    // ── Sources ─────────────────────────────────────────────────────────
    if (config_.show_sources && !result.ai_response.citations.empty())
        print_sources(result.ai_response.citations);

    // ── Timing ──────────────────────────────────────────────────────────
    if (config_.show_timing) print_timing(result);

    // ── Update conversation history ──────────────────────────────────────
    if (result.success) {
        history_.push_back({"user",      query});
        history_.push_back({"assistant", result.ai_response.answer});
        if (history_.size() > 20)
            history_.erase(history_.begin(), history_.begin() + 2);
    }

    std::cout << "\n";
}

// ── URL research handler ──────────────────────────────────────────────────────
void CLI::process_url(const std::string& url) {
    std::cout << "\n" << c(col::DIM, std::string(60,'-')) << "\n";
    std::cout << c(col::BOLD, "  URL Research Mode\n")
              << "  Seed: " << c(col::BLUE, url) << "\n\n";

    std::cout << "  " << c(col::DIM, "[ scraping seed URL... ]") << "\r" << std::flush;

    auto result = engine_->run_from_url(url, history_);

    std::cout << "                                \r";

    // ── Show extracted topic ──────────────────────────────────────────────
    if (!result.query.empty()) {
        std::cout << c(col::BOLD, "  Extracted Topic: ")
                  << c(col::CYAN, "\"" + result.query + "\"") << "\n";
    }
    if (!result.seed_url.empty()) {
        std::cout << c(col::DIM, "  Seed Page: ")
                  << c(col::BLUE, result.seed_url) << "\n";
    }
    if (!result.scraped_pages.empty()) {
        std::cout << c(col::DIM, fmt::format("  Pages scraped: {} (1 seed + {} related)\n",
            result.scraped_pages.size(), result.scraped_pages.size() - 1));
    }
    std::cout << "\n";

    // ── Dork queries ─────────────────────────────────────────────────────
    if (!result.dork_queries.empty() && config_.show_search_results) {
        std::cout << c(col::BOLD, "  Dork Queries Used:\n");
        for (size_t i = 0; i < result.dork_queries.size(); ++i) {
            std::cout << "  " << c(col::DIM, std::to_string(i+1) + ". ")
                      << c(col::MAGENTA, result.dork_queries[i]) << "\n";
        }
        std::cout << "\n";
    }

    // ── Search results ───────────────────────────────────────────────────
    if (config_.show_search_results && !result.search_results.empty())
        print_search_results(result.search_results);

    // ── AI Answer ────────────────────────────────────────────────────────
    print_answer(result.ai_response);

    // ── Sources ──────────────────────────────────────────────────────────
    if (config_.show_sources && !result.ai_response.citations.empty())
        print_sources(result.ai_response.citations);

    // ── Timing ───────────────────────────────────────────────────────────
    if (config_.show_timing) print_timing(result);

    // ── Update history ───────────────────────────────────────────────────
    if (result.success) {
        std::string summary = "Research URL: " + url + " (topic: " + result.query + ")";
        history_.push_back({"user",      summary});
        history_.push_back({"assistant", result.ai_response.answer});
        if (history_.size() > 20)
            history_.erase(history_.begin(), history_.begin() + 2);
    }

    std::cout << "\n";
}

// ── Print search results ──────────────────────────────────────────────────────
void CLI::print_search_results(const std::vector<SearchResult>& results) {
    std::cout << c(col::BOLD, "  Web Results:\n");
    int i = 1;
    for (const auto& r : results) {
        if (i > 5) break;
        std::string snippet = r.snippet;
        if (static_cast<int>(snippet.size()) > config_.max_snippet_chars)
            snippet = snippet.substr(0, static_cast<size_t>(config_.max_snippet_chars)) + "…";

        std::string engine_tag = (r.engine_hits > 1)
            ? fmt::format(" [{}x engines]", r.engine_hits) : "";

        std::cout << "  " << c(col::DIM, std::to_string(i) + ". ")
                  << c(col::BOLD, r.title)
                  << c(col::DIM, engine_tag) << "\n"
                  << "     " << c(col::BLUE, r.url) << "\n"
                  << "     " << c(col::DIM, snippet) << "\n\n";
        ++i;
    }
}

// ── Print AI answer ───────────────────────────────────────────────────────────
void CLI::print_answer(const AIResponse& response) {
    if (!response.success) {
        std::cout << c(col::RED, "  ✗ Error: ") << response.error << "\n";
        return;
    }

    std::string answer_color = fmt::format("{}{}", col::BOLD, col::GREEN);
    std::cout << c(answer_color, "  Answer:\n");
    std::cout << c(col::DIM, "  " + std::string(56,'-')) << "\n";

    std::istringstream stream(response.answer);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line[0] == '#') {
            size_t hashes = line.find_first_not_of('#');
            std::string header_text = (hashes != std::string::npos)
                                      ? line.substr(hashes + 1) : line;
            std::string header_color = fmt::format("{}{}", col::BOLD, col::CYAN);
            std::cout << "\n  " << c(header_color, header_text) << "\n";
        } else if (!line.empty() && line.substr(0,2) == "**" ) {
            std::cout << "  " << c(col::BOLD, line) << "\n";
        } else {
            std::cout << "  " << line << "\n";
        }
    }
    std::cout << c(col::DIM, "  " + std::string(56,'-')) << "\n\n";
}

// ── Print sources ─────────────────────────────────────────────────────────────
void CLI::print_sources(const std::vector<Citation>& citations) {
    std::cout << c(col::BOLD, "  Sources:\n");
    int i = 1;
    for (const auto& cit : citations) {
        std::cout << "  " << c(col::DIM, std::to_string(i++) + ". ")
                  << c(col::BLUE, cit.url) << "\n";
        if (!cit.title.empty())
            std::cout << "     " << c(col::DIM, cit.title) << "\n";
    }
    std::cout << "\n";
}

// ── Timing ────────────────────────────────────────────────────────────────────
void CLI::print_timing(const NobodyResult& result) {
    std::string mode_str = "standard";
    if (result.ai_response.mode_used == SynthesisMode::DEEP) mode_str = "deep";
    else if (result.ai_response.mode_used == SynthesisMode::QUICK) mode_str = "quick";

    std::cout << c(col::DIM, fmt::format(
        "  ⏱  {} ms total  |  {} results  |  {} pages scraped"
        "  |  {} + {} tokens  |  {} passes  |  mode: {}\n",
        result.total_time.count(),
        result.search_results.size(),
        result.scraped_pages.size(),
        result.ai_response.input_tokens,
        result.ai_response.output_tokens,
        result.ai_response.passes_used,
        mode_str
    ));
}

// ── Command handling ──────────────────────────────────────────────────────────
bool CLI::is_command(const std::string& input) {
    return !input.empty() && input[0] == '/';
}

bool CLI::handle_command(const std::string& cmd) {
    if (cmd == "/quit" || cmd == "/exit") return false;
    if (cmd == "/help") { print_help(); return true; }

    if (cmd == "/clear") {
        history_.clear();
        std::cout << c(col::GREEN, "  ✓ Conversation history cleared.\n\n");
        return true;
    }

    if (cmd == "/history") {
        if (history_.empty()) {
            std::cout << c(col::DIM, "  No history yet.\n\n");
        } else {
            std::cout << c(col::BOLD, "\n  Conversation history:\n");
            for (size_t i = 0; i < history_.size(); ++i) {
                const auto& m = history_[i];
                std::string role_label = (m.role == "user")
                    ? c(col::CYAN,  "  You : ")
                    : c(col::GREEN, "  AI  : ");
                std::string preview = m.content.substr(
                    0, std::min<size_t>(80, m.content.size()));
                if (m.content.size() > 80) preview += "…";
                std::cout << role_label << preview << "\n";
            }
            std::cout << "\n";
        }
        return true;
    }

    // Toggle commands
    if (cmd == "/sources on")  { config_.show_sources = true;  std::cout << "  Sources: on\n\n";  return true; }
    if (cmd == "/sources off") { config_.show_sources = false; std::cout << "  Sources: off\n\n"; return true; }
    if (cmd == "/search on")   { config_.show_search_results = true;  std::cout << "  Search results: on\n\n";  return true; }
    if (cmd == "/search off")  { config_.show_search_results = false; std::cout << "  Search results: off\n\n"; return true; }
    if (cmd == "/timing on")   { config_.show_timing = true;  std::cout << "  Timing: on\n\n";  return true; }
    if (cmd == "/timing off")  { config_.show_timing = false; std::cout << "  Timing: off\n\n"; return true; }

    // Depth commands
    if (cmd == "/depth quick" || cmd == "/depth q") {
        EngineConfig cfg = engine_->config();
        cfg.depth_mode = DepthMode::QUICK;
        engine_->set_config(cfg);
        std::cout << c(col::GREEN, "  ✓ Depth: QUICK (fast, fewer sources)\n\n");
        return true;
    }
    if (cmd == "/depth standard" || cmd == "/depth std" || cmd == "/depth s") {
        EngineConfig cfg = engine_->config();
        cfg.depth_mode = DepthMode::STANDARD;
        engine_->set_config(cfg);
        std::cout << c(col::GREEN, "  ✓ Depth: STANDARD (balanced)\n\n");
        return true;
    }
    if (cmd == "/depth deep" || cmd == "/depth d") {
        EngineConfig cfg = engine_->config();
        cfg.depth_mode = DepthMode::DEEP;
        engine_->set_config(cfg);
        std::cout << c(col::GREEN, "  ✓ Depth: DEEP (thorough, map-reduce AI)\n\n");
        return true;
    }

    std::cout << c(col::YELLOW, "  Unknown command: ") << cmd
              << ". Type /help for commands.\n\n";
    return true;
}

} // namespace nobody