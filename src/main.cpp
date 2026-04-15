// =============================================================================
//  main.cpp  —  Nobody entry point
//
//  Wires up: HttpClient → SearchEngine + WebScraper → AIBrain → IntelligenceEngine
//  Then hands control to the CLI REPL.
//
//  Environment variables:
//    ANTHROPIC_API_KEY   (required for AI synthesis)
//    GOOGLE_API_KEY      (optional, for Google CSE)
//    GOOGLE_CX           (optional, Google Custom Search Engine ID)
//    NOBODY_LOG_LEVEL     (debug|info|warn|error, default: info)
//    NOBODY_NO_COLOR      (set to disable ANSI colours)
//    NOBODY_NO_SCRAPE     (set to disable web scraping)
// =============================================================================

#include "core/HttpClient.h"
#include "search/SearchEngine.h"
#include "scraper/WebScraper.h"
#include "ai/AIBrain.h"
#include "engine/IntelligenceEngine.h"
#include "ui/CLI.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <memory>
#include <stdexcept>

using json = nlohmann::json;
using namespace nobody;

// ── Env helper ────────────────────────────────────────────────────────────────
static std::string getenv_str(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}

// ── Load optional JSON config ─────────────────────────────────────────────────
static json load_config(const std::string& path = "config/config.json") {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    try { return json::parse(f); }
    catch (...) { return {}; }
}

// ── Logging setup ─────────────────────────────────────────────────────────────
static void setup_logging(const std::string& level) {
    auto logger = spdlog::stdout_color_mt("nobody");
    spdlog::set_default_logger(logger);
    if      (level == "debug") spdlog::set_level(spdlog::level::debug);
    else if (level == "warn")  spdlog::set_level(spdlog::level::warn);
    else if (level == "error") spdlog::set_level(spdlog::level::err);
    else                       spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");
}

// ── Print usage ───────────────────────────────────────────────────────────────
static void print_usage(const char* prog) {
    fmt::print(R"(
Usage: {} [OPTIONS] [QUERY]

Options:
  -q, --query <text>    Run a single query (non-interactive)
  -n, --no-scrape       Disable web page scraping (faster, less context)
  -s, --no-search-ui    Hide search results in output
  -t, --no-timing       Hide timing information
  -v, --verbose         Enable debug logging
  --no-color            Disable ANSI colours
  -h, --help            Show this help

Environment variables:
  ANTHROPIC_API_KEY     Anthropic Claude API key (required)
  GOOGLE_API_KEY        Google Custom Search API key (optional)
  GOOGLE_CX             Google Custom Search Engine ID (optional)
  NOBODY_LOG_LEVEL       debug|info|warn|error (default: info)

Examples:
  {} "What is quantum entanglement?"
  {} -q "Who is Elon Musk?" --no-scrape
  {} --verbose
)", prog, prog, prog, prog);
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // ── Parse CLI args ─────────────────────────────────────────────────────
    std::string single_query;
    bool        no_scrape      = getenv_str("NOBODY_NO_SCRAPE")  != "";
    bool        no_color       = getenv_str("NOBODY_NO_COLOR")   != "";
    bool        show_search    = true;
    bool        show_timing    = true;
    bool        verbose        = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")         { print_usage(argv[0]); return 0; }
        if (arg == "-v" || arg == "--verbose")      { verbose = true; continue; }
        if (arg == "-n" || arg == "--no-scrape")    { no_scrape = true; continue; }
        if (arg == "-s" || arg == "--no-search-ui") { show_search = false; continue; }
        if (arg == "-t" || arg == "--no-timing")    { show_timing = false; continue; }
        if (arg == "--no-color")                    { no_color = true; continue; }
        if ((arg == "-q" || arg == "--query") && i+1 < argc) {
            single_query = argv[++i]; continue;
        }
        // Positional argument = query
        if (arg[0] != '-') { single_query = arg; continue; }
        fmt::print(stderr, "Unknown option: {}\n", arg);
        return 1;
    }

    // ── Logging ───────────────────────────────────────────────────────────
    setup_logging(verbose ? "debug" : getenv_str("NOBODY_LOG_LEVEL", "warn"));

    // ── Config file ───────────────────────────────────────────────────────
    auto cfg = load_config();

    // ── API Keys ──────────────────────────────────────────────────────────
    std::string anthropic_key = getenv_str("ANTHROPIC_API_KEY",
        cfg.value("anthropic_api_key", ""));
    std::string google_key    = getenv_str("GOOGLE_API_KEY",
        cfg.value("google_api_key", ""));
    std::string google_cx     = getenv_str("GOOGLE_CX",
        cfg.value("google_cx", ""));

    if (anthropic_key.empty()) {
        fmt::print(stderr,
            "\n  ⚠  ANTHROPIC_API_KEY not set.\n"
            "     The AI synthesis step will fail.\n"
            "     Export it with:  export ANTHROPIC_API_KEY=sk-ant-...\n\n");
    }

    // ── Build the dependency graph ────────────────────────────────────────
    //
    //  HttpClient  ─────────────────────────────────────┐
    //      │                                            │
    //      ├─► SearchEngine (DuckDuckGo + Google CSE)  │
    //      │                                            │
    //      ├─► WebScraper (fetch & clean HTML)         │
    //      │                                            ▼
    //      └─► AIBrain  (Anthropic Claude API)     NobodyEngine
    //                                                   │
    //                                                   ▼
    //                                                  CLI

    auto http = std::make_shared<HttpClient>();

    SearchConfig search_cfg;
    search_cfg.max_results    = 8;
    search_cfg.use_duckduckgo = true;
    search_cfg.use_google     = !google_key.empty();
    search_cfg.google_api_key = google_key;
    search_cfg.google_cx      = google_cx;
    auto search = std::make_shared<SearchEngine>(http, search_cfg);

    ScraperConfig scraper_cfg;
    scraper_cfg.max_text_length  = 6000;
    scraper_cfg.strip_boilerplate = true;
    auto scraper = std::make_shared<WebScraper>(http, scraper_cfg);

    AIConfig ai_cfg;
    ai_cfg.api_key     = anthropic_key;
    ai_cfg.model       = "claude-sonnet-4-20250514";
    ai_cfg.max_tokens  = 1500;
    ai_cfg.temperature = 0.3;
    auto ai = std::make_shared<AIBrain>(http, ai_cfg);

    EngineConfig engine_cfg;
    engine_cfg.max_search_results = 8;
    engine_cfg.pages_to_scrape    = 3;
    engine_cfg.enable_scraping    = !no_scrape;
    auto engine = std::make_shared<NobodyEngine>(http, search, scraper, ai, engine_cfg);

    CLIConfig cli_cfg;
    cli_cfg.show_search_results = show_search;
    cli_cfg.show_timing         = show_timing;
    cli_cfg.color               = !no_color;
    auto cli = std::make_shared<CLI>(engine, cli_cfg);

    // ── Run ───────────────────────────────────────────────────────────────
    if (!single_query.empty()) {
        // Non-interactive single query mode
        cli->print_banner();
        cli->process(single_query);
    } else {
        // Interactive REPL
        cli->run();
    }

    return 0;
}