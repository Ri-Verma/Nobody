#!/usr/bin/env bash
# =============================================================================
#  setup_venv.sh  —  C++ Virtual Environment for osint-ai
#  Mimics Python's `python -m venv venv` + `pip install -r requirements.txt`
#  All libraries are downloaded and installed LOCALLY inside ./venv/
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"
DEPS_CACHE="$VENV_DIR/.cache"
INSTALL_PREFIX="$VENV_DIR"
BUILD_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
log_info()    { echo -e "${CYAN}[VENV]${RESET} $*"; }
log_success() { echo -e "${GREEN}[VENV]${RESET} $*"; }
log_warn()    { echo -e "${YELLOW}[VENV]${RESET} $*"; }
log_error()   { echo -e "${RED}[VENV]${RESET} $*" >&2; }
log_section() { echo -e "\n${BOLD}${GREEN}━━━  $*  ━━━${RESET}\n"; }

# ── Dependency Versions ───────────────────────────────────────────────────────
NLOHMANN_JSON_VERSION="v3.11.3"
SPDLOG_VERSION="v1.13.0"
FMT_VERSION="10.2.1"
CURL_MIN_VERSION="7.68.0"

# ── Prerequisite check ────────────────────────────────────────────────────────
check_prerequisites() {
    log_section "Checking prerequisites"
    local missing=()
    for cmd in cmake git curl g++ pkg-config; do
        if command -v "$cmd" &>/dev/null; then
            log_success "$cmd found → $(command -v $cmd)"
        else
            log_error "$cmd NOT found"
            missing+=("$cmd")
        fi
    done

    # Check libcurl-dev (system level — needed for CURL::libcurl in CMake)
    if pkg-config --exists libcurl 2>/dev/null; then
        CURL_VER=$(pkg-config --modversion libcurl)
        log_success "libcurl-dev found → $CURL_VER"
    else
        log_warn "libcurl-dev not found via pkg-config"
        log_warn "Install it with:  sudo apt install libcurl4-openssl-dev  (Debian/Ubuntu)"
        log_warn "                  sudo dnf install libcurl-devel          (Fedora/RHEL)"
        log_warn "                  brew install curl                       (macOS)"
        missing+=("libcurl-dev")
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        log_error "Missing prerequisites: ${missing[*]}"
        log_error "Please install them and re-run this script."
        exit 1
    fi

    CMAKE_VER=$(cmake --version | head -1 | awk '{print $3}')
    log_success "All prerequisites satisfied (cmake $CMAKE_VER, jobs=$BUILD_JOBS)"
}

# ── Create venv skeleton ──────────────────────────────────────────────────────
create_venv_skeleton() {
    log_section "Creating venv directory skeleton"
    mkdir -p "$VENV_DIR"/{include,lib,bin,share,lib/pkgconfig,.cache/src}
    log_success "Created: $VENV_DIR"
    log_info  "  ├── include/   ← header files"
    log_info  "  ├── lib/       ← compiled static/shared libraries"
    log_info  "  ├── bin/       ← binaries"
    log_info  "  ├── share/     ← cmake config files"
    log_info  "  └── .cache/    ← downloaded source tarballs"
}

# ── Helper: clone or update a git repo into cache ────────────────────────────
fetch_source() {
    local name="$1" url="$2" tag="$3"
    local src_dir="$DEPS_CACHE/src/$name"
    if [ -d "$src_dir/.git" ]; then
        log_info "$name already fetched — skipping download"
    else
        log_info "Cloning $name @ $tag ..."
        git clone --depth=1 --branch "$tag" "$url" "$src_dir"
        log_success "$name source ready"
    fi
    echo "$src_dir"
}

# ── Install nlohmann/json (header-only) ───────────────────────────────────────
install_nlohmann_json() {
    log_section "Installing nlohmann/json $NLOHMANN_JSON_VERSION (header-only)"
    local src_dir
    src_dir=$(fetch_source "nlohmann_json" \
        "https://github.com/nlohmann/json.git" "$NLOHMANN_JSON_VERSION")

    local build_dir="$DEPS_CACHE/build/nlohmann_json"
    mkdir -p "$build_dir"
    cmake -S "$src_dir" -B "$build_dir" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DJSON_BuildTests=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DJSON_Install=ON \
        -Wno-dev -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF > /dev/null 2>&1
    cmake --install "$build_dir" > /dev/null 2>&1
    log_success "nlohmann/json installed → $INSTALL_PREFIX/include/nlohmann/"
}

# ── Install fmt ───────────────────────────────────────────────────────────────
install_fmt() {
    log_section "Installing fmt $FMT_VERSION"
    local src_dir
    src_dir=$(fetch_source "fmt" \
        "https://github.com/fmtlib/fmt.git" "$FMT_VERSION")

    local build_dir="$DEPS_CACHE/build/fmt"
    mkdir -p "$build_dir"
    cmake -S "$src_dir" -B "$build_dir" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DFMT_TEST=OFF \
        -DFMT_DOC=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -Wno-dev > /dev/null 2>&1
    cmake --build "$build_dir" -j"$BUILD_JOBS" > /dev/null 2>&1
    cmake --install "$build_dir" > /dev/null 2>&1
    log_success "fmt installed → $INSTALL_PREFIX/lib/libfmt.a"
}

