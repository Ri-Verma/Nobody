// =============================================================================
//  ui/CLI.cpp
// =============================================================================

#include "ui/CLI.h"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <iostream>
#include <sstream>
#include <algorithm>

namespace nobody {

// ANSI colour codes
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

// ── Colour helper ─────────────────────────────────────────────────────────────
std::string CLI::c(const std::string& code, const std::string& text) const {
    if (!config_.color) return text;
    return code + text + col::RESET;
}

// ── Banner ────────────────────────────────────────────────────────────────────
void CLI::print_banner() {
    if (config_.color) std::cout << col::CYAN;
    std::cout << R"(
  ██████╗ ███████╗██╗███╗   ██╗████████╗       █████╗ ██╗
 ██╔═══██╗██╔════╝██║████╗  ██║╚══██╔══╝      ██╔══██╗██║
 ██║   ██║███████╗██║██╔██╗ ██║   ██║   █████╗███████║██║
 ██║   ██║╚════██║██║██║╚██╗██║   ██║   ╚════╝██╔══██║██║
 ╚██████╔╝███████║██║██║ ╚████║   ██║         ██║  ██║██║
  ╚═════╝ ╚══════╝╚═╝╚═╝  ╚═══╝   ╚═╝         ╚═╝  ╚═╝╚═╝
)" << col::RESET << "\n";

    std::cout << c(col::BOLD, "  Open Source Intelligence AI  ")
              << c(col::DIM,  "v1.0.0\n")
              << c(col::DIM,  "  Powered by live web search + Claude AI\n\n")
              << "  Type a question or command. Type "
              << c(col::YELLOW, "/help") << " for help.\n"
              << "  " << c(col::DIM, std::string(60,'-')) << "\n\n";
}

// ── Help ──────────────────────────────────────────────────────────────────────
void CLI::print_help() {
    std::cout << c(col::BOLD, "\n  Commands:\n\n");
    const std::vector<std::pair<std::string,std::string>> cmds = {
        {"/help",         "Show this help"},
        {"/clear",        "Clear conversation history"},
        {"/history",      "Show conversation history"},
        {"/sources on|off","Toggle sources display"},
        {"/search on|off", "Toggle search results display"},
        {"/timing on|off", "Toggle timing info"},
        {"/quit or /exit", "Exit the program"},
    };
    for (const auto& [cmd, desc] : cmds)
        std::cout << "    " << c(col::YELLOW, fmt::format("{:<22}", cmd))
                  << c(col::DIM, desc) << "\n";
    std::cout << "\n";
}

// ── Main REPL ─────────────────────────────────────────────────────────────────
void CLI::run() {
    print_banner();
    std::string input;

    while (true) {
        std::string prompt_color = fmt::format("{}{}", col::BOLD, col::CYAN);
        std::cout << c(prompt_color, "nobody> ") << std::flush;

        if (!std::getline(std::cin, input)) break; // EOF

        // Trim
        input.erase(0, input.find_first_not_of(" \t\r\n"));
        input.erase(input.find_last_not_of(" \t\r\n") + 1);

        if (input.empty()) continue;

        if (is_command(input)) {
            if (!handle_command(input)) break; // /quit or /exit
            continue;
        }

        process(input);
    }

    std::cout << "\n" << c(col::DIM, "  Goodbye.\n\n");
}

// ── Single query handler ──────────────────────────────────────────────────────
void CLI::process(const std::string& query) {
    std::cout << "\n" << c(col::DIM, std::string(60,'-')) << "\n";
    std::cout << c(col::BOLD, "  Searching & analysing: ")
              << c(col::CYAN, "\"" + query + "\"") << "\n\n";

    // Spinner effect (simple)
    std::cout << "  " << c(col::DIM, "[ searching... ]") << "\r" << std::flush;

    auto result = engine_->run(query, history_);

    // Clear the spinner line
    std::cout << "                           \r";

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
        // Keep history manageable (last 10 turns = 20 messages)
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
        if (i > 5) break; // show top 5 in UI
        std::string snippet = r.snippet;
        if (static_cast<int>(snippet.size()) > config_.max_snippet_chars)
            snippet = snippet.substr(0, static_cast<size_t>(config_.max_snippet_chars)) + "…";

        std::cout << "  " << c(col::DIM, std::to_string(i) + ". ")
                  << c(col::BOLD, r.title) << "\n"
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

    // Word-wrap and indent the answer text
    std::istringstream stream(response.answer);
    std::string line;
    while (std::getline(stream, line)) {
        // Detect markdown headers and colour them
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
    std::cout << c(col::DIM, fmt::format(
        "  ⏱  {} ms total  |  {} search results  |  {} pages scraped"
        "  |  {} + {} tokens\n",
        result.total_time.count(),
        result.search_results.size(),
        result.scraped_pages.size(),
        result.ai_response.input_tokens,
        result.ai_response.output_tokens
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

    if (cmd == "/sources on")  { config_.show_sources = true;  std::cout << "  Sources: on\n\n";  return true; }
    if (cmd == "/sources off") { config_.show_sources = false; std::cout << "  Sources: off\n\n"; return true; }
    if (cmd == "/search on")   { config_.show_search_results = true;  std::cout << "  Search results: on\n\n";  return true; }
    if (cmd == "/search off")  { config_.show_search_results = false; std::cout << "  Search results: off\n\n"; return true; }
    if (cmd == "/timing on")   { config_.show_timing = true;  std::cout << "  Timing: on\n\n";  return true; }
    if (cmd == "/timing off")  { config_.show_timing = false; std::cout << "  Timing: off\n\n"; return true; }

    std::cout << c(col::YELLOW, "  Unknown command: ") << cmd
              << ". Type /help for commands.\n\n";
    return true;
}

} // namespace nobody