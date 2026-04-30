# NOBODY — Web Intelligence Engine

**NOBODY** is a C++ OSINT-grade AI research tool that answers questions by searching the live internet in real-time. It uses no pre-trained datasets for answers — instead it queries **multiple search engines** (DuckDuckGo, Google, Brave, Bing, Wayback Machine), applies **Google Dorking** techniques for deeper discovery, scrapes and extracts content from top web pages **concurrently**, and synthesises a cited, factual response using a **local AI model running on your own machine via Ollama**. No external API keys required for core functionality.

```
 ███╗   ██╗ ██████╗ ██████╗  ██████╗ ██████╗ ██╗   ██╗
 ████╗  ██║██╔═══██╗██╔══██╗██╔═══██╗██╔══██╗╚██╗ ██╔╝
 ██╔██╗ ██║██║   ██║██████╔╝██║   ██║██║  ██║ ╚████╔╝
 ██║╚██╗██║██║   ██║██╔══██╗██║   ██║██║  ██║  ╚██╔╝
 ██║ ╚████║╚██████╔╝██████╔╝╚██████╔╝██████╔╝   ██║
 ╚═╝  ╚═══╝ ╚═════╝ ╚═════╝  ╚═════╝ ╚═════╝    ╚═╝
```

---

## How It Works

Every query goes through a multi-stage intelligence pipeline:

```
User Query
    │
    ▼
[1] Intent Classifier  ──► Detects query type (fact, person, news, technical, research)
    │                       Selects optimal search strategy and depth
    ▼
[2] Query Expander     ──► Google Dorking: generates site:, filetype:, intitle:,
    │                       date-filtered, and negative-filtered query variants
    ▼
[3] SearchEngine       ──► Multi-engine: DuckDuckGo HTML + Instant Answer,
    │                       Google CSE, Brave Search, Bing, Wayback Machine CDX
    │                       Reciprocal Rank Fusion + BM25 + Domain Authority scoring
    ▼
[4] WebScraper         ──► Concurrent scraping (thread pool), extracts clean text,
    │                       meta tags (author, date, description), JSON-LD structured
    │                       data, Wayback Machine fallback for dead/paywalled links
    ▼
[5] AIBrain            ──► Intent-specific system prompts, fact extraction,
    │                       map-reduce multi-pass synthesis for deep research,
    │                       conflict detection, domain citations
    ▼
CLI Output (coloured, cited, timed, with dork queries shown)
```

---

## Features

- **100% offline AI** — uses your local Ollama model (Llama 3, Phi-4, etc.). No cloud API keys needed.
- **Multi-engine search** — queries DuckDuckGo, Google CSE, Brave Search, Bing Web Search, and Wayback Machine simultaneously. Results fused via Reciprocal Rank Fusion.
- **Google Dorking** — auto-generates advanced search operators (`site:`, `filetype:`, `intitle:`, `inurl:`, date ranges) tailored to query intent. Negative filters exclude low-quality sources.
- **BM25 + Domain Authority ranking** — industry-standard text relevance scoring combined with tiered domain authority (Wikipedia, .edu, .gov boosted; Pinterest, Quora penalised) and freshness decay.
- **Concurrent web scraping** — fetches and extracts clean readable text from multiple pages in parallel using thread pools.
- **Rich content extraction** — meta tags (author, publish date, description), Open Graph data, JSON-LD structured data, and canonical URLs.
- **Wayback Machine fallback** — automatically fetches archived versions of pages that return 403/404 or are paywalled.
- **Intent classification** — automatically detects query type (fact lookup, person, news, technical, deep research) and adjusts search strategy, scraping depth, and AI synthesis mode accordingly.
- **URL-seeded research** — paste a URL to automatically scrape it, extract its core topic via NLP TF analysis, search the web for related information, and synthesize a comprehensive report.
- **Map-reduce AI synthesis** — for deep research queries, each scraped page is summarised individually (map), then all summaries are combined into a comprehensive answer (reduce). Overcomes context window limits.
- **Intent-specific AI prompts** — five tailored system prompts ensure optimal output for facts, research, people, news, and technical queries.
- **Fact extraction** — key factual sentences are extracted from sources and cross-referenced for confidence scoring.
- **Anti-hallucination** — the AI is only allowed to cite facts from the fetched web context. Conflicts between sources are flagged.
- **Cited answers** — sources listed by domain at the end of every response, with automatic fallback citation.
- **Scraper hardening** — 12 rotating User-Agent strings, retry with exponential backoff, per-domain rate limiting, gzip/deflate compression.
- **Depth modes** — `quick` (fast, fewer sources), `standard` (balanced), `deep` (maximum coverage, map-reduce AI).
- **Interactive REPL** — persistent conversation with history, or single-shot query mode.
- **Configurable** — all settings via `config.json` or environment variables.