# ── Install spdlog ────────────────────────────────────────────────────────────
install_spdlog() {
    log_section "Installing spdlog $SPDLOG_VERSION"
    local src_dir
    src_dir=$(fetch_source "spdlog" \
        "https://github.com/gabime/spdlog.git" "$SPDLOG_VERSION")

    local build_dir="$DEPS_CACHE/build/spdlog"
    mkdir -p "$build_dir"
    cmake -S "$src_dir" -B "$build_dir" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DSPDLOG_BUILD_EXAMPLE=OFF \
        -DSPDLOG_BUILD_TESTS=OFF \
        -DSPDLOG_FMT_EXTERNAL=ON \
        -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -Wno-dev > /dev/null 2>&1
    cmake --build "$build_dir" -j"$BUILD_JOBS" > /dev/null 2>&1
    cmake --install "$build_dir" > /dev/null 2>&1
    log_success "spdlog installed → $INSTALL_PREFIX/lib/libspdlog.a"
}

# ── Write activation marker & activate script ─────────────────────────────────
write_activation_marker() {
    log_section "Finalising virtual environment"
    cat > "$VENV_DIR/.activated" << EOF
OSINT_AI_VENV=1
VENV_DIR=$VENV_DIR
VENV_CREATED=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
DEPS=nlohmann_json,fmt,spdlog,libcurl
EOF
    log_success "Venv marker written to $VENV_DIR/.activated"

    # Generate pkg-config summary
    cat > "$VENV_DIR/venv_deps.cmake" << EOF
# Auto-generated by setup_venv.sh — DO NOT EDIT
set(VENV_DIR "$VENV_DIR")
set(VENV_INCLUDE_DIR "$VENV_DIR/include")
set(VENV_LIB_DIR "$VENV_DIR/lib")
set(CMAKE_PREFIX_PATH "$VENV_DIR" \${CMAKE_PREFIX_PATH})
EOF
    log_success "CMake integration written to $VENV_DIR/venv_deps.cmake"
}

# ── Summary ───────────────────────────────────────────────────────────────────
print_summary() {
    echo ""
    echo -e "${BOLD}${GREEN}╔══════════════════════════════════════════════════════╗${RESET}"
    echo -e "${BOLD}${GREEN}║      C++ Virtual Environment Ready!  ✓               ║${RESET}"
    echo -e "${BOLD}${GREEN}╠══════════════════════════════════════════════════════╣${RESET}"
    echo -e "${BOLD}${GREEN}║${RESET}  Location : $VENV_DIR"
    echo -e "${BOLD}${GREEN}║${RESET}  Libraries: nlohmann/json, fmt, spdlog, libcurl (sys)"
    echo -e "${BOLD}${GREEN}║${RESET}"
    echo -e "${BOLD}${GREEN}║${RESET}  Next steps:"
    echo -e "${BOLD}${GREEN}║${RESET}    source activate.sh      # activate the venv"
    echo -e "${BOLD}${GREEN}║${RESET}    ./build.sh              # build the project"
    echo -e "${BOLD}${GREEN}║${RESET}    ./build/osint_ai        # run"
    echo -e "${BOLD}${GREEN}╚══════════════════════════════════════════════════════╝${RESET}"
}

# ── Main ──────────────────────────────────────────────────────────────────────
main() {
    echo -e "${BOLD}${CYAN}"
    echo "  ██████╗ ███████╗██╗███╗   ██╗████████╗       █████╗ ██╗"
    echo " ██╔═══██╗██╔════╝██║████╗  ██║╚══██╔══╝      ██╔══██╗██║"
    echo " ██║   ██║███████╗██║██╔██╗ ██║   ██║   █████╗███████║██║"
    echo " ██║   ██║╚════██║██║██║╚██╗██║   ██║   ╚════╝██╔══██║██║"
    echo " ╚██████╔╝███████║██║██║ ╚████║   ██║         ██║  ██║██║"
    echo "  ╚═════╝ ╚══════╝╚═╝╚═╝  ╚═══╝   ╚═╝         ╚═╝  ╚═╝╚═╝"
    echo -e "${RESET}"
    echo -e "${CYAN}  C++ Virtual Environment Setup${RESET}"
    echo -e "${CYAN}  Equivalent to: python -m venv venv && pip install -r requirements.txt${RESET}"
    echo ""

    check_prerequisites
    create_venv_skeleton
    install_nlohmann_json
    install_fmt
    install_spdlog
    write_activation_marker
    print_summary
}

main "$@"