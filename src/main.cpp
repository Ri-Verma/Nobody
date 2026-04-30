// =============================================================================
//  main.cpp  —  Nobody v2.0 entry point
//  New flags: --depth, --dork, --engines
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

static std::string getenv_str(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}

static json load_config(const std::string& path = "config.json") {
    std::ifstream f(path);
    if (!f.is_open()) return json::object();
    try {
        json j = json::parse(f);
        return j.is_object() ? j : json::object();
    }
    catch (...) { return json::object(); }
}

static void setup_logging(const std::string& level) {
    auto logger = spdlog::stdout_color_mt("nobody");
    spdlog::set_default_logger(logger);
    if      (level == "debug") spdlog::set_level(spdlog::level::debug);
    else if (level == "warn")  spdlog::set_level(spdlog::level::warn);
    else if (level == "error") spdlog::set_level(spdlog::level::err);
    else                       spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");
}

static void print_usage(const char* prog) {
    fmt::print(R"(
Usage: {} [OPTIONS] [QUERY]

Options:
  -q, --query <text>        Run a single query (non-interactive)
  -n, --no-scrape           Disable web page scraping (faster, less context)
  -s, --no-search-ui        Hide search results in output
  -t, --no-timing           Hide timing information
  -v, --verbose             Enable debug logging
  --no-color                Disable ANSI colours
  --depth <mode>            Set depth: quick, standard, deep (default: standard)
  --dork                    Force Google Dorking for all queries
  --no-dork                 Disable Google Dorking
  -h, --help                Show this help

Environment variables:
  OLLAMA_MODEL              Ollama local model (default: llama3.1:latest)
  GOOGLE_API_KEY            Google Custom Search API key (optional)
  GOOGLE_CX                 Google Custom Search Engine ID (optional)
  BRAVE_API_KEY             Brave Search API key (optional)
  BING_API_KEY              Bing Search API key (optional)
  NOBODY_LOG_LEVEL          debug|info|warn|error (default: info)

Depth modes:
  quick      Fast answers, fewer sources, single-pass AI
  standard   Balanced scraping and synthesis (default)
  deep       Maximum coverage, map-reduce AI, concurrent scraping

Examples:
  {} "What is quantum entanglement?"
  {} -q "Who is Elon Musk?" --depth deep
  {} --dork -q "latest AI research 2026"
  {} --verbose --depth quick
)", prog, prog, prog, prog, prog);
}