---

## Dependencies

| Library | Purpose |
|---|---|
| `libcurl` | All HTTP requests (search, scraping, Ollama) |
| `nlohmann-json` | JSON parsing for APIs |
| `spdlog` | Structured logging |
| `fmt` | String formatting |
| `Ollama` | Local LLM inference (separate install) |

---

## Installation & Setup

### 1. Install System Libraries

```bash
sudo apt update
sudo apt install -y cmake build-essential libcurl4-openssl-dev nlohmann-json3-dev libfmt-dev libspdlog-dev
```

### 2. Install Ollama

Ollama runs open-source AI models locally on your machine.

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

### 3. Pull a Model

Pull at least one model. Recommended options:

| Model | Speed | Quality | Command |
|---|---|---|---|
| `llama3.1:latest` | Slow | Best | `ollama pull llama3.1` |
| `llama3.2:latest` | Medium | Great | `ollama pull llama3.2` |
| `phi4:latest` | Fast | Good | `ollama pull phi4` |
| `phi3.5:latest` | Fastest | Good | `ollama pull phi3.5` |

```bash
ollama pull llama3.1
```

### 4. Build NOBODY

Clone (or navigate to) the project directory and run the build script:

```bash
cd /path/to/Agent
chmod +x build.sh
./build.sh
```

The compiled binary will be at `./build/nobody`.

---

## Configuration

NOBODY reads settings from `config.json` in the project root. All values can be overridden with environment variables.

```json
{
  "ollama_model":   "llama3.1:latest",
  "google_api_key": "",
  "google_cx":      "",

  "search": {
    "max_results":             15,
    "use_duckduckgo":          true,
    "use_google":              false,
    "use_brave":               false,
    "use_bing":                false,
    "use_wayback":             true,
    "brave_api_key":           "",
    "bing_api_key":            "",
    "language":                "en",
    "enable_dorking":          true,
    "dork_queries_per_search": 3,
    "negative_domains":        ["pinterest.com","quora.com","ask.com","slideshare.net"]
  },

  "scraper": {
    "pages_to_scrape":       5,
    "max_text_length":       12000,
    "timeout_seconds":       20,
    "strip_boilerplate":     true,
    "max_concurrent":        4,
    "use_wayback_fallback":  true,
    "extract_meta":          true,
    "extract_structured_data": true
  },

  "ai": {
    "model":             "llama3.1:latest",
    "max_tokens":        2500,
    "temperature":       0.2,
    "max_context_chars": 24000,
    "synthesis_mode":    "standard"
  },

  "ui": {
    "color":               true,
    "show_search_results": true,
    "show_sources":        true,
    "show_timing":         true
  },

  "logging": {
    "level": "warn"
  }
}
```

---

## Running the Software

### Make sure Ollama is running first

```bash
ollama serve
```
*(Leave this running in a separate terminal, or run it in the background with `ollama serve &`)*

---

### Interactive Mode (REPL)

Starts a persistent chat session. Type questions and get answers. Use `/help` for commands.

```bash
./build/nobody
```

**In-session commands:**

| Command | Description |
|---|---|
| `/help` | Show available commands |
| `/clear` | Clear conversation history |
| `/history` | Show conversation history |
| `/sources on\|off` | Toggle source URL display |
| `/search on\|off` | Toggle web results display |
| `/timing on\|off` | Toggle timing info |
| `/depth quick\|std\|deep` | Set scraping depth mode |
| `/quit` or `/exit` | Exit NOBODY |

---

### Single Query Mode

Run a one-shot query and exit. Good for scripting.

```bash
./build/nobody -q "What is quantum entanglement?"
```

Or pass the query as a positional argument:

```bash
./build/nobody "Who founded SpaceX?"
```

---

### CLI Flags

| Flag | Description |
|---|---|
| `-q`, `--query <text>` | Run a single query non-interactively |
| `-n`, `--no-scrape` | Skip web page scraping (faster, snippets only) |
| `-s`, `--no-search-ui` | Hide the search results list |
| `-t`, `--no-timing` | Hide timing information |
| `-v`, `--verbose` | Enable debug logging (shows full pipeline) |
| `--no-color` | Disable ANSI colour output |
| `--depth <mode>` | Set depth: `quick`, `standard`, `deep` (default: standard) |
| `--dork` | Force Google Dorking for all queries |
| `--no-dork` | Disable Google Dorking |
| `-h`, `--help` | Show help message |

