#pragma once
// =============================================================================
//  ui/CLI.h  —  Terminal user interface for osint-ai
// =============================================================================

#include "osint/OSINTEngine.h"
#include <string>
#include <memory>
#include <vector>

namespace osint {

struct CLIConfig {
    bool show_search_results = true;
    bool show_scraped_pages  = false;
    bool show_sources        = true;
    bool show_timing         = true;
    bool color               = true;
    int  max_snippet_chars   = 120;
};

class CLI {
public:
    explicit CLI(std::shared_ptr<OSINTEngine> engine, CLIConfig config = {});

    // Blocking REPL loop
    void run();

    // Process a single query (used by run() and for testing)
    void process(const std::string& query);

    void print_banner();
    void print_help();

private:
    std::shared_ptr<OSINTEngine> engine_;
    CLIConfig config_;
    std::vector<Message> history_;

    void print_search_results(const std::vector<SearchResult>& results);
    void print_answer(const AIResponse& response);
    void print_sources(const std::vector<Citation>& citations);
    void print_timing(const OSINTResult& result);

    // Colors
    std::string c(const std::string& code, const std::string& text) const;
    static bool is_command(const std::string& input);
    bool handle_command(const std::string& cmd);
};

} // namespace osint