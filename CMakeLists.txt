cmake_minimum_required(VERSION 3.20)
project(nobody VERSION 1.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Find packages (system-wide libraries) ────────────────────────────────────
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
    src/engine/IntelligenceEngine.cpp
    src/ui/CLI.cpp
)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(nobody ${SOURCES})

target_include_directories(nobody PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(nobody PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    pthread
)

# ── Test executable ───────────────────────────────────────────────────────────
add_executable(nobody_tests
    tests/test_main.cpp
    src/core/HttpClient.cpp
    src/search/SearchEngine.cpp
    src/scraper/WebScraper.cpp
    src/ai/AIBrain.cpp
    src/engine/IntelligenceEngine.cpp
    src/ui/CLI.cpp
)

target_include_directories(nobody_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(nobody_tests PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    fmt::fmt
    spdlog::spdlog
    pthread
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS nobody DESTINATION bin)

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════╗")
message(STATUS "║  nobody – Web Intelligence Engine             ║")
message(STATUS "╠══════════════════════════════════════════════╣")
message(STATUS "║  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "║  C++ Standard : C++${CMAKE_CXX_STANDARD}")
message(STATUS "║  CURL         : ${CURL_VERSION_STRING}")
message(STATUS "╚══════════════════════════════════════════════╝")
message(STATUS "")