---

### Depth Modes

| Mode | Search Results | Pages Scraped | AI Synthesis | Use Case |
|---|---|---|---|---|
| `quick` | 5–8 | 1–2 | Single pass | Fast factual lookups |
| `standard` | 15 | 5 | Single pass + fact extraction | General purpose (default) |
| `deep` | 20+ | 8+ | Map-reduce multi-pass | Comprehensive research |

```bash
# Quick factual answer
./build/nobody --depth quick -q "Capital of France?"

# Deep research with maximum coverage
./build/nobody --depth deep -q "Impact of quantum computing on cryptography"
```

---

### Environment Variables

| Variable | Description |
|---|---|
| `OLLAMA_MODEL` | Override the model (e.g. `phi4:latest`) |
| `GOOGLE_API_KEY` | Google Custom Search API key (optional) |
| `GOOGLE_CX` | Google Custom Search Engine ID (optional) |
| `BRAVE_API_KEY` | Brave Search API key (optional, enables Brave engine) |
| `BING_API_KEY` | Bing Web Search API key (optional, enables Bing engine) |
| `NOBODY_LOG_LEVEL` | `debug` / `info` / `warn` / `error` |
| `NOBODY_NO_COLOR` | Set to any value to disable colours |
| `NOBODY_NO_SCRAPE` | Set to any value to disable scraping |

---

## Usage Examples

**General knowledge:**
```bash
./build/nobody -q "What is quantum entanglement?"
```

**Deep research with Google Dorking:**
```bash
./build/nobody --depth deep --dork -q "Explain the impact of AI on healthcare"
```

**Research a topic directly from a seed URL:**
```bash
./build/nobody "https://en.wikipedia.org/wiki/Quantum_computing"
```

**Fast mode (no scraping, snippets only):**
```bash
./build/nobody --no-scrape -q "Current population of India"
```

**Person lookup (deep):**
```bash
./build/nobody --depth deep -q "Who is Jensen Huang?"
```

**Use a faster local model:**
```bash
OLLAMA_MODEL=phi3.5:latest ./build/nobody -q "Explain black holes"
```

**Debug mode (see dorking queries, engine results, full pipeline):**
```bash
NOBODY_LOG_LEVEL=debug ./build/nobody --verbose -q "Who is Linus Torvalds?"
```

**Interactive session with multiple search engines enabled:**
```bash
GOOGLE_API_KEY=your_key GOOGLE_CX=your_cx BRAVE_API_KEY=your_brave_key ./build/nobody
```

---

## Known Behaviour

- **First query takes 30–120 seconds** — Ollama generates the full answer locally. Use `--depth quick`, `--no-scrape`, or a lighter model (`phi3.5`, `phi4`) for faster responses.
- **Deep mode takes 2–5x longer** — map-reduce synthesis makes multiple LLM calls (one per scraped page + one final synthesis). Worth it for research-grade answers.
- **DuckDuckGo is always used** by default. Google, Brave, and Bing are opt-in and require API keys.
- **Wayback Machine** is enabled by default (no API key needed) and provides archived fallback for dead links.
- **Rate limiting** is enabled by default (500ms between same-domain requests) to avoid IP bans.
- **User-Agent rotation** cycles through 12 realistic browser strings to avoid scraping blocks.
- **Conversation history** is maintained within the interactive REPL session (last 10 turns).
- If Ollama is not running, the software will show a clear error message and gracefully print the raw search results instead of crashing.

---

## Project Structure

```
Agent/
├── src/
│   ├── main.cpp                       # Entry point, wires all components
│   ├── core/
│   │   ├── HttpClient.h/.cpp          # libcurl wrapper, UA rotation, retry, rate limiting
│   ├── search/
│   │   ├── SearchEngine.h/.cpp        # Multi-engine search, Google Dorking, BM25, RRF
│   ├── scraper/
│   │   ├── WebScraper.h/.cpp          # Concurrent scraper, meta/JSON-LD extraction
│   ├── ai/
│   │   ├── AIBrain.h/.cpp             # Ollama API, map-reduce synthesis, fact extraction
│   ├── engine/
│   │   ├── IntelligenceEngine.h/.cpp  # Pipeline orchestrator, intent classifier, depth modes
│   └── ui/
│       ├── CLI.h/.cpp                 # Terminal REPL & output formatting
├── tests/
│   └── test_main.cpp                  # Unit tests
├── config.json                        # Default configuration
├── CMakeLists.txt                     # CMake build config
├── build.sh                           # One-shot build script
├── overview.md                        # High-level project overview
└── DOCUMENTATION.md                   # Detailed architecture documentation
```
