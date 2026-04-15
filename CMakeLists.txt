cmake_minimum_required(VERSION 3.20)
project(osint_ai VERSION 1.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Venv Integration ─────────────────────────────────────────────────────────
# If the user sourced activate.sh, CMAKE_PREFIX_PATH already points to ./venv.
# Otherwise, load venv_deps.cmake explicitly.
set(VENV_DIR "${CMAKE_SOURCE_DIR}/venv")
if(EXISTS "${VENV_DIR}/venv_deps.cmake")
    include("${VENV_DIR}/venv_deps.cmake")
    message(STATUS "[venv] Loaded from ${VENV_DIR}/venv_deps.cmake")
else()
    message(WARNING "[venv] venv_deps.cmake not found — run setup_venv.sh first")
endif()

# ── Find packages (resolved from venv first via CMAKE_PREFIX_PATH) ────────────
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)

# ── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    "$<$<CONFIG:Release>:-O3>"
    "$<$<CONFIG:Debug>:-g;-O0;-fsanitize=address,undefined>"
)

add_link_options(
    "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
)

# ── Source files ──────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/core/HttpClient.cpp
    src/search/SearchEngine.cpp
    src/scraper/WebScraper.cpp
    src/ai/AIBrain.cpp
    src/osint/OSINTEngine.cpp
    src/ui/CLI.cpp
)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(osint_ai ${SOURCES})

target_include_directories(osint_ai PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(osint_ai PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    pthread
)

# ── Test executable ───────────────────────────────────────────────────────────
add_executable(osint_ai_tests
    tests/test_main.cpp
    src/core/HttpClient.cpp
    src/search/SearchEngine.cpp
    src/scraper/WebScraper.cpp
    src/ai/AIBrain.cpp
    src/osint/OSINTEngine.cpp
    src/ui/CLI.cpp
)

target_include_directories(osint_ai_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(osint_ai_tests PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    pthread
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS osint_ai DESTINATION bin)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════╗")
message(STATUS "║  osint-ai build configuration                ║")
message(STATUS "╠══════════════════════════════════════════════╣")
message(STATUS "║  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "║  C++ Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "║  Venv dir     : ${VENV_DIR}")
message(STATUS "║  CURL         : ${CURL_VERSION_STRING}")
message(STATUS "╚══════════════════════════════════════════════╝")
message(STATUS "")