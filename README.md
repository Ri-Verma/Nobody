# Nobody — Web Intelligence Engine

**Nobody** is a C++ AI research tool that answers questions by searching the live internet in real-time. It uses no pre-trained datasets for answers — instead it queries search engines (DuckDuckGo, Google), scrapes the top web pages, and synthesises a cited, factual response using a **local AI model running on your own machine via Ollama**. No external API keys required.

```
  ██████╗ ███████╗██╗███╗   ██╗████████╗       █████╗ ██╗
 ██╔═══██╗██╔════╝██║████╗  ██║╚══██╔══╝      ██╔══██╗██║
 ██║   ██║███████╗██║██╔██╗ ██║   ██║   █████╗███████║██║
 ██║   ██║╚════██║██║██║╚██╗██║   ██║   ╚════╝██╔══██║██║
 ╚██████╔╝███████║██║██║ ╚████║   ██║         ██║  ██║██║
  ╚═════╝ ╚══════╝╚═╝╚═╝  ╚═══╝   ╚═╝         ╚═╝  ╚═╝╚═╝
```

---

## How It Works

Every query goes through a 3-step pipeline:

```
User Query
    │
    ▼
[1] SearchEngine  ──► DuckDuckGo HTML + Instant Answer (+ Google CSE if configured)
    │                 Deduplicates & ranks results by TF-IDF relevance
    ▼
[2] WebScraper    ──► Fetches HTML of top URLs, strips boilerplate,
    │                 extracts clean article text (no regex stack-overflow issues)
    ▼
[3] AIBrain       ──► Bundles context + query → sends to local Ollama model
    │                 Parses answer & extracts domain citations
    ▼
CLI Output (coloured, cited, timed)
```

---

## Features

- **100% offline AI** — uses your local Ollama model (Llama 3, Phi-4, etc.). No cloud API keys needed.
- **Live web search** — queries DuckDuckGo by default; optionally Google Custom Search.
- **Smart web scraping** — fetches and extracts clean readable text from real pages.
- **Intent classification** — automatically detects query type (fact lookup, person, news, technical, deep research) and adjusts scraping depth accordingly.
- **Anti-hallucination** — the AI is only allowed to cite facts from the fetched web context.
- **Cited answers** — sources listed by domain at the end of every response.
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

### 4. Build Nobody

Clone (or navigate to) the project directory and run the build script:

```bash
cd /path/to/Agent
chmod +x build.sh
./build.sh
```

The compiled binary will be at `./build/nobody`.

---

## Configuration

Nobody reads settings from `config.json` in the project root. All values can be overridden with environment variables.

```json
{
  "ollama_model":   "llama3.1:latest",
  "google_api_key": "",
  "google_cx":      "",

  "search": {
    "max_results":    8,
    "use_duckduckgo": true,
    "use_google":     false,
    "language":       "en"
  },

  "scraper": {
    "pages_to_scrape":   3,
    "max_text_length":   6000,
    "timeout_seconds":   20,
    "strip_boilerplate": true
  },

  "ai": {
    "model":       "llama3.1:latest",
    "max_tokens":  1500,
    "temperature": 0.3
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
| `/quit` or `/exit` | Exit Nobody |

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
| `-h`, `--help` | Show help message |

---

### Environment Variables

| Variable | Description |
|---|---|
| `OLLAMA_MODEL` | Override the model (e.g. `phi4:latest`) |
| `GOOGLE_API_KEY` | Google Custom Search API key (optional) |
| `GOOGLE_CX` | Google Custom Search Engine ID (optional) |
| `NOBODY_LOG_LEVEL` | `debug` / `info` / `warn` / `error` |
| `NOBODY_NO_COLOR` | Set to any value to disable colours |
| `NOBODY_NO_SCRAPE` | Set to any value to disable scraping |

---

## Usage Examples

**General knowledge:**
```bash
./build/nobody -q "What is quantum entanglement?"
```

**Fast mode (no scraping, snippets only):**
```bash
./build/nobody --no-scrape -q "Current population of India"
```

**Use a faster local model:**
```bash
OLLAMA_MODEL=phi3.5:latest ./build/nobody -q "Explain black holes"
```

**Debug mode (see the full pipeline — search → scrape → AI):**
```bash
NOBODY_LOG_LEVEL=debug ./build/nobody -q "Who is Linus Torvalds?"
```

**Interactive session with Google also enabled:**
```bash
GOOGLE_API_KEY=your_key GOOGLE_CX=your_cx ./build/nobody
```

---

## Known Behaviour

- **First query takes 30–120 seconds** — Ollama generates the full answer locally. Use `--no-scrape` or a lighter model (`phi3.5`, `phi4`) for faster responses.
- **DuckDuckGo is always used** by default. Google is opt-in and requires API keys.
- **Conversation history** is maintained within the interactive REPL session (last 10 turns).
- If Ollama is not running, the software will show a clear error message and gracefully print the raw search results instead of crashing.

---

## Project Structure

```
Agent/
├── src/
│   ├── main.cpp                    # Entry point, wires all components
│   ├── core/
│   │   ├── HttpClient.h/.cpp       # Thread-safe libcurl wrapper
│   ├── search/
│   │   ├── SearchEngine.h/.cpp     # DuckDuckGo + Google search
│   ├── scraper/
│   │   ├── WebScraper.h/.cpp       # HTML fetcher & text extractor
│   ├── ai/
│   │   ├── AIBrain.h/.cpp          # Ollama API client & prompt builder
│   ├── engine/
│   │   ├── IntelligenceEngine.h/.cpp  # Pipeline orchestrator
│   └── ui/
│       ├── CLI.h/.cpp              # Terminal REPL & output formatting
├── tests/
│   └── test_main.cpp               # Unit tests
├── config.json                     # Default configuration
├── CMakeLists.txt                  # CMake build config
├── build.sh                        # One-shot build script
├── overview.md                     # High-level project overview
└── DOCUMENTATION.md                # Detailed architecture documentation
```
