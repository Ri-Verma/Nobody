# Nobody Architecture & Documentation

This document describes the inner workings of the **Nobody** software suite, detailing how the project is structured, the data flow, and what each file specifically handles. 

## High-Level Architecture
**Nobody** is built as a pipeline consisting of five main blocks:
1. **Core Networking** (HTTP & Data Requests)
2. **Search Engine Handler** (Querying live web endpoints)
3. **Web Scraper** (Extracting plain text from raw web pages)
4. **AI Brain** (Prompting and LLM Response Synthesis)
5. **Intelligence Engine & UI** (Orchestration and User interaction)

The application handles everything chronologically: User Input $\rightarrow$ Query Web Search $\rightarrow$ Scrape Top Results $\rightarrow$ Pass Content to LLM $\rightarrow$ Format Output.

---

## File and Component Breakdown

### 1. The Entry Point
**`src/main.cpp`**
- Serves as the bootstrapping interface.
- Reads environment variables (such as `ANTHROPIC_API_KEY`) and optional configurations.
- Initializes logging (`spdlog`) and parses Command Line Arguments (like `--no-scrape` or `--no-color`).
- Instantiates all the major C++ objects (`HttpClient`, `SearchEngine`, `WebScraper`, `AIBrain`, `NobodyEngine`, and `CLI`) and explicitly wires their dependencies together using smart pointers.
- Launches either a single non-interactive query (`-q`) or hands control to the interactive `CLI` REPL.

### 2. Core (Low-Level Utilities)
**`src/core/HttpClient.h` & `src/core/HttpClient.cpp`**
- Function as the central HTTP requester for the entirety of the program.
- Provide a thread-safe object-oriented wrapper around `libcurl`.
- Handle everything from connection timeouts, URL encoding, HTTP GET/POST methods, redirects, and injecting User-Agents (e.g., `Nobody/1.0`).

### 3. Search Engine
**`src/search/SearchEngine.h` & `src/search/SearchEngine.cpp`**
- Converts user questions into programmatic search queries.
- Connects to Google Custom Search Engine (via REST API) and DuckDuckGo (via Instant Answer or HTML endpoint).
- Strips out duplicates and ranks the links via internal TF-IDF style relevance scoring based on snippet hits matching the user query.
- Returns a clean standardized `SearchResult` struct (containing `url`, `title`, and `snippet`).

### 4. Boilerplate & Page Scraping
**`src/scraper/WebScraper.h` & `src/scraper/WebScraper.cpp`**
- Bypasses API limitations by directly visiting the URLs returned by the `SearchEngine`.
- Requests HTML dumps of the pages and runs an algorithm to intelligently strip DOM elements, boilerplate (`<nav>`, `<footer>`, `<script>`), and styling. 
- Leaves behind pure contiguous textual content, clipping large texts based on `max_text_length` to preserve AI context windows. 

### 5. AI Synthesis
**`src/ai/AIBrain.h` & `src/ai/AIBrain.cpp`**
- Forms the "AI" component of the stack (communicating externally with LLMs, pre-configured generally to Anthropic's Claude API).
- Embeds the `SYSTEM_PROMPT` containing constraints telling the AI it's "Nobody" and enforcing anti-hallucination policies.
- Formats the scraped web contexts and search snippets alongside the user query, orchestrates the API payload, and tracks usage tokens/responses.
- Automatically extracts citations from the AI's generated response to attribute the original scraped domains.

### 6. The Orchestrator
**`src/engine/IntelligenceEngine.h` & `src/engine/IntelligenceEngine.cpp`**
- Houses the `NobodyEngine` class.
- Dictates the complete program lifecycle: 
  1. Asking `SearchEngine` for links.
  2. Passing top-scoring links to `WebScraper`.
  3. Waiting for scraped text. 
  4. Pushing all results to the `AIBrain` for final inference.
- Yields the final `AIResponse` object comprising textual answers, citations, and metadata.

### 7. Interface
**`src/ui/CLI.h` & `src/ui/CLI.cpp`**
- The visual loop handling `stdin`/`stdout`.
- Prettifies the standard output, enforcing ANSI color-coding for citations.
- Draws search timings and provides an elegant REPL (Read-Eval-Print Loop) for the user to continually prompt Nobody interactively.

---

## Build System & Initialization
- **`CMakeLists.txt` & `build.sh`**: Setup rules for CMake compilation. Since Nobody relies purely on robust system-wide dependencies (`nlohmann-json`, `libcurl`, `spdlog`), it uses these build files to locate the system `.so` libraries and link the resulting executable securely into the root directory.
- **`config.json`**: An optional dot-style setting file providing fallback keys and search depth configurations (e.g., number of models, toggle booleans for DDG/Google) in the absence of user-provided Environment Variables.

---

## Installation & Setup

Follow these steps to compile and run Nobody on a Linux system:

### 1. Install System Dependencies
Nobody relies on globally available C++ libraries instead of isolated virtual environments. Install them via your package manager (e.g., `apt` for Debian/Ubuntu):

```bash
sudo apt update
sudo apt install -y cmake build-essential libcurl4-openssl-dev nlohmann-json3-dev libfmt-dev libspdlog-dev
```

### 2. Configure API Keys
You need at least an Anthropic Core API key to synthesize answers. Google Custom Search is optional but recommended.
You can configure them by exporting environment variables:

```bash
export ANTHROPIC_API_KEY="sk-ant-..."
export GOOGLE_API_KEY="your-google-api-key"
export GOOGLE_CX="your-google-cx"
```
*(Alternatively, you can place these keys inside the `config.json` file in the root directory).*

### 3. Build the Software
Compile the source code using the provided bash script or standard CMake commands:

**Option A (Using build script):**
```bash
chmod +x build.sh
./build.sh
```

**Option B (Manual CMake):**
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 4. Running the Program
After compiling, the `nobody` binary will be available (usually in `build/` or installed in `bin/`).

**Interactive Mode (REPL):**
Starts an interactive terminal session where you can repeatedly query Nobody.
```bash
./nobody
```

**Single Query Mode:**
Executes a single search quickly without dropping into the interactive shell. Useful for scripting.
```bash
./nobody -q "What is the latest advancement in fusion energy?"
```

**Disable Scraping for Speed:**
To perform standard searches without fetching and extracting deep HTML, use the `--no-scrape` or `-n` flag.
```bash
./nobody --no-scrape -q "Current stock price of AAPL"
```