int main(int argc, char* argv[]) {
    std::string single_query;
    bool        no_scrape      = getenv_str("NOBODY_NO_SCRAPE")  != "";
    bool        no_color       = getenv_str("NOBODY_NO_COLOR")   != "";
    bool        show_search    = true;
    bool        show_timing    = true;
    bool        verbose        = false;
    std::string depth_str      = "standard";
    bool        force_dork     = false;
    bool        no_dork        = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")         { print_usage(argv[0]); return 0; }
        if (arg == "-v" || arg == "--verbose")      { verbose = true; continue; }
        if (arg == "-n" || arg == "--no-scrape")    { no_scrape = true; continue; }
        if (arg == "-s" || arg == "--no-search-ui") { show_search = false; continue; }
        if (arg == "-t" || arg == "--no-timing")    { show_timing = false; continue; }
        if (arg == "--no-color")                    { no_color = true; continue; }
        if (arg == "--dork")                        { force_dork = true; continue; }
        if (arg == "--no-dork")                     { no_dork = true; continue; }
        if (arg == "--depth" && i+1 < argc)         { depth_str = argv[++i]; continue; }
        if ((arg == "-q" || arg == "--query") && i+1 < argc) {
            single_query = argv[++i]; continue;
        }
        if (arg[0] != '-') { single_query = arg; continue; }
        fmt::print(stderr, "Unknown option: {}\n", arg);
        return 1;
    }

    setup_logging(verbose ? "debug" : getenv_str("NOBODY_LOG_LEVEL", "warn"));

    auto cfg = load_config();

    // ── API keys ──────────────────────────────────────────────────────────
    std::string ollama_model = getenv_str("OLLAMA_MODEL",
        cfg.value("ollama_model", "llama3.1:latest"));
    std::string google_key   = getenv_str("GOOGLE_API_KEY",
        cfg.value("google_api_key", ""));
    std::string google_cx    = getenv_str("GOOGLE_CX",
        cfg.value("google_cx", ""));
    std::string brave_key    = getenv_str("BRAVE_API_KEY",
        cfg.contains("search") ? cfg["search"].value("brave_api_key", "") : "");
    std::string bing_key     = getenv_str("BING_API_KEY",
        cfg.contains("search") ? cfg["search"].value("bing_api_key", "") : "");

    // ── Parse depth mode ──────────────────────────────────────────────────
    DepthMode depth_mode = DepthMode::STANDARD;
    if (depth_str == "quick" || depth_str == "q") depth_mode = DepthMode::QUICK;
    else if (depth_str == "deep" || depth_str == "d") depth_mode = DepthMode::DEEP;

    // ── Build dependency graph ────────────────────────────────────────────
    auto http = std::make_shared<HttpClient>();

    // Search config
    SearchConfig search_cfg;
    search_cfg.max_results    = cfg.contains("search") ? cfg["search"].value("max_results", 15) : 15;
    search_cfg.use_duckduckgo = true;
    search_cfg.use_google     = !google_key.empty();
    search_cfg.google_api_key = google_key;
    search_cfg.google_cx      = google_cx;
    search_cfg.use_brave      = !brave_key.empty();
    search_cfg.brave_api_key  = brave_key;
    search_cfg.use_bing       = !bing_key.empty();
    search_cfg.bing_api_key   = bing_key;
    search_cfg.use_wayback    = cfg.contains("search") ? cfg["search"].value("use_wayback", true) : true;

    // Dorking config
    search_cfg.dorking.enabled = !no_dork;
    if (force_dork) search_cfg.dorking.enabled = true;
    if (cfg.contains("search")) {
        search_cfg.dorking.max_dork_queries =
            cfg["search"].value("dork_queries_per_search", 3);
        if (cfg["search"].contains("negative_domains")) {
            search_cfg.dorking.negative_domains.clear();
            for (const auto& d : cfg["search"]["negative_domains"])
                search_cfg.dorking.negative_domains.push_back(d.get<std::string>());
        }
    }

    auto search = std::make_shared<SearchEngine>(http, search_cfg);

    // Scraper config
    ScraperConfig scraper_cfg;
    scraper_cfg.max_text_length   = cfg.contains("scraper") ? cfg["scraper"].value("max_text_length", 12000) : 12000;
    scraper_cfg.strip_boilerplate = true;
    scraper_cfg.max_concurrent    = cfg.contains("scraper") ? cfg["scraper"].value("max_concurrent", 4) : 4;
    scraper_cfg.use_wayback_fallback = cfg.contains("scraper") ? cfg["scraper"].value("use_wayback_fallback", true) : true;
    scraper_cfg.extract_meta      = cfg.contains("scraper") ? cfg["scraper"].value("extract_meta", true) : true;
    scraper_cfg.extract_structured = cfg.contains("scraper") ? cfg["scraper"].value("extract_structured_data", true) : true;
    auto scraper = std::make_shared<WebScraper>(http, scraper_cfg);

    // AI config
    AIConfig ai_cfg;
    ai_cfg.api_key          = "";
    ai_cfg.model            = ollama_model;
    ai_cfg.max_tokens       = cfg.contains("ai") ? cfg["ai"].value("max_tokens", 2500) : 2500;
    ai_cfg.temperature      = cfg.contains("ai") ? cfg["ai"].value("temperature", 0.2) : 0.2;
    ai_cfg.max_context_chars = cfg.contains("ai") ? cfg["ai"].value("max_context_chars", 24000) : 24000;

    std::string synth_mode_str = cfg.contains("ai") ? cfg["ai"].value("synthesis_mode", "standard") : "standard";
    if (synth_mode_str == "quick") ai_cfg.synthesis_mode = SynthesisMode::QUICK;
    else if (synth_mode_str == "deep") ai_cfg.synthesis_mode = SynthesisMode::DEEP;

    auto ai = std::make_shared<AIBrain>(http, ai_cfg);

    // Engine config
    EngineConfig engine_cfg;
    engine_cfg.max_search_results = search_cfg.max_results;
    engine_cfg.pages_to_scrape    = cfg.contains("scraper") ? cfg["scraper"].value("pages_to_scrape", 5) : 5;
    engine_cfg.enable_scraping    = !no_scrape;
    engine_cfg.enable_dorking     = search_cfg.dorking.enabled;
    engine_cfg.depth_mode         = depth_mode;
    auto engine = std::make_shared<NobodyEngine>(http, search, scraper, ai, engine_cfg);

    // CLI config
    CLIConfig cli_cfg;
    cli_cfg.show_search_results = show_search;
    cli_cfg.show_timing         = show_timing;
    cli_cfg.color               = !no_color;
    auto cli = std::make_shared<CLI>(engine, cli_cfg);

    // ── Run ───────────────────────────────────────────────────────────────
    if (!single_query.empty()) {
        cli->print_banner();
        if (NobodyEngine::is_url(single_query)) {
            cli->process_url(single_query);
        } else {
            cli->process(single_query);
        }
    } else {
        cli->run();
    }

    return 0;
